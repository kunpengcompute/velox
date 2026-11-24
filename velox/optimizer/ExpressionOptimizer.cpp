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

#include "velox/optimizer/ExpressionOptimizer.h"

#include "velox/common/base/Exceptions.h"
#include "velox/core/QueryCtx.h"

#include <iostream>
#include <utility>

#include <gflags/gflags.h>

DEFINE_bool(
    velox_optimizer_enabled,
    true,
    "Enable explicitly registered E-Graph expression rewrites.");
DEFINE_bool(
    velox_optimizer_dump_changes,
    false,
    "Print before/after trees when the expression optimizer changes them.");

namespace facebook::velox::optimizer {

ExpressionOptimizer::ExpressionOptimizer(
    const EGraphConfig& config,
    const core::QueryCtx* queryCtx)
    : config_(config), queryCtx_(queryCtx) {}

bool ExpressionOptimizer::hasActiveRules() const {
  return FLAGS_velox_optimizer_enabled && !rules_.empty();
}

void ExpressionOptimizer::registerRule(std::unique_ptr<RewriteRule> rule) {
  rules_.push_back(std::move(rule));
}

core::TypedExprPtr ExpressionOptimizer::optimize(
    const core::TypedExprPtr& expr) {
  if (!expr || !hasActiveRules()) {
    return expr;
  }
  // Unknown functions and lambdas, including their parents, stay opaque.
  if (!isExpressionDeterministic(expr, config_.functionDeterminismCheck)) {
    return expr;
  }

  EGraph graph(config_);
  const auto root = graph.addExpression(expr);
  const auto revision = graph.revision();
  const auto added = graph.applyRules(rules_);
  // A node-budget fallback is not an equivalence proof.
  if (graph.overflowed() || graph.revision() == revision) {
    return expr;
  }
  core::TypedExprPtr result;
  try {
    result = graph.extractBest(root, costModel_);
  } catch (const VeloxUserError& error) {
    // An unimplemented reconstruction may decline an optional rewrite.
    // Rule/metadata callbacks execute outside this catch; resource errors,
    // cancellation and invariant failures must keep their original meaning.
    if (error.errorCode() != error_code::kUnsupported.c_str()) {
      throw;
    }
    return expr;
  }
  if (!result) {
    return expr;
  }
  if (FLAGS_velox_optimizer_dump_changes || config_.debugLog ||
      (queryCtx_ && queryCtx_->queryConfig().optimizerDumpChanges())) {
    const auto before = expr->toString();
    const auto after = result->toString();
    if (before != after) {
      std::cerr << "[Velox Optimizer] Expression optimized (" << added
                << " new nodes):\n  BEFORE: " << before
                << "\n  AFTER: " << after << std::endl;
    }
  }
  return result;
}

std::vector<core::TypedExprPtr> ExpressionOptimizer::optimize(
    const std::vector<core::TypedExprPtr>& exprs) {
  std::vector<core::TypedExprPtr> result;
  result.reserve(exprs.size());
  for (const auto& expr : exprs) {
    result.push_back(optimize(expr));
  }
  return result;
}

} // namespace facebook::velox::optimizer
