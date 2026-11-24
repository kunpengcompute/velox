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
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "velox/optimizer/CostModel.h"
#include "velox/optimizer/EGraph.h"
#include "velox/optimizer/ExpressionProperties.h"
#include "velox/optimizer/tests/OptimizerTestConfig.h"

using namespace facebook::velox;
using namespace facebook::velox::core;
using namespace facebook::velox::optimizer;

namespace {
TypedExprPtr call(
    const std::string& name,
    std::vector<TypedExprPtr> children,
    const TypePtr& type = BOOLEAN()) {
  return std::make_shared<CallTypedExpr>(type, std::move(children), name);
}
TypedExprPtr field(const std::string& name, const TypePtr& type = BOOLEAN()) {
  return std::make_shared<FieldAccessTypedExpr>(type, name);
}
TypedExprPtr number(int32_t value) {
  return std::make_shared<ConstantTypedExpr>(INTEGER(), variant(value));
}

double recursiveCost(const TypedExprPtr& expression) {
  EGraph graph(testGraphConfig());
  const auto root = graph.addExpression(expression);
  // No extraction first: these checks exercise the recursive CASE-depth
  // contract, independently of cached standalone subtree estimates.
  return CostModel{}.computeCost(graph.getClass(root).nodes()[0], graph);
}

void checkCostAndExtraction(EGraph& graph, uint32_t root) {
  const auto node = graph.getClass(graph.find(root)).nodes()[0];
  const double cost = CostModel{}.computeCost(node, graph);
  EXPECT_TRUE(std::isfinite(cost));
  EXPECT_GT(cost, 0);
  EXPECT_NE(graph.extractBest(root, CostModel{}), nullptr);
}
} // namespace

TEST(CostModelGuardWalkTest, nestedAndSwitchCompletesWithoutOtherRules) {
  withOptimizerWatchdog([] {
    EGraph graph(testGraphConfig());
    const auto left =
        call("greaterthan", {field("left", INTEGER()), number(0)});
    const auto right =
        call("lessthan", {field("right", INTEGER()), number(100)});
    auto expression = call(
        "switch",
        {call("and", {left, right}),
         number(1),
         call("and", {right, left}),
         number(2),
         number(3)},
        INTEGER());
    checkCostAndExtraction(graph, graph.addExpression(expression));
  });
}

TEST(CostModelGuardWalkTest, mergedOrCycleInSwitchGuardCompletes) {
  withOptimizerWatchdog([] {
    EGraph graph(testGraphConfig());
    const auto predicate = field("p");
    const auto falseValue =
        std::make_shared<ConstantTypedExpr>(BOOLEAN(), variant(false));
    const auto disjunction = call("or", {predicate, falseValue});
    const auto predicateId = graph.addExpression(predicate);
    const auto orId = graph.addExpression(disjunction);
    const auto expression = call(
        "switch",
        {disjunction, number(1), call("identity", {number(2)}, INTEGER())},
        INTEGER());
    const auto root = graph.addExpression(expression);
    // or(p, FALSE) == p. Keep OR first to force the recursive pricing path
    // through a child resolving to its own class, without a folding rule.
    graph.merge(orId, predicateId);
    const auto& merged = graph.getClass(graph.find(orId));
    ASSERT_EQ(merged.nodes()[0].kind, ENode::Kind::Or);
    ASSERT_EQ(graph.find(merged.nodes()[0].children[0]), graph.find(orId));
    checkCostAndExtraction(graph, root);
    // The selected guard is the field (selectivity .5), not the cyclic OR.
    EXPECT_DOUBLE_EQ(*graph.getClass(graph.find(root)).cachedCost(), 14);
  });
}

TEST(CostModelGuardWalkTest, parentPricingUsesCheapestCachedChild) {
  withOptimizerWatchdog([] {
    EGraph graph(testGraphConfig());
    const auto value = field("value", INTEGER());
    const auto redundant = call("coalesce", {value, value}, INTEGER());
    const auto redundantId = graph.addExpression(redundant);
    const auto valueId = graph.addExpression(value);
    const auto expression = call("identity", {redundant}, INTEGER());
    const auto root = graph.addExpression(expression);
    graph.merge(redundantId, valueId);
    const auto result = graph.extractBest(root, CostModel{});
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->inputs().size(), 1);
    EXPECT_NE(
        dynamic_cast<const FieldAccessTypedExpr*>(result->inputs()[0].get()),
        nullptr);
    const auto& child = graph.getClass(graph.find(valueId));
    ASSERT_TRUE(child.cachedCost().has_value());
    EXPECT_DOUBLE_EQ(
        CostModel{}.computeCost(
            graph.getClass(graph.find(root)).nodes()[0], graph),
        CostModel::baseCost(ENode::Kind::Call, "identity") +
            *child.cachedCost());
  });
}

TEST(CostModelGuardWalkTest, pmodKeepsItsArithmeticCost) {
  EXPECT_DOUBLE_EQ(CostModel::baseCost(ENode::Kind::Call, "pmod"), 4.0);
}

TEST(CostModelGuardWalkTest, onlyCaseBoundariesIncreaseRecursiveDepth) {
  withOptimizerWatchdog([] {
    const auto yes =
        std::make_shared<ConstantTypedExpr>(BOOLEAN(), variant(true));
    const auto no =
        std::make_shared<ConstantTypedExpr>(BOOLEAN(), variant(false));
    const auto inner = call("switch", {field("p"), yes, no});
    const double cost = recursiveCost(inner);
    EXPECT_DOUBLE_EQ(recursiveCost(call("and", {yes, inner})), 2 + 1 + cost);
    EXPECT_DOUBLE_EQ(recursiveCost(call("or", {inner, no})), 2 + cost + 0.5);
    EXPECT_DOUBLE_EQ(recursiveCost(call("identity", {inner})), 10 + cost);
    EXPECT_DOUBLE_EQ(
        recursiveCost(call("switch", {field("q"), inner, yes})),
        5 + 1 + 2 + 0.5 * (cost + 5 * 0.5) + 0.5);
  });
}

TEST(CostModelGuardWalkTest, unreachableCaseSuffixHasNoCost) {
  withOptimizerWatchdog([] {
    const auto yes =
        std::make_shared<ConstantTypedExpr>(BOOLEAN(), variant(true));
    const auto condition = field("p");
    const auto prefix =
        call("switch", {condition, number(1), yes, number(2)}, INTEGER());
    const auto withDeadTail = call(
        "switch",
        {condition,
         number(1),
         yes,
         number(2),
         field("unused"),
         number(99),
         field("unreachable_else", INTEGER())},
        INTEGER());
    EXPECT_DOUBLE_EQ(recursiveCost(prefix), recursiveCost(withDeadTail));
  });
}

TEST(CostModelGuardWalkTest, sameClassSelectsAndMaterializesAtEachCaseDepth) {
  withOptimizerWatchdog([] {
    EGraph graph(testGraphConfig());
    const auto inner =
        call("switch", {field("p"), number(1), number(2)}, INTEGER());
    const auto outer =
        call("switch", {field("q"), inner, number(0)}, INTEGER());
    const auto pair = std::make_shared<ConcatTypedExpr>(
        std::vector<std::string>{"plain", "nested"},
        std::vector<TypedExprPtr>{inner, outer});
    const auto root = graph.addExpression(pair);
    const auto innerId = graph.addExpression(inner);
    // This synthetic dialect declares the call equivalent to the inner CASE.
    // CASE costs 9 at depth zero and 11.5 at depth one; the call always
    // costs 10.
    graph.merge(
        innerId, graph.addExpression(call("equivalent_value", {}, INTEGER())));
    const auto alone = graph.extractBest(innerId, CostModel{});
    ASSERT_NE(alone, nullptr);
    EXPECT_EQ(dynamic_cast<const CallTypedExpr&>(*alone).name(), "switch");
    EXPECT_DOUBLE_EQ(*graph.getClass(graph.find(innerId)).cachedCost(), 9);
    for (int repeat = 0; repeat < 2; ++repeat) {
      const auto result = graph.extractBest(root, CostModel{});
      ASSERT_NE(result, nullptr);
      const auto& plain =
          dynamic_cast<const CallTypedExpr&>(*result->inputs()[0]);
      const auto& nested =
          dynamic_cast<const CallTypedExpr&>(*result->inputs()[1]);
      EXPECT_EQ(plain.name(), "switch");
      EXPECT_EQ(
          dynamic_cast<const CallTypedExpr&>(*nested.inputs()[1]).name(),
          "equivalent_value");
      const double selectedCost =
          *graph.getClass(graph.find(root)).cachedCost();
      EXPECT_DOUBLE_EQ(selectedCost, 25.5);
      EXPECT_DOUBLE_EQ(recursiveCost(result), selectedCost);
      EXPECT_DOUBLE_EQ(
          CostModel{}.computeCost(graph.getClass(root).nodes()[0], graph),
          selectedCost);
    }
  });
}

TEST(
    CostModelGuardWalkTest,
    cycleCannotBorrowZeroCostThenReconstructAnotherNode) {
  withOptimizerWatchdog([] {
    EGraph graph(testGraphConfig());
    const auto predicate = call("equalto", {field("x", INTEGER()), number(1)});
    const auto yes =
        std::make_shared<ConstantTypedExpr>(BOOLEAN(), variant(true));
    const auto root = graph.addExpression(call("and", {predicate, yes}));
    graph.merge(root, graph.addExpression(predicate)); // and(p, TRUE) == p.
    const auto id = graph.find(root);
    const auto cyclic = graph.getClass(id).nodes()[0];
    ASSERT_EQ(cyclic.kind, ENode::Kind::And);
    EXPECT_TRUE(std::isinf(CostModel{}.computeCost(cyclic, graph)));
    for (int repeat = 0; repeat < 2; ++repeat) {
      // Stale scalar hints cannot override the contextual finite witness.
      graph.getClass(id).setCachedCost(0);
      graph.getClass(id).setBestNodeIndex(0);
      const auto result = graph.extractBest(id, CostModel{});
      ASSERT_NE(result, nullptr);
      EXPECT_EQ(dynamic_cast<const CallTypedExpr&>(*result).name(), "equalto");
      EXPECT_DOUBLE_EQ(*graph.getClass(id).cachedCost(), 6);
      EXPECT_DOUBLE_EQ(recursiveCost(result), 6);
    }
  });
}

TEST(CostModelGuardWalkTest, deadArmStillRequiresAFiniteStructuralWitness) {
  withOptimizerWatchdog([] {
    EGraph graph(testGraphConfig());
    const auto one = number(1);
    const auto equivalent = call("identity", {one}, INTEGER());
    const auto yes =
        std::make_shared<ConstantTypedExpr>(BOOLEAN(), variant(true));
    const auto root =
        graph.addExpression(call("switch", {yes, one, equivalent}, INTEGER()));
    graph.merge(root, graph.addExpression(equivalent));
    const auto cyclic = graph.getClass(graph.find(root)).nodes()[0];
    ASSERT_EQ(cyclic.kind, ENode::Kind::Switch);
    EXPECT_TRUE(std::isinf(CostModel{}.computeCost(cyclic, graph)));
    const auto result = graph.extractBest(root, CostModel{});
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(dynamic_cast<const CallTypedExpr&>(*result).name(), "identity");
    EXPECT_DOUBLE_EQ(
        *graph.getClass(graph.find(root)).cachedCost(), recursiveCost(result));
  });
}

TEST(CostModelGuardWalkTest, budgetExhaustionAbandonsAnEarlierFiniteChoice) {
  withOptimizerWatchdog([] {
    EGraph graph(testGraphConfig());
    const auto value = field("x", INTEGER());
    const auto redundant = call("coalesce", {value, value}, INTEGER());
    graph.merge(graph.addExpression(redundant), graph.addExpression(value));
    const auto root = graph.addExpression(call("identity", {value}, INTEGER()));
    auto expanded = value;
    for (int i = 0; i < 18; ++i) {
      // Every class reaches the coalesce cycle: these two occurrences must
      // be solved in their path contexts, without unsafe DAG memoization.
      expanded = call("duplicate_value", {expanded, expanded}, INTEGER());
    }
    graph.merge(root, graph.addExpression(expanded));
    // The first root alternative is finite, but the full search exceeds the
    // budget. Returning it would present an unfinished comparison as best.
    EXPECT_EQ(graph.extractBest(root, CostModel{}), nullptr);
    EXPECT_FALSE(graph.getClass(graph.find(root)).cachedCost().has_value());
  });
}

TEST(CostModelGuardWalkTest, watchdogRethrowsWorkerException) {
  EXPECT_THROW(
      withOptimizerWatchdog([] { throw std::runtime_error("worker failed"); }),
      std::runtime_error);
}

TEST(CostModelGuardWalkTest, metadataReceivesEachNestedSignatureIntact) {
  const auto inner =
      call("inner", {field("i", INTEGER()), field("b")}, DOUBLE());
  const auto outer = call("outer", {inner, field("s", VARCHAR())});
  std::vector<std::pair<std::string, std::vector<TypePtr>>> signatures;
  const auto check =
      [&](const std::string& name, const auto& inputs, const auto&) {
        signatures.emplace_back(name, inputs);
        return true;
      };
  EXPECT_TRUE(isExpressionDeterministic(call("and", {outer, outer}), check));
  ASSERT_EQ(signatures.size(), 2);
  EXPECT_EQ(signatures[0].first, "inner");
  EXPECT_EQ(signatures[0].second, (std::vector<TypePtr>{INTEGER(), BOOLEAN()}));
  EXPECT_EQ(signatures[1].first, "outer");
  EXPECT_EQ(signatures[1].second, (std::vector<TypePtr>{DOUBLE(), VARCHAR()}));
}
