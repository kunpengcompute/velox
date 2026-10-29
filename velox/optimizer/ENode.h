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

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "velox/common/base/Exceptions.h"
#include "velox/core/Expressions.h"
#include "velox/type/Type.h"

namespace facebook::velox::optimizer {

/// Represents a single expression node in the e-graph.
/// Unlike the expression tree where nodes point to other nodes directly,
/// e-nodes point to EClass IDs (equivalence classes containing their children).
/// This enables sharing: many different expressions can share the same child
/// e-class.
struct ENode {
  /// Classification of expression node types.
  enum class Kind {
    Constant, // Literal value
    FieldAccess, // Column reference
    Call, // Function call or operator
    Switch, // CASE WHEN expression
    If, // IF expression
    And, // Logical AND
    Or, // Logical OR
    Cast, // CAST expression
    Concat, // Row concatenation
    Lambda, // Lambda expression
    Opaque, // Uncertified tree, retained with occurrence identity
    Input, // Input row reference; only occurs under a column reference
    Dereference, // Positional struct access; name stores the decimal index
  };

  Kind kind;
  TypePtr type;
  // Call: function name; FieldAccess: field name; Dereference: decimal index.
  std::string name;
  std::vector<uint32_t> children; // EClass IDs of child expressions
  std::optional<variant> constantValue; // For Constant nodes (scalar values)
  core::TypedExprPtr originalExpr; // Vector-backed constant or opaque tree

  ENode() = delete;

  ENode(Kind k, TypePtr t, std::string n, std::vector<uint32_t> c)
      : kind(k),
        type(std::move(t)),
        name(std::move(n)),
        children(std::move(c)) {}

  /// Compute a hash value for this e-node.
  size_t hash() const;

  /// Equality comparison for hash-consing.
  bool operator==(const ENode& other) const;

  /// Create an ENode from a Velox TypedExpr.
  /// @param expr The typed expression to convert.
  /// @param exprToEClass Mapping from ITypedExpr* to EClassId for child
  /// expressions.
  /// @param original Optional occurrence identity for vector-backed constants.
  /// When omitted, an owned ConstantTypedExpr wrapper retains the value vector.
  static ENode fromTypedExpr(
      const core::ITypedExpr& expr,
      const std::unordered_map<const core::ITypedExpr*, uint32_t>& exprToEClass,
      const core::TypedExprPtr& original = nullptr);
};

/// Hash functor for ENode.
struct ENodeHash {
  size_t operator()(const ENode& node) const {
    return node.hash();
  }
};

} // namespace facebook::velox::optimizer
