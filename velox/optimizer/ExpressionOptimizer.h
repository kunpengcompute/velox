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

#include <memory>
#include <vector>

#include "velox/core/Expressions.h"
#include "velox/optimizer/CostModel.h"
#include "velox/optimizer/EGraph.h"
#include "velox/optimizer/RewriteRule.h"

namespace facebook::velox::core {
class QueryCtx;
}

namespace facebook::velox::optimizer {

/// Applies explicitly registered rewrites and extracts the cheapest expression.
/// An optimizer without registered rules returns the original expressions.
class ExpressionOptimizer {
 public:
  explicit ExpressionOptimizer(
      const EGraphConfig& config = EGraphConfig{},
      const core::QueryCtx* queryCtx = nullptr);

  /// False for the default empty pass or a process-wide disabled optimizer.
  bool hasActiveRules() const;

  core::TypedExprPtr optimize(const core::TypedExprPtr& expr);

  std::vector<core::TypedExprPtr> optimize(
      const std::vector<core::TypedExprPtr>& exprs);

  void registerRule(std::unique_ptr<RewriteRule> rule);

  const CostModel& costModel() const {
    return costModel_;
  }

 private:
  EGraphConfig config_;
  const core::QueryCtx* queryCtx_;
  CostModel costModel_;
  std::vector<std::unique_ptr<RewriteRule>> rules_;
};

} // namespace facebook::velox::optimizer
