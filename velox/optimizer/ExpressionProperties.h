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
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "velox/core/Expressions.h"

namespace facebook::velox::optimizer {

/// The embedding compiler must check the metadata of the resolved signature,
/// including its return type. Missing metadata is not a proof of determinism.
using FunctionDeterminismCheck = std::function<
    bool(const std::string&, const std::vector<TypePtr>&, const TypePtr&)>;

/// Optimizer identity is stricter than SQL numeric equality: +0 and -0,
/// and different NaN payloads, remain distinct literal representations.
size_t hashConstantValueExactly(const variant& value);
bool constantValuesEqualExactly(const variant& left, const variant& right);
/// Transitive stability: volatile/unknown calls and all their ancestors fail.
/// Built-in lazy special forms derive stability from every child.
bool isExpressionDeterministic(
    const core::TypedExprPtr& expr,
    const FunctionDeterminismCheck& check = {});

} // namespace facebook::velox::optimizer
