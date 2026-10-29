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
#include <optional>
#include <utility>
#include <vector>

#include "velox/optimizer/ENode.h"
#include "velox/type/Type.h"

namespace facebook::velox::optimizer {

/// An equivalence class: a set of equivalent ENodes that all produce the same
/// result. Each e-class has a unique ID and a type.
class EClass {
 public:
  explicit EClass(uint32_t id, TypePtr type)
      : id_(id), type_(std::move(type)) {}

  uint32_t id() const {
    return id_;
  }

  const TypePtr& type() const {
    return type_;
  }

  const std::vector<ENode>& nodes() const {
    return nodes_;
  }

  /// Add a new e-node to this equivalence class.
  /// Returns true if the node was added (false if it was a duplicate).
  bool addNode(const ENode& node);

  /// Add a new e-node to this equivalence class (move version).
  bool addNode(ENode&& node);

  /// Invalidate the cached cost (called when the class changes).
  void invalidateCost() {
    cachedCost_.reset();
    bestNodeIndex_ = 0; // Valid fallback, not a newly computed minimum.
  }

  /// Diagnostic depth-zero cost snapshot from the last extraction. Contextual
  /// extraction uses its own costs and never treats this as a universal
  /// minimum.
  std::optional<double> cachedCost() const {
    return cachedCost_;
  }

  /// Set the cached cost.
  void setCachedCost(double cost) const {
    cachedCost_ = cost;
  }

  /// Diagnostic selected index when cachedCost() is present; otherwise a
  /// fallback.
  size_t bestNodeIndex() const {
    return bestNodeIndex_;
  }

  void setBestNodeIndex(size_t idx) const {
    bestNodeIndex_ = idx;
  }

 private:
  uint32_t id_;
  TypePtr type_;
  std::vector<ENode> nodes_;
  mutable std::optional<double> cachedCost_;
  mutable size_t bestNodeIndex_ = 0;
};

} // namespace facebook::velox::optimizer
