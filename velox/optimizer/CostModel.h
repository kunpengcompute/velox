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
#include <memory>
#include <string>
#include <vector>

#include "velox/optimizer/ENode.h"

namespace facebook::velox::optimizer {

class EGraph;
class CostSelection;

/// A finite, selected representative and the exact contextual children priced
/// with it. Sharing is by witness, never solely by e-class across CASE depths.
struct CostWitness {
  ENode node;
  double cost;
  std::vector<std::shared_ptr<const CostWitness>> children;
  size_t nodeIndex;
  int depth;
};

/// Cost model for evaluating the execution cost of expression nodes.
///
/// Key design decisions:
/// 1. Recursive estimates count CASE/IF nesting, not arbitrary AST depth.
///    AND/OR and ordinary calls retain the current depth. Selection memoizes
///    by (e-class, depth) only for subgraphs that cannot reach a cycle.
/// 2. Dead branch elimination: if a condition is known TRUE (tautology),
///    subsequent branches and ELSE have zero cost, including branch overhead.
/// 3. A fixed overhead per reachable CASE branch favors compact structures.
/// 4. Selectivity-weighted evaluation: more selective conditions first
///    reduces the expected number of condition evaluations.
class CostModel {
 public:
  CostModel() = default;

  double computeCost(const ENode& node, const EGraph& graph) const;

  /// Choose a finite witness excluding repeated e-classes on a path. Cyclic
  /// contexts are searched without memo reuse. Child selection precedes parent
  /// selectivity/discounts, so this is a heuristic, not a global optimum proof.
  /// A 100,000-candidate budget aborts the
  /// entire selection on exhaustion, returning nullptr rather than a partial
  /// best choice. Scalar EClass costs are diagnostics, not selection inputs.
  std::shared_ptr<const CostWitness> selectBest(
      uint32_t classId,
      const EGraph& graph) const;

  double estimateSelectivity(const ENode& node) const;

  static double baseCost(ENode::Kind kind, const std::string& name);

 private:
  friend class CostSelection;
  double selectedCost(
      const ENode& node,
      int depth,
      const std::vector<std::shared_ptr<const CostWitness>>& children) const;
};

} // namespace facebook::velox::optimizer
