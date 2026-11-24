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
#include "velox/optimizer/ExpressionProperties.h"

#include <cstring>
#include <functional>
#include <unordered_map>

namespace facebook::velox::optimizer {
namespace {

template <typename T>
uint64_t floatingBits(T value) {
  uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(value));
  return bits;
}

bool isLazySpecialForm(const std::string& name) {
  return name == "and" || name == "or" || name == "if" || name == "switch" ||
      name == "coalesce" || name == "try";
}

bool deterministicImpl(
    const core::TypedExprPtr& expr,
    const FunctionDeterminismCheck& check,
    std::unordered_map<const core::ITypedExpr*, bool>& memo,
    std::vector<TypePtr>& inputTypes) {
  if (!expr) {
    return true;
  }
  if (auto it = memo.find(expr.get()); it != memo.end()) {
    return it->second;
  }
  for (const auto& input : expr->inputs()) {
    if (!deterministicImpl(input, check, memo, inputTypes)) {
      memo[expr.get()] = false;
      return false;
    }
  }

  bool stable = false;
  if (const auto* call = dynamic_cast<const core::CallTypedExpr*>(expr.get())) {
    if (isLazySpecialForm(call->name())) {
      stable = true;
    } else if (check) {
      // Children are complete before the callback: one traversal-local
      // buffer can serve every signature without per-call allocations.
      inputTypes.clear();
      inputTypes.reserve(call->inputs().size());
      for (const auto& input : call->inputs()) {
        if (!input || !input->type()) {
          memo[expr.get()] = false;
          return false;
        }
        inputTypes.push_back(input->type());
      }
      stable = check(call->name(), inputTypes, call->type());
    }
  } else {
    stable = dynamic_cast<const core::ConstantTypedExpr*>(expr.get()) ||
        dynamic_cast<const core::FieldAccessTypedExpr*>(expr.get()) ||
        dynamic_cast<const core::DereferenceTypedExpr*>(expr.get()) ||
        dynamic_cast<const core::InputTypedExpr*>(expr.get()) ||
        dynamic_cast<const core::CastTypedExpr*>(expr.get()) ||
        dynamic_cast<const core::ConcatTypedExpr*>(expr.get());
  }
  memo[expr.get()] = stable;
  return stable;
}

} // namespace

size_t hashConstantValueExactly(const variant& value) {
  if (!value.isNull()) {
    if (value.kind() == TypeKind::REAL) {
      return std::hash<uint64_t>{}(floatingBits(value.value<float>()));
    }
    if (value.kind() == TypeKind::DOUBLE) {
      return std::hash<uint64_t>{}(floatingBits(value.value<double>()));
    }
  }
  return value.hash();
}

bool constantValuesEqualExactly(const variant& left, const variant& right) {
  if (left.kind() != right.kind() || left.isNull() != right.isNull()) {
    return false;
  }
  if (!left.isNull()) {
    if (left.kind() == TypeKind::REAL) {
      return floatingBits(left.value<float>()) ==
          floatingBits(right.value<float>());
    }
    if (left.kind() == TypeKind::DOUBLE) {
      return floatingBits(left.value<double>()) ==
          floatingBits(right.value<double>());
    }
  }
  return left == right;
}

bool isExpressionDeterministic(
    const core::TypedExprPtr& expr,
    const FunctionDeterminismCheck& check) {
  std::unordered_map<const core::ITypedExpr*, bool> memo;
  std::vector<TypePtr> inputTypes;
  return deterministicImpl(expr, check, memo, inputTypes);
}

} // namespace facebook::velox::optimizer
