/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/optimizer/CostModel.h"
#include "velox/optimizer/EGraph.h"

#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace facebook::velox::optimizer {

namespace {

/// Check if a type kind is an integral numeric kind.
bool isIntegralKind(TypeKind kind) {
  return kind == TypeKind::TINYINT || kind == TypeKind::SMALLINT ||
      kind == TypeKind::INTEGER || kind == TypeKind::BIGINT;
}

} // namespace

// ---- Base costs ----

double CostModel::baseCost(ENode::Kind kind, const std::string& name) {
  switch (kind) {
    case ENode::Kind::Constant:
      return 1.0;
    case ENode::Kind::Input:
      // The compiler skips this structural child of a column reference.
      return 0.0;
    case ENode::Kind::Dereference:
    case ENode::Kind::FieldAccess:
      return 2.0;
    case ENode::Kind::And:
    case ENode::Kind::Or:
      return 2.0;
    case ENode::Kind::Cast:
      // CastTypedExpr maps to Kind::Cast with name "cast"/"try_cast"
      // (ENode::fromTypedExpr). The try form adds a per-row
      // error-suppression branch, so it prices slightly above plain cast.
      return name == "try_cast" ? 6.0 : 5.0;
    case ENode::Kind::Call: {
      if (name == "=" || name == ">" || name == "<" || name == ">=" ||
          name == "<=" || name == "<>" || name == "!=")
        return 3.0;
      // Velox-internal comparison names (Gluten/Substrait, Spark SQL plans).
      // Gluten plans use these names exclusively; without this entry every
      // comparison fell through to the 10.0 default, which made extraction
      // prefer e.g. between(x, x, y) (6.0) over a single
      // lessthanorequal(x, y) (3.0 + children) — the degenerate range
      // rewrite then always won over the cheaper single comparison.
      if (name == "equalto" || name == "notequalto" || name == "lessthan" ||
          name == "greaterthan" || name == "lessthanorequal" ||
          name == "greaterthanorequal") {
        return 3.0;
      }
      if (name == "+" || name == "-" || name == "*" || name == "/" ||
          name == "%" || name == "add" || name == "subtract" ||
          name == "multiply" || name == "divide" || name == "modulus" ||
          name == "pmod")
        return 4.0;
      // Defensive duplicate of the Kind::Cast case above: try_cast arrives
      // as Kind::Cast (priced there); this covers a hypothetical future
      // rule that synthesizes a Call-form try_cast, keeping pricing
      // consistent either way.
      if (name == "try_cast")
        return 6.0;
      if (name == "in")
        return 8.0;
      if (name == "between")
        return 4.0;
      if (name == "like" || name == "rlike" || name == "regexp" ||
          name == "regexp_replace" || name == "regexp_extract")
        return 15.0;
      if (name == "not")
        return 1.0;
      if (name == "coalesce" || name == "if" || name == "nvl")
        return 4.0;
      if (name == "nullif")
        return 3.0;
      if (name == "get_json_object")
        return 20.0;
      if (name == "concat" || name == "concat_ws")
        return 6.0;
      if (name == "substr" || name == "substring" || name == "substring_index")
        return 8.0;
      if (name == "replace" || name == "trim" || name == "lower" ||
          name == "upper" || name == "reverse" || name == "repeat" ||
          name == "rpad" || name == "lpad")
        return 8.0;
      if (name == "length" || name == "instr" || name == "locate")
        return 5.0;
      if (name == "split")
        return 12.0;
      if (name == "round" || name == "floor" || name == "ceil" ||
          name == "ceiling" || name == "abs" || name == "sqrt" ||
          name == "pow" || name == "power" || name == "ln" || name == "bin" ||
          name == "rand")
        return 6.0;
      if (name == "date_add" || name == "date_sub" || name == "datediff" ||
          name == "months_between" || name == "from_unixtime" ||
          name == "unix_timestamp" || name == "to_date" || name == "year" ||
          name == "month" || name == "day" || name == "hour" ||
          name == "dayofmonth")
        return 8.0;
      if (name == "array_contains")
        return 8.0;
      if (name == "str_to_map")
        return 12.0;
      if (name == "least" || name == "greatest")
        return 5.0;
      if (name == "isnull" || name == "is_null")
        return 2.0;
      if (name == "isnotnull" || name == "is_not_null")
        return 2.0;
      if (name == "format_number")
        return 10.0;
      return 10.0; // Default function cost.
    }
    case ENode::Kind::Switch:
    case ENode::Kind::If:
      return 5.0; // Base overhead — increased from 3.0 to reflect real cost.
    case ENode::Kind::Concat:
      return 3.0;
    case ENode::Kind::Lambda:
      return 15.0;
    default:
      return 5.0;
  }
}

// ---- Selectivity ----

static double operatorSelectivity(const std::string& name) {
  if (name == "=" || name == "equalto")
    return 0.10;
  if (name == "in")
    return 0.20;
  if (name == "between")
    return 0.25;
  if (name == ">" || name == "<" || name == ">=" || name == "<=" ||
      name == "greaterthan" || name == "lessthan" ||
      name == "greaterthanorequal" || name == "lessthanorequal") {
    return 0.30;
  }
  if (name == "like" || name == "rlike" || name == "regexp")
    return 0.50;
  if (name == "!=" || name == "<>" || name == "notequalto")
    return 0.90;
  if (name == "isnull" || name == "is_null")
    return 0.05;
  if (name == "isnotnull" || name == "is_not_null")
    return 0.95;
  return 0.30;
}

double CostModel::estimateSelectivity(const ENode& node) const {
  switch (node.kind) {
    case ENode::Kind::Call:
      return operatorSelectivity(node.name);
    case ENode::Kind::And: {
      double sel = 1.0;
      for (size_t i = 0; i < node.children.size(); ++i)
        sel *= 0.3;
      return sel;
    }
    case ENode::Kind::Or: {
      double sel = 1.0;
      for (size_t i = 0; i < node.children.size(); ++i)
        sel *= 0.7;
      return 1.0 - sel;
    }
    case ENode::Kind::Switch:
    case ENode::Kind::If:
      return 0.5;
    case ENode::Kind::Constant:
      return 1.0;
    case ENode::Kind::FieldAccess:
      return 0.5;
    default:
      return 0.5;
  }
}

// Selection and pricing share one finite witness, including metadata used by
// selectivity and special discounts. Never price from mutable EClass hints.
class CostSelection {
 public:
  using Witness = std::shared_ptr<const CostWitness>;
  CostSelection(const CostModel& model, const EGraph& graph)
      : model_(model),
        graph_(graph),
        state_(graph.numClasses()),
        unsafe_(graph.numClasses()) {
    for (uint32_t id = 0; id < graph_.numClasses(); ++id) {
      if (graph_.find(id) == id) {
        reachesCycle(id);
      }
    }
  }

  Witness selectClass(uint32_t id, int depth) {
    id = graph_.find(id);
    if (active_.count(id)) {
      return nullptr;
    }
    const uint64_t key = (uint64_t{id} << 32) | uint32_t(depth);
    if (!unsafe_[id]) {
      if (auto it = memo_.find(key); it != memo_.end()) {
        return it->second;
      }
    }
    active_.insert(id);
    Witness best;
    const auto& nodes = graph_.getClass(id).nodes();
    for (size_t i = 0; i < nodes.size() && !exhausted_; ++i) {
      auto candidate = selectNode(nodes[i], depth, i);
      if (candidate && (!best || candidate->cost < best->cost)) {
        best = std::move(candidate);
      }
    }
    active_.erase(id);
    if (!unsafe_[id] && !exhausted_) {
      memo_[key] = best;
    }
    return exhausted_ ? nullptr : best;
  }

  Witness fixedNode(const ENode& node) {
    // The legacy node-pricing API may receive a node copied from its class.
    // Block that root class too, so its own back edge is not expanded as a
    // deceptively cheap different representative of the same class.
    for (uint32_t id = 0; id < graph_.numClasses(); ++id) {
      if (graph_.find(id) != id) {
        continue;
      }
      for (const auto& existing : graph_.getClass(id).nodes()) {
        if (existing == node) {
          active_.insert(id);
        }
      }
    }
    return selectNode(node, 0, 0);
  }

 private:
  bool reachesCycle(uint32_t id) {
    id = graph_.find(id);
    if (state_[id] == 1) {
      return true;
    }
    if (state_[id] == 2) {
      return unsafe_[id];
    }
    state_[id] = 1;
    bool unsafe = false;
    for (const auto& node : graph_.getClass(id).nodes()) {
      for (auto child : node.children) {
        const bool childUnsafe = reachesCycle(child);
        unsafe = unsafe || childUnsafe;
      }
    }
    state_[id] = 2;
    return unsafe_[id] = unsafe;
  }

  Witness selectNode(const ENode& node, int depth, size_t index) {
    if (exhausted_ || remaining_ == 0) {
      exhausted_ = true;
      return nullptr;
    }
    --remaining_;
    const int childDepth = depth +
        (node.kind == ENode::Kind::Switch || node.kind == ENode::Kind::If);
    std::vector<Witness> children;
    children.reserve(node.children.size());
    // Even a zero-priced dead arm must have a finite structural witness:
    // reconstruction still needs its child expression.
    for (auto id : node.children) {
      auto child = selectClass(id, childDepth);
      if (!child) {
        return nullptr;
      }
      children.push_back(std::move(child));
    }
    const double cost = model_.selectedCost(node, depth, children);
    if (!std::isfinite(cost)) {
      return nullptr;
    }
    return std::make_shared<CostWitness>(
        CostWitness{node, cost, std::move(children), index, depth});
  }

  const CostModel& model_;
  const EGraph& graph_;
  std::vector<uint8_t> state_;
  std::vector<bool> unsafe_;
  std::unordered_set<uint32_t> active_;
  std::unordered_map<uint64_t, Witness> memo_;
  size_t remaining_ = 100000;
  bool exhausted_ = false;
};

std::shared_ptr<const CostWitness> CostModel::selectBest(
    uint32_t classId,
    const EGraph& graph) const {
  if (graph.overflowed()) {
    return nullptr;
  }
  return CostSelection(*this, graph).selectClass(classId, 0);
}

double CostModel::computeCost(const ENode& node, const EGraph& graph) const {
  if (graph.overflowed()) {
    return std::numeric_limits<double>::infinity();
  }
  const auto witness = CostSelection(*this, graph).fixedNode(node);
  return witness ? witness->cost : std::numeric_limits<double>::infinity();
}

double CostModel::selectedCost(
    const ENode& node,
    int depth,
    const std::vector<std::shared_ptr<const CostWitness>>& children) const {
  const double penalty = 1.0 + depth * 0.5; // CASE/IF nesting, not AST depth.
  if (node.kind == ENode::Kind::Switch || node.kind == ENode::Kind::If) {
    double cost = baseCost(node.kind, node.name) * penalty;
    double remaining = 1.0;
    bool knownTrue = false;
    const size_t cases = children.size() / 2;
    for (size_t i = 0; i < cases && !knownTrue; ++i) {
      const auto& condition = *children[2 * i];
      const auto& value = condition.node.constantValue;
      knownTrue = condition.node.kind == ENode::Kind::Constant && value &&
          !value->isNull() && value->kind() == TypeKind::BOOLEAN &&
          value->value<bool>();
      const double selectivity =
          knownTrue ? 1.0 : estimateSelectivity(condition.node);
      cost += 1.0 + remaining * condition.cost;
      cost += remaining * selectivity * children[2 * i + 1]->cost;
      remaining *= 1.0 - selectivity;
    }
    if (!knownTrue) {
      cost += remaining * (children.size() % 2 ? children.back()->cost : 0.5);
    }
    return cost;
  }
  if (node.kind == ENode::Kind::And || node.kind == ENode::Kind::Or) {
    double cost = baseCost(node.kind, node.name) * penalty;
    double remaining = 1.0;
    for (const auto& child : children) {
      cost += remaining * child->cost;
      const double selectivity = estimateSelectivity(child->node);
      remaining *=
          node.kind == ENode::Kind::And ? selectivity : 1 - selectivity;
    }
    return cost;
  }
  if (node.kind == ENode::Kind::Call && node.name == "coalesce" &&
      children.size() == 2 && node.type && isIntegralKind(node.type->kind())) {
    const auto& first = *children[0];
    if (first.node.kind == ENode::Kind::Cast && first.children.size() == 1 &&
        first.children[0]->node.type &&
        first.children[0]->node.type->kind() == TypeKind::BOOLEAN) {
      return first.children[0]->cost + 1.0 + children[1]->cost;
    }
  }
  double cost = baseCost(node.kind, node.name);
  for (const auto& child : children) {
    cost += child->cost;
  }
  return cost;
}

} // namespace facebook::velox::optimizer
