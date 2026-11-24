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
#include <string>

namespace facebook::velox::optimizer {

class EGraph;

/// Abstract base class for rewrite rules.
///
/// Each rule searches for patterns in the e-graph and adds equivalent
/// expressions (e-nodes) to the relevant e-classes. The e-graph engine
/// handles canonicalization, merging, and saturation.
class RewriteRule {
 public:
  virtual ~RewriteRule() = default;

  /// Human-readable name for debugging and logging.
  virtual std::string name() const = 0;

  /// Apply the rule to all e-classes in the graph.
  /// For each e-class found matching the rule's pattern, add equivalent
  /// e-nodes to that e-class.
  ///
  /// @param graph The e-graph to operate on.
  /// @return The number of new e-nodes added.
  /// A rule may return zero while merging existing classes. The engine detects
  /// graph mutations independently when deciding whether to continue rewriting.
  virtual size_t apply(EGraph& graph) = 0;
};

} // namespace facebook::velox::optimizer
