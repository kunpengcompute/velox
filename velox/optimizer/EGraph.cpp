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

#include "velox/optimizer/EGraph.h"
#include "velox/optimizer/CostModel.h"
#include "velox/optimizer/RewriteRule.h"

#include <folly/Conv.h>

#include <algorithm>
#include <deque>
#include <functional>
#include <unordered_set>
#include <utility>

namespace facebook::velox::optimizer {

namespace {

core::TypedExprPtr reconstructTypedExprImpl(
    const ENode& node,
    std::vector<core::TypedExprPtr> childExprs) {
  switch (node.kind) {
    case ENode::Kind::Constant: {
      if (node.constantValue.has_value()) {
        return std::make_shared<core::ConstantTypedExpr>(
            node.type, node.constantValue.value());
      }
      if (node.originalExpr) {
        // Vector-backed constant (e.g., ARRAY, MAP) — use the original.
        if (auto* origConst = dynamic_cast<const core::ConstantTypedExpr*>(
                node.originalExpr.get())) {
          if (origConst->hasValueVector()) {
            return std::make_shared<core::ConstantTypedExpr>(
                origConst->valueVector());
          }
        }
        // Fallback: return the original expression as-is.
        return node.originalExpr;
      }
      // No value and no original — this shouldn't happen in practice,
      // but create a null constant as a defensive fallback.
      return std::make_shared<core::ConstantTypedExpr>(
          node.type, variant::null(node.type->kind()));
    }

    case ENode::Kind::Input:
      VELOX_CHECK(childExprs.empty());
      return std::make_shared<core::InputTypedExpr>(node.type);

    case ENode::Kind::Dereference:
      VELOX_CHECK_EQ(childExprs.size(), 1);
      return std::make_shared<core::DereferenceTypedExpr>(
          node.type, childExprs[0], folly::to<uint32_t>(node.name));

    case ENode::Kind::FieldAccess: {
      if (childExprs.empty()) {
        // Top-level column reference.
        return std::make_shared<core::FieldAccessTypedExpr>(
            node.type, node.name);
      }
      // Struct field dereference.
      return std::make_shared<core::FieldAccessTypedExpr>(
          node.type, childExprs[0], node.name);
    }

    case ENode::Kind::Call:
    case ENode::Kind::Switch:
    case ENode::Kind::If:
    case ENode::Kind::And:
    case ENode::Kind::Or: {
      return std::make_shared<core::CallTypedExpr>(
          node.type, std::move(childExprs), node.name);
    }

    case ENode::Kind::Cast: {
      VELOX_CHECK_GE(childExprs.size(), 1);
      bool nullOnFailure = (node.name == "try_cast");
      return std::make_shared<core::CastTypedExpr>(
          node.type, childExprs[0], nullOnFailure);
    }

    case ENode::Kind::Concat: {
      VELOX_CHECK(node.type && node.type->isRow());
      VELOX_CHECK_EQ(node.type->size(), childExprs.size());
      return std::make_shared<core::ConcatTypedExpr>(
          node.type->asRow().names(), std::move(childExprs));
    }

    case ENode::Kind::Opaque:
      VELOX_CHECK_NOT_NULL(node.originalExpr);
      return node.originalExpr;

    case ENode::Kind::Lambda: {
      // The pipeline handles this explicitly unsupported reconstruction.
      VELOX_UNSUPPORTED("Lambda reconstruction not yet supported");
    }
  }

  VELOX_UNREACHABLE("Unknown ENode kind");
  return nullptr;
}

core::TypedExprPtr materializeWitness(
    const std::shared_ptr<const CostWitness>& witness,
    std::unordered_map<const CostWitness*, core::TypedExprPtr>& cache) {
  if (auto it = cache.find(witness.get()); it != cache.end()) {
    return it->second;
  }
  std::vector<core::TypedExprPtr> children;
  children.reserve(witness->children.size());
  for (const auto& child : witness->children) {
    children.push_back(materializeWitness(child, cache));
  }
  // Selection already proved the witness finite. Reconstruct only that
  // witness: unsupported or malformed unselected alternatives cannot throw.
  auto result = reconstructTypedExprImpl(witness->node, std::move(children));
  cache.emplace(witness.get(), result);
  return result;
}

} // namespace

EGraph::EGraph(const EGraphConfig& config)
    : config_(config), parents_(), totalNodes_(0) {}

uint32_t EGraph::createClass(TypePtr type) {
  uint32_t id = classes_.size();
  classes_.emplace_back(id, std::move(type));
  parents_.push_back(id);
  uses_.emplace_back();
  queued_.push_back(false);
  countedSizes_.push_back(0);
  invalidatedAt_.push_back(0);
  return id;
}

uint32_t EGraph::find(uint32_t id) const {
  while (parents_[id] != id) {
    parents_[id] = parents_[parents_[id]]; // Path compression
    id = parents_[id];
  }
  return id;
}

uint32_t EGraph::canonicalize(const ENode& node) {
  // Rebuilding may replace the e-class that owns the argument.
  ENode canonical = node;
  if (needsRebuild_ && !overflowed_) {
    rebuild();
  }
  if (overflowed_) {
    return 0;
  }
  // First, canonicalize children by finding their union-find roots.
  for (auto& child : canonical.children) {
    child = find(child);
  }

  // Look up in hash-cons table.
  size_t h = canonical.hash();
  auto it = hashCons_.find(h);
  if (it != hashCons_.end()) {
    for (const auto& entry : it->second) {
      if (entry.node == canonical) {
        return find(entry.eclassId);
      }
    }
  }

  // Not found — create a new e-class.
  if (isFull() || config_.maxNodesPerClass == 0) {
    overflowed_ = true;
    return 0; // Only meaningful together with overflowed().
  }

  uint32_t newId = createClass(canonical.type);
  classes_[newId].addNode(std::move(canonical));
  totalNodes_++;
  countedSizes_[newId] = 1;
  ++revision_;

  // Register in hash-cons table.
  hashCons_[h].push_back({classes_[newId].nodes().back(), newId});
  rememberUses(newId);

  return newId;
}

void EGraph::merge(uint32_t id1, uint32_t id2) {
  ++invalidationEpoch_;
  if (needsRebuild_ && !overflowed_) {
    rebuild();
  }
  if (overflowed_) {
    return;
  }
  if (mergeClasses(id1, id2)) {
    rebuild();
  }
}

void EGraph::invalidateCosts(const std::vector<uint32_t>& roots) {
  std::vector<uint32_t> pending = roots;
  while (!pending.empty()) {
    const auto id = find(pending.back());
    pending.pop_back();
    if (invalidatedAt_[id] == invalidationEpoch_) {
      continue;
    }
    invalidatedAt_[id] = invalidationEpoch_;
    classes_[id].invalidateCost();
    pending.insert(pending.end(), uses_[id].begin(), uses_[id].end());
  }
}

void EGraph::enqueue(uint32_t id) {
  id = find(id);
  if (!queued_[id]) {
    queued_[id] = true;
    work_.push_back(id);
  }
  needsRebuild_ = true;
}

void EGraph::rememberUses(uint32_t id) {
  for (const auto& node : classes_[id].nodes()) {
    for (auto child : node.children) {
      uses_[find(child)].insert(id);
    }
  }
}

void EGraph::detachClass(uint32_t id) {
  for (const auto& node : classes_[id].nodes()) {
    auto it = hashCons_.find(node.hash());
    if (it != hashCons_.end()) {
      auto& bucket = it->second;
      bucket.erase(
          std::remove_if(
              bucket.begin(),
              bucket.end(),
              [&](const HashEntry& entry) { return entry.eclassId == id; }),
          bucket.end());
      if (bucket.empty()) {
        hashCons_.erase(it);
      }
    }
    for (auto child : node.children) {
      uses_[find(child)].erase(id);
    }
  }
}

bool EGraph::mergeClasses(uint32_t id1, uint32_t id2) {
  id1 = find(id1);
  id2 = find(id2);
  if (id1 == id2) {
    return false;
  }
  if (classes_[id1].nodes().size() < classes_[id2].nodes().size()) {
    std::swap(id1, id2);
  }
  // Build the complete union before committing it. A budget failure must
  // never discard alternatives or publish a partial equivalence class.
  EClass merged(id1, classes_[id1].type());
  for (auto id : {id1, id2}) {
    for (auto node : classes_[id].nodes()) {
      for (auto& child : node.children) {
        child = find(child);
        if (child == id2) {
          child = id1;
        }
      }
      merged.addNode(std::move(node));
      if (merged.nodes().size() > config_.maxNodesPerClass) {
        overflowed_ = true;
        return false;
      }
    }
  }
  std::vector<uint32_t> affected(uses_[id1].begin(), uses_[id1].end());
  affected.insert(affected.end(), uses_[id2].begin(), uses_[id2].end());
  invalidateCosts({id1, id2});
  detachClass(id1);
  detachClass(id2);
  totalNodes_ -= countedSizes_[id1] + countedSizes_[id2];
  countedSizes_[id1] = merged.nodes().size();
  countedSizes_[id2] = 0;
  totalNodes_ += countedSizes_[id1];
  parents_[id2] = id1;
  classes_[id1] = std::move(merged);
  classes_[id2] = EClass(id2, classes_[id2].type());
  uses_[id1].insert(uses_[id2].begin(), uses_[id2].end());
  uses_[id2].clear();
  rememberUses(id1);
  for (auto parent : affected) {
    enqueue(parent);
  }
  enqueue(id1);
  ++revision_;
  return true;
}

void EGraph::rebuild() {
  // Account all dirty classes together: a growth and a shrink may offset each
  // other at the global budget. queued_ makes canonical root entries unique.
  for (auto id : work_) {
    if (find(id) == id) {
      const auto size = classes_[id].nodes().size();
      totalNodes_ = totalNodes_ - countedSizes_[id] + size;
      countedSizes_[id] = size;
    }
  }
  if (totalNodes_ > config_.maxTotalNodes) {
    overflowed_ = true;
    return;
  }
  while (!work_.empty() && !overflowed_) {
    auto id = work_.front();
    work_.pop_front();
    queued_[id] = false;
    if (find(id) != id) {
      enqueue(id);
      continue;
    }
    // Never trim alternatives to meet a budget.
    const auto size = classes_[id].nodes().size();
    if (size > config_.maxNodesPerClass) {
      overflowed_ = true;
      return;
    }
    detachClass(id);
    EClass canonicalClass(id, classes_[id].type());
    for (auto node : classes_[id].nodes()) {
      for (auto& child : node.children) {
        child = find(child);
      }
      canonicalClass.addNode(std::move(node));
    }
    classes_[id] = std::move(canonicalClass);
    totalNodes_ -= size - classes_[id].nodes().size();
    countedSizes_[id] = classes_[id].nodes().size();
    rememberUses(id);
    std::vector<uint32_t> congruent;
    for (const auto& node : classes_[id].nodes()) {
      auto& bucket = hashCons_[node.hash()];
      for (const auto& entry : bucket) {
        if (entry.node == node && find(entry.eclassId) != id) {
          congruent.push_back(entry.eclassId);
          break;
        }
      }
      bucket.push_back({node, id});
    }
    for (auto other : congruent) {
      id = find(id);
      other = find(other);
      if (id == other) {
        continue;
      }
      if (!mergeClasses(id, other)) {
        return; // A class-budget failure makes the graph unextractable.
      }
    }
  }
  needsRebuild_ = false;
}

uint32_t EGraph::addExpression(const core::TypedExprPtr& expr) {
  VELOX_CHECK_NOT_NULL(expr);
  if (!isExpressionDeterministic(expr, config_.functionDeterminismCheck)) {
    // Treat the WHOLE tree as opaque. Independent volatile occurrences and
    // their parents must never become common semantic classes. No rule can
    // descend into this node; extraction preserves the original tree.
    ENode opaque(ENode::Kind::Opaque, expr->type(), "", {});
    opaque.originalExpr = expr;
    return canonicalize(opaque);
  }

  // Add children before parents, memoizing each original expression pointer.
  std::unordered_map<const core::ITypedExpr*, uint32_t> exprToEClass;

  std::function<uint32_t(const core::TypedExprPtr&)> addExpr =
      [&](const core::TypedExprPtr& e) -> uint32_t {
    // Check if already processed (CSE).
    auto it = exprToEClass.find(e.get());
    if (it != exprToEClass.end()) {
      return it->second;
    }

    // Add children first (bottom-up).
    for (const auto& input : e->inputs()) {
      addExpr(input);
    }

    // Create the e-node.
    ENode node = ENode::fromTypedExpr(*e, exprToEClass, e);
    uint32_t id = canonicalize(node);
    exprToEClass[e.get()] = id;
    return id;
  };

  return addExpr(expr);
}

size_t EGraph::applyRules(
    const std::vector<std::unique_ptr<RewriteRule>>& rules,
    int maxIterations) {
  int iterations = maxIterations > 0 ? maxIterations : config_.maxIterations;
  size_t totalAdded = 0;

  for (int i = 0; i < iterations; ++i) {
    if (needsRebuild_ && !overflowed_) {
      rebuild();
    }
    const auto before = revision_;
    size_t addedThisIteration = 0;

    for (const auto& rule : rules) {
      if (overflowed_) {
        break;
      }
      addedThisIteration += rule->apply(*this);
      if (needsRebuild_ && !overflowed_) {
        rebuild();
      }
    }

    totalAdded += addedThisIteration;

    if (overflowed_ || revision_ == before) {
      // Merging existing classes can enable rules without adding any e-nodes.
      break;
    }
  }

  return totalAdded;
}

core::TypedExprPtr EGraph::extractBest(
    uint32_t eclassId,
    const CostModel& costModel) {
  if (needsRebuild_ && !overflowed_) {
    rebuild();
  }
  if (overflowed_) {
    return nullptr;
  }
  eclassId = find(eclassId);
  const auto& eclass = classes_[eclassId];

  if (eclass.nodes().empty()) {
    return nullptr;
  }

  const auto witness = costModel.selectBest(eclassId, *this);
  if (!witness) {
    return nullptr;
  }
  // Preserve scalar diagnostics for depth-zero selected witnesses. They are
  // never consulted by selection, which also tracks CASE depth and path.
  std::unordered_set<const CostWitness*> visited;
  std::function<void(uint32_t, const std::shared_ptr<const CostWitness>&)>
      record = [&](uint32_t id, const auto& selected) {
        if (!visited.insert(selected.get()).second) {
          return;
        }
        if (selected->depth == 0) {
          const auto& selectedClass = classes_[find(id)];
          selectedClass.setCachedCost(selected->cost);
          selectedClass.setBestNodeIndex(selected->nodeIndex);
        }
        for (size_t i = 0; i < selected->children.size(); ++i) {
          record(selected->node.children[i], selected->children[i]);
        }
      };
  record(eclassId, witness);
  std::unordered_map<const CostWitness*, core::TypedExprPtr> cache;
  return materializeWitness(witness, cache);
}

} // namespace facebook::velox::optimizer
