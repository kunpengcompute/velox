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
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "velox/core/Expressions.h"
#include "velox/optimizer/EClass.h"
#include "velox/optimizer/ENode.h"
#include "velox/optimizer/ExpressionProperties.h"
#include "velox/type/Type.h"
#include "velox/type/Variant.h"

namespace facebook::velox::optimizer {

// Forward declarations.
class RewriteRule;
class CostModel;

/// Configuration for the e-graph optimizer.
struct EGraphConfig {
  /// Maximum number of rewrite iterations.
  int maxIterations = 10;

  /// Maximum number of e-nodes per e-class (to prevent unbounded growth).
  size_t maxNodesPerClass = 10;

  /// Maximum total number of e-nodes in the graph.
  size_t maxTotalNodes = 20000;

  /// Whether to enable detailed logging.
  bool debugLog = false;

  /// Resolved-signature determinism from the embedding compiler. Without a
  /// witness, ordinary calls and their dependent trees remain opaque.
  FunctionDeterminismCheck functionDeterminismCheck;
};

/// Stores equivalent expressions using hash-consing and congruence closure.
/// Rewrite rules add alternatives; extraction selects and reconstructs by cost.
class EGraph {
 public:
  explicit EGraph(const EGraphConfig& config = EGraphConfig{});

  /// Add a Velox typed expression to the e-graph.
  /// Returns the ID of the e-class containing the root of the expression.
  uint32_t addExpression(const core::TypedExprPtr& expr);

  /// Add a single e-node to the graph and return its e-class ID.
  /// This is the canonicalization step: if the e-node already exists in the
  /// graph, the existing e-class ID is returned.
  uint32_t canonicalize(const ENode& node);

  /// Merge two e-classes, making them equivalent.
  /// After merging, all e-nodes that referenced either e-class will be
  /// re-canonicalized (congruence closure).
  void merge(uint32_t id1, uint32_t id2);

  /// Apply a set of rewrite rules for a fixed number of iterations.
  /// Returns the total number of new e-nodes added.
  size_t applyRules(
      const std::vector<std::unique_ptr<RewriteRule>>& rules,
      int maxIterations = -1);

  /// Select and materialize one finite cost witness, including CASE depth.
  /// Cycles exclude repeated e-classes on a path; budget exhaustion returns
  /// nullptr. See CostModel::selectBest() for the cyclic search contract.
  core::TypedExprPtr extractBest(uint32_t eclassId, const CostModel& costModel);

  /// Get the number of e-classes in the graph.
  size_t numClasses() const {
    return classes_.size();
  }

  /// Get the total number of e-nodes in the graph.
  size_t numNodes() const {
    return totalNodes_;
  }

  /// Changes on node insertion and successful union. Mutable exposure also
  /// conservatively marks a change, since callers can replace the whole class.
  uint64_t revision() const {
    return revision_;
  }

  /// A node budget was exhausted. Fallback IDs are not equivalence proofs;
  /// callers must abandon optimization, and extraction returns nullptr.
  bool overflowed() const {
    return overflowed_;
  }

  /// Read a storage slot; merged alias slots are empty. Use find(id) for its
  /// root.
  const EClass& getClass(uint32_t id) const {
    VELOX_CHECK_LT(id, classes_.size());
    return classes_[id];
  }

  /// Get the canonical class and schedule repair before the next graph
  /// operation. The reference must not be retained across other graph
  /// mutations.
  EClass& getClassMutable(uint32_t id) {
    VELOX_CHECK_LT(id, classes_.size());
    id = find(id);
    ++invalidationEpoch_;
    invalidateCosts({id});
    detachClass(id);
    enqueue(id);
    ++revision_;
    return classes_[id];
  }

  /// Find the canonical representative of an e-class (union-find).
  uint32_t find(uint32_t id) const;

  /// Get the configuration.
  const EGraphConfig& config() const {
    return config_;
  }

 private:
  /// Create a new e-class and return its ID.
  uint32_t createClass(TypePtr type);

  /// Check if the total node limit has been exceeded.
  bool isFull() const {
    return totalNodes_ >= config_.maxTotalNodes;
  }

  /// Repair only queued classes and propagate congruence through reverse uses.
  void rebuild();
  bool mergeClasses(uint32_t id1, uint32_t id2);
  void enqueue(uint32_t id);
  void detachClass(uint32_t id);
  void rememberUses(uint32_t id);
  void invalidateCosts(const std::vector<uint32_t>& roots);

  EGraphConfig config_;

  /// All e-classes, indexed by EClassId.
  std::vector<EClass> classes_;

  /// Union-find parent array for tracking merged e-classes.
  mutable std::vector<uint32_t> parents_;

  /// Persistent reverse edges and repair work; unrelated classes stay
  /// untouched.
  std::vector<std::unordered_set<uint32_t>> uses_;
  std::deque<uint32_t> work_;
  std::vector<bool> queued_;
  std::vector<size_t> countedSizes_;
  std::vector<uint64_t> invalidatedAt_;
  uint64_t invalidationEpoch_ = 0;
  uint64_t revision_ = 0;

  /// Total number of e-nodes across all e-classes.
  size_t totalNodes_ = 0;

  /// Set when the node-limit fallback in canonicalize() was taken at least
  /// once; extraction from such a graph is unsound (see overflowed()).
  bool overflowed_ = false;
  bool needsRebuild_ = false;

  /// Hash-cons table: maps ENode hash to (ENode, EClassId) pairs.
  /// Used for canonicalization: given an e-node, find if it already exists.
  struct HashEntry {
    ENode node;
    uint32_t eclassId;
  };
  std::unordered_map<size_t, std::vector<HashEntry>> hashCons_;
};

} // namespace facebook::velox::optimizer
