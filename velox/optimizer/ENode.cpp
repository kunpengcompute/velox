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

#include "velox/optimizer/ENode.h"
#include "velox/common/base/BitUtil.h"
#include "velox/optimizer/ExpressionProperties.h"

#include <memory>

namespace facebook::velox::optimizer {

namespace {

ENode::Kind kindFromString(const std::string& name) {
  if (name == "switch" || name == "if") {
    // Both "switch" and "if" map to Switch — they are semantically
    // equivalent in the e-graph. Substrait uses "if" for multi-branch
    // CASE WHEN, while the DuckDB parser uses "switch".
    return ENode::Kind::Switch;
  }
  if (name == "and") {
    return ENode::Kind::And;
  }
  if (name == "or") {
    return ENode::Kind::Or;
  }
  return ENode::Kind::Call;
}

} // namespace

size_t ENode::hash() const {
  size_t h =
      bits::hashMix(static_cast<size_t>(kind), type ? type->hashKind() : 0);
  h = bits::hashMix(h, std::hash<std::string>()(name));
  for (auto child : children) {
    h = bits::hashMix(h, child);
  }
  if (constantValue.has_value()) {
    h = bits::hashMix(h, hashConstantValueExactly(*constantValue));
  }
  if (originalExpr) {
    h = bits::hashMix(h, reinterpret_cast<size_t>(originalExpr.get()));
  }
  return h;
}

bool ENode::operator==(const ENode& other) const {
  if (kind != other.kind || name != other.name ||
      children.size() != other.children.size()) {
    return false;
  }
  for (size_t i = 0; i < children.size(); ++i) {
    if (children[i] != other.children[i])
      return false;
  }
  if (constantValue.has_value() != other.constantValue.has_value())
    return false;
  if (constantValue.has_value() &&
      !constantValuesEqualExactly(
          constantValue.value(), other.constantValue.value()))
    return false;
  if (originalExpr != other.originalExpr)
    return false; // nullptr-safe, conservative
  if (type != other.type && (!type || !other.type || *type != *other.type)) {
    return false;
  }
  return true;
}

ENode ENode::fromTypedExpr(
    const core::ITypedExpr& expr,
    const std::unordered_map<const core::ITypedExpr*, uint32_t>& exprToEClass,
    const core::TypedExprPtr& original) {
  // Map child expressions to their e-class IDs.
  std::vector<uint32_t> childIds;
  childIds.reserve(expr.inputs().size());
  for (const auto& input : expr.inputs()) {
    auto it = exprToEClass.find(input.get());
    VELOX_CHECK(
        it != exprToEClass.end(),
        "Child expression not found in e-class mapping");
    childIds.push_back(it->second);
  }

  // Handle each expression type.
  if (dynamic_cast<const core::InputTypedExpr*>(&expr)) {
    return ENode(ENode::Kind::Input, expr.type(), "", std::move(childIds));
  }

  if (auto* constant = dynamic_cast<const core::ConstantTypedExpr*>(&expr)) {
    ENode node(ENode::Kind::Constant, expr.type(), "", std::move(childIds));
    if (constant->hasValueVector()) {
      // Vector-backed constant — can't extract the value as a variant.
      // Retain the supplied occurrence identity, or own a wrapper sharing
      // the value vector when only an ITypedExpr reference was provided.
      node.originalExpr = original
          ? original
          : std::make_shared<core::ConstantTypedExpr>(constant->valueVector());
    } else {
      node.constantValue = constant->value();
    }
    return node;
  }

  if (auto* fieldAccess =
          dynamic_cast<const core::FieldAccessTypedExpr*>(&expr)) {
    return ENode(
        ENode::Kind::FieldAccess,
        expr.type(),
        fieldAccess->name(),
        std::move(childIds));
  }

  if (auto* dereference =
          dynamic_cast<const core::DereferenceTypedExpr*>(&expr)) {
    return ENode(
        ENode::Kind::Dereference,
        expr.type(),
        // Keep the positional index distinct from a field name.
        std::to_string(dereference->index()),
        std::move(childIds));
  }

  if (auto* call = dynamic_cast<const core::CallTypedExpr*>(&expr)) {
    auto kind = kindFromString(call->name());
    return ENode(kind, expr.type(), call->name(), std::move(childIds));
  }

  if (auto* cast = dynamic_cast<const core::CastTypedExpr*>(&expr)) {
    return ENode(
        ENode::Kind::Cast,
        expr.type(),
        cast->nullOnFailure() ? "try_cast" : "cast",
        std::move(childIds));
  }

  if (auto* concat = dynamic_cast<const core::ConcatTypedExpr*>(&expr)) {
    return ENode(
        ENode::Kind::Concat, expr.type(), "concat", std::move(childIds));
  }

  if (auto* lambda = dynamic_cast<const core::LambdaTypedExpr*>(&expr)) {
    return ENode(
        ENode::Kind::Lambda, expr.type(), "lambda", std::move(childIds));
  }

  VELOX_UNSUPPORTED("Unknown expression type: {}", expr.toString());
}

} // namespace facebook::velox::optimizer
