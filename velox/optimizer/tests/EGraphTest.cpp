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
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include "velox/optimizer/tests/OptimizerTestConfig.h"

#include "velox/common/memory/Memory.h"
#include "velox/core/Expressions.h"
#include "velox/optimizer/CostModel.h"
#include "velox/optimizer/EGraph.h"
#include "velox/optimizer/RewriteRule.h"
#include "velox/vector/VariantToVector.h"

using namespace facebook::velox;
using namespace facebook::velox::core;
using namespace facebook::velox::optimizer;

namespace {

// Helper: create a simple field access expression.
TypedExprPtr makeField(const std::string& name, TypePtr type = BIGINT()) {
  return std::make_shared<FieldAccessTypedExpr>(type, name);
}

// Helper: create a constant expression.
TypedExprPtr makeConstant(int64_t value) {
  return std::make_shared<ConstantTypedExpr>(BIGINT(), variant(value));
}

// Helper: create a function call expression.
TypedExprPtr makeCall(
    const std::string& name,
    std::vector<TypedExprPtr> inputs,
    TypePtr type) {
  return std::make_shared<CallTypedExpr>(type, std::move(inputs), name);
}

// Helper: create a switch (CASE WHEN) expression.
TypedExprPtr makeSwitch(
    std::vector<TypedExprPtr> inputs,
    TypePtr type = BIGINT()) {
  return std::make_shared<CallTypedExpr>(type, std::move(inputs), "switch");
}

// Helper: create a cast expression.
TypedExprPtr
makeCast(TypedExprPtr input, TypePtr toType, bool nullOnFailure = false) {
  return std::make_shared<CastTypedExpr>(
      toType, std::move(input), nullOnFailure);
}

class MergeOnlyRule : public RewriteRule {
 public:
  explicit MergeOnlyRule(std::function<void(EGraph&)> action)
      : action_(std::move(action)) {}
  std::string name() const override {
    return "TestMergeOnlyRule";
  }
  size_t apply(EGraph& graph) override {
    action_(graph);
    return 0; // Existing classes only: no new e-nodes.
  }

 private:
  std::function<void(EGraph&)> action_;
};

} // namespace

class EGraphTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config_.debugLog = false;
    config_.maxIterations = 3;
  }

  EGraphConfig config_ = testGraphConfig();
};

TEST_F(EGraphTest, addSimpleExpression) {
  // Create: a + 1
  auto expr = makeCall("+", {makeField("a"), makeConstant(1)}, BIGINT());

  EGraph graph(config_);
  EXPECT_EQ(graph.numClasses(), 0u);
  EXPECT_EQ(graph.numNodes(), 0u);
  EXPECT_EQ(graph.config().maxIterations, config_.maxIterations);
  uint32_t rootId = graph.addExpression(expr);

  EXPECT_LT(rootId, graph.numClasses());
  EXPECT_GT(graph.numNodes(), 0);
}

TEST_F(EGraphTest, addSwitchExpression) {
  // Create: CASE WHEN a > 0 THEN 1 ELSE 0 END
  auto expr = makeSwitch(
      {
          makeCall(">", {makeField("a"), makeConstant(0)}, BOOLEAN()),
          makeConstant(1),
          makeConstant(0),
      },
      BIGINT());

  EGraph graph(config_);
  uint32_t rootId = graph.addExpression(expr);

  EXPECT_LT(rootId, graph.numClasses());
  EXPECT_GT(graph.numNodes(), 0);

  const auto& eclass = graph.getClass(graph.find(rootId));
  EXPECT_EQ(eclass.type()->kind(), TypeKind::BIGINT);
}

TEST_F(EGraphTest, canonicalizeDeduplicates) {
  // Add the same expression twice — should get the same e-class ID.
  auto expr = makeCall("+", {makeField("a"), makeConstant(1)}, BIGINT());

  EGraph graph(config_);
  uint32_t id1 = graph.addExpression(expr);
  uint32_t id2 = graph.addExpression(expr);

  EXPECT_EQ(graph.find(id1), graph.find(id2));
}

TEST_F(EGraphTest, merge) {
  // Create two different expressions and merge their e-classes.
  auto expr1 = makeField("a");
  auto expr2 = makeField("b");

  EGraph graph(config_);
  uint32_t id1 = graph.addExpression(expr1);
  uint32_t id2 = graph.addExpression(expr2);

  EXPECT_NE(graph.find(id1), graph.find(id2));

  graph.merge(id1, id2);

  EXPECT_EQ(graph.find(id1), graph.find(id2));
}

TEST_F(EGraphTest, extractBest) {
  // Create an expression and verify extraction works.
  auto expr = makeCall("+", {makeField("a"), makeConstant(1)}, BIGINT());

  EGraph graph(config_);
  uint32_t rootId = graph.addExpression(expr);

  CostModel costModel;
  auto result = graph.extractBest(rootId, costModel);

  EXPECT_NE(result, nullptr);
  EXPECT_EQ(result->type()->kind(), TypeKind::BIGINT);
}

TEST_F(EGraphTest, addComplexNestedSwitch) {
  // Create: CASE WHEN a > 10 THEN CASE WHEN b < 5 THEN 1 ELSE 2 END ELSE 3 END
  auto innerSwitch = makeSwitch(
      {
          makeCall("<", {makeField("b"), makeConstant(5)}, BOOLEAN()),
          makeConstant(1),
          makeConstant(2),
      },
      BIGINT());

  auto outerSwitch = makeSwitch(
      {
          makeCall(">", {makeField("a"), makeConstant(10)}, BOOLEAN()),
          innerSwitch,
          makeConstant(3),
      },
      BIGINT());

  EGraph graph(config_);
  uint32_t rootId = graph.addExpression(outerSwitch);

  EXPECT_LT(rootId, graph.numClasses());
  // Should have at least a few e-classes for the nested structure.
  EXPECT_GT(graph.numClasses(), 3);
}

TEST_F(EGraphTest, emptyExpression) {
  EGraph graph(config_);
  // Adding a plain constant should work.
  auto expr = makeConstant(42);
  uint32_t rootId = graph.addExpression(expr);

  EXPECT_LT(rootId, graph.numClasses());
  const auto& eclass = graph.getClass(graph.find(rootId));
  ASSERT_FALSE(eclass.nodes().empty());
  EXPECT_EQ(eclass.nodes()[0].kind, ENode::Kind::Constant);
}

TEST_F(EGraphTest, booleanExpression) {
  // Create: a > 0 AND b < 10
  auto expr = makeCall(
      "and",
      {
          makeCall(">", {makeField("a"), makeConstant(0)}, BOOLEAN()),
          makeCall("<", {makeField("b"), makeConstant(10)}, BOOLEAN()),
      },
      BOOLEAN());

  EGraph graph(config_);
  uint32_t rootId = graph.addExpression(expr);

  EXPECT_LT(rootId, graph.numClasses());
  const auto& eclass = graph.getClass(graph.find(rootId));
  ASSERT_FALSE(eclass.nodes().empty());
  EXPECT_EQ(eclass.nodes()[0].kind, ENode::Kind::And);
}

// ---------------------------------------------------------------------------
// ENode unit tests — the hash-consing primitives every rule relies on.
// ---------------------------------------------------------------------------

class ENodeTest : public ::testing::Test {};
static_assert(!std::is_default_constructible_v<ENode>);

TEST_F(ENodeTest, equalNodesCompareEqualAndHashCodeMatch) {
  // Hash-consistency invariant: structurally equal e-nodes must be equal
  // AND hash identically, or the canonicalization table would miss.
  ENode a(ENode::Kind::Call, BIGINT(), "+", {1, 2});
  ENode b(ENode::Kind::Call, BIGINT(), "+", {1, 2});
  EXPECT_TRUE(a == b);
  EXPECT_EQ(a.hash(), b.hash());
  EXPECT_EQ(ENodeHash{}(a), ENodeHash{}(b));
  ENode nullType(ENode::Kind::Call, nullptr, "f", {4});
  EXPECT_NO_THROW(ENodeHash{}(nullType));
}

TEST_F(ENodeTest, differentFieldsBreakEquality) {
  ENode base(ENode::Kind::Call, BIGINT(), "+", {1, 2});
  EXPECT_FALSE(base == ENode(ENode::Kind::Call, BIGINT(), "-", {1, 2}));
  EXPECT_FALSE(base == ENode(ENode::Kind::Call, BIGINT(), "+", {1, 3}));
  EXPECT_FALSE(base == ENode(ENode::Kind::Call, BIGINT(), "+", {1}));
  EXPECT_FALSE(base == ENode(ENode::Kind::And, BOOLEAN(), "and", {1, 2}));
  EXPECT_FALSE(base == ENode(ENode::Kind::Call, DOUBLE(), "+", {1, 2}));

  ENode c1(ENode::Kind::Constant, BIGINT(), "", {});
  c1.constantValue = variant(int64_t(7));
  ENode c2(ENode::Kind::Constant, BIGINT(), "", {});
  c2.constantValue = variant(int64_t(8));
  EXPECT_FALSE(c1 == c2); // different constant values
  ENode cNone(ENode::Kind::Constant, BIGINT(), "", {});
  EXPECT_FALSE(c1 == cNone); // value vs no value
  ENode c1Copy(ENode::Kind::Constant, BIGINT(), "", {});
  c1Copy.constantValue = variant(int64_t(7));
  EXPECT_TRUE(c1 == c1Copy);
}

TEST_F(ENodeTest, nullableAndStructuralTypesPreserveHashIdentity) {
  ENode unknown(ENode::Kind::Input, nullptr, "", {});
  ENode unknownCopy(ENode::Kind::Input, nullptr, "", {});
  const auto first = ROW({"x"}, {BIGINT()});
  const auto second = ROW({"x"}, {BIGINT()});
  ASSERT_NE(first.get(), second.get());
  ENode row(ENode::Kind::Input, first, "", {});
  ENode equalRow(ENode::Kind::Input, second, "", {});
  EXPECT_TRUE(unknown == unknownCopy);
  EXPECT_EQ(unknown.hash(), unknownCopy.hash());
  EXPECT_FALSE(unknown == row);
  EXPECT_FALSE(row == unknown);
  EXPECT_TRUE(row == equalRow);
  EXPECT_EQ(row.hash(), equalRow.hash());
  EXPECT_FALSE(
      row == ENode(ENode::Kind::Input, ROW({"y"}, {BIGINT()}), "", {}));
}

TEST_F(ENodeTest, fromTypedExprScalarConstant) {
  auto expr = makeConstant(5);
  ENode node = ENode::fromTypedExpr(*expr, {});
  EXPECT_EQ(node.kind, ENode::Kind::Constant);
  ASSERT_TRUE(node.constantValue.has_value());
  EXPECT_EQ(node.constantValue->value<int64_t>(), 5);
  EXPECT_EQ(node.originalExpr, nullptr);
}

TEST_F(ENodeTest, fromTypedExprFieldAccessAndCallKinds) {
  auto a = makeField("a");
  auto one = makeConstant(1);
  auto p = makeField("p", BOOLEAN());
  std::unordered_map<const ITypedExpr*, uint32_t> map{
      {a.get(), 7}, {one.get(), 9}, {p.get(), 11}};

  ENode field = ENode::fromTypedExpr(*a, map);
  EXPECT_EQ(field.kind, ENode::Kind::FieldAccess);
  EXPECT_EQ(field.name, "a");
  EXPECT_TRUE(field.children.empty());

  auto plus = makeCall("+", {a, one}, BIGINT());
  ENode call = ENode::fromTypedExpr(*plus, map);
  EXPECT_EQ(call.kind, ENode::Kind::Call);
  EXPECT_EQ(call.name, "+");
  ASSERT_EQ(call.children.size(), 2u);
  EXPECT_EQ(call.children[0], 7u);
  EXPECT_EQ(call.children[1], 9u);

  // Both "switch" and "if" map to the Switch kind.
  auto switchExpr = makeSwitch({p, one, one}, BIGINT());
  EXPECT_EQ(ENode::fromTypedExpr(*switchExpr, map).kind, ENode::Kind::Switch);
  auto ifExpr = makeCall("if", {p, one, one}, BIGINT());
  EXPECT_EQ(ENode::fromTypedExpr(*ifExpr, map).kind, ENode::Kind::Switch);

  // Logical connectives get dedicated kinds; plain boolean calls do not.
  EXPECT_EQ(
      ENode::fromTypedExpr(*makeCall("and", {p, p}, BOOLEAN()), map).kind,
      ENode::Kind::And);
  EXPECT_EQ(
      ENode::fromTypedExpr(*makeCall("or", {p, p}, BOOLEAN()), map).kind,
      ENode::Kind::Or);
  EXPECT_EQ(
      ENode::fromTypedExpr(*makeCall("equalto", {p, p}, BOOLEAN()), map).kind,
      ENode::Kind::Call);
}

TEST_F(ENodeTest, fromTypedExprCastVariants) {
  auto a = makeField("a");
  std::unordered_map<const ITypedExpr*, uint32_t> map{{a.get(), 3}};
  auto cast = makeCast(a, VARCHAR());
  EXPECT_EQ(ENode::fromTypedExpr(*cast, map).kind, ENode::Kind::Cast);
  EXPECT_EQ(ENode::fromTypedExpr(*cast, map).name, "cast");
  auto tryCast = makeCast(a, VARCHAR(), /*nullOnFailure=*/true);
  EXPECT_EQ(ENode::fromTypedExpr(*tryCast, map).name, "try_cast");
}

TEST_F(ENodeTest, fromTypedExprDereferenceConcatLambda) {
  auto a = makeField("a");
  std::unordered_map<const ITypedExpr*, uint32_t> map{{a.get(), 1}};

  auto deref = std::make_shared<DereferenceTypedExpr>(BIGINT(), a, 3);
  ENode d = ENode::fromTypedExpr(*deref, map);
  EXPECT_EQ(d.kind, ENode::Kind::Dereference);
  EXPECT_EQ(d.name, "3");

  auto concat = std::make_shared<ConcatTypedExpr>(
      std::vector<std::string>{"x"}, std::vector<TypedExprPtr>{a});
  ENode c = ENode::fromTypedExpr(*concat, map);
  EXPECT_EQ(c.kind, ENode::Kind::Concat);
  EXPECT_EQ(c.name, "concat");

  auto lambda = std::make_shared<LambdaTypedExpr>(
      ROW(std::vector<std::string>{"x"}, std::vector<TypePtr>{BIGINT()}), a);
  ENode l = ENode::fromTypedExpr(*lambda, map);
  EXPECT_EQ(l.kind, ENode::Kind::Lambda);
  EXPECT_EQ(l.name, "lambda");
}

TEST_F(ENodeTest, fromTypedExprVectorBackedConstantKeepsOriginal) {
  // Vector-backed constants (e.g. arrays) cannot be extracted as variants:
  // the node must keep the original TypedExprPtr for reconstruction.
  memory::MemoryManager::testingSetInstance({});
  auto pool = memory::memoryManager()->addLeafPool("enodeVectorConst");
  auto arrayExpr = std::make_shared<ConstantTypedExpr>(
      variantArrayToVector(ARRAY(BIGINT()), {variant(int64_t(1))}, pool.get()));

  ENode node = ENode::fromTypedExpr(*arrayExpr, {}, arrayExpr);
  EXPECT_EQ(node.kind, ENode::Kind::Constant);
  EXPECT_FALSE(node.constantValue.has_value());
  EXPECT_EQ(node.originalExpr, arrayExpr);
}

TEST_F(ENodeTest, fromTypedExprMissingChildThrows) {
  // A child expression absent from the e-class mapping is an internal
  // error (VELOX_CHECK), not a silent zero id. This fork's exception
  // hierarchy has no VeloxCheckError: checks throw a VeloxException
  // subclass, which EXPECT_THROW catches by base type.
  auto a = makeField("a");
  auto call = makeCall("+", {a, makeConstant(1)}, BIGINT());
  EXPECT_THROW(ENode::fromTypedExpr(*call, {}), VeloxException);
}

// ---------------------------------------------------------------------------
// EClass unit tests.
// ---------------------------------------------------------------------------

class EClassTest : public ::testing::Test {};

TEST_F(EClassTest, addNodeDeduplicatesAndInvalidatesCost) {
  EClass eclass(3, BIGINT());
  EXPECT_EQ(eclass.id(), 3u);
  EXPECT_EQ(eclass.type()->kind(), TypeKind::BIGINT);
  EXPECT_EQ(eclass.bestNodeIndex(), 0u);

  ENode node(ENode::Kind::Call, BIGINT(), "f", {1});
  EXPECT_TRUE(eclass.addNode(node));
  EXPECT_FALSE(eclass.addNode(node)); // duplicate — copy overload
  EXPECT_EQ(eclass.nodes().size(), 1u);

  eclass.setCachedCost(1.5);
  ASSERT_TRUE(eclass.cachedCost().has_value());
  EXPECT_DOUBLE_EQ(*eclass.cachedCost(), 1.5);

  // Any structural change must drop the cached cost.
  ENode other(ENode::Kind::Call, BIGINT(), "g", {1});
  EXPECT_TRUE(eclass.addNode(other));
  EXPECT_FALSE(eclass.cachedCost().has_value());

  eclass.setBestNodeIndex(1);
  EXPECT_EQ(eclass.bestNodeIndex(), 1u);
  eclass.invalidateCost();
  EXPECT_FALSE(eclass.cachedCost().has_value());
  EXPECT_EQ(
      eclass.bestNodeIndex(), 0u); // Fallback until extraction recomputes.
}

TEST_F(EClassTest, addNodeMoveOverloadDeduplicatesToo) {
  EClass eclass(0, BOOLEAN());
  EXPECT_TRUE(eclass.addNode(ENode(ENode::Kind::Or, BOOLEAN(), "or", {1, 2})));
  EXPECT_FALSE(eclass.addNode(ENode(ENode::Kind::Or, BOOLEAN(), "or", {1, 2})));
  EXPECT_EQ(eclass.nodes().size(), 1u);
}

// ---------------------------------------------------------------------------
// EGraph core: merge/rebuild invariants (union-find, congruence closure,
// hash-consing) on top of the smoke tests above.
// ---------------------------------------------------------------------------

TEST_F(EGraphTest, mergeIsIdempotent) {
  auto exprA = makeField("a");
  auto exprB = makeField("b");
  EGraph graph(config_);
  auto idA = graph.addExpression(exprA);
  auto idB = graph.addExpression(exprB);

  graph.merge(idA, idB);
  const size_t nodesAfterFirst = graph.numNodes();
  const uint32_t root = graph.find(idA);
  const auto revision = graph.revision();
  graph.getClass(root).setCachedCost(2);

  // Re-merging the same pair (or a self-merge) must be a no-op.
  graph.merge(idA, idB);
  graph.merge(root, root);
  EXPECT_EQ(graph.find(idA), graph.find(idB));
  EXPECT_EQ(graph.find(idA), root);
  EXPECT_EQ(graph.numNodes(), nodesAfterFirst);
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(graph.getClass(root).cachedCost(), 2);
}

TEST_F(EGraphTest, mergeTriggersCongruenceClosure) {
  // merge(a, b) must also merge (a + 1) with (b + 1): rebuild()
  // re-canonicalizes parents over merged children.
  auto plusA = makeCall("+", {makeField("a"), makeConstant(1)}, BIGINT());
  auto plusB = makeCall("+", {makeField("b"), makeConstant(1)}, BIGINT());
  EGraph graph(config_);
  auto idA = graph.addExpression(plusA->inputs()[0]);
  auto idB = graph.addExpression(plusB->inputs()[0]);
  auto idPlusA = graph.addExpression(plusA);
  auto idPlusB = graph.addExpression(plusB);
  EXPECT_NE(graph.find(idPlusA), graph.find(idPlusB));

  graph.merge(idA, idB);
  EXPECT_EQ(graph.find(idPlusA), graph.find(idPlusB));
}

TEST_F(EGraphTest, deepCongruenceClosureRefreshesStoredNodesAndHashCons) {
  withOptimizerWatchdog([&] {
    EGraph graph(config_);
    std::vector<uint32_t> left{graph.addExpression(makeField("a"))};
    std::vector<uint32_t> right{graph.addExpression(makeField("b"))};
    for (size_t i = 0; i < 128; ++i) {
      left.push_back(graph.canonicalize(
          ENode(ENode::Kind::Call, BIGINT(), "f", {left.back()})));
      right.push_back(graph.canonicalize(
          ENode(ENode::Kind::Call, BIGINT(), "f", {right.back()})));
    }
    graph.merge(left[0], right[0]);
    ASSERT_FALSE(graph.overflowed());
    const auto count = graph.numNodes();
    for (size_t i = 1; i < left.size(); ++i) {
      const auto root = graph.find(left[i]);
      EXPECT_EQ(root, graph.find(right[i]));
      ASSERT_EQ(graph.getClass(root).nodes().size(), 1);
      EXPECT_EQ(
          graph.getClass(root).nodes()[0].children[0], graph.find(left[i - 1]));
      EXPECT_EQ(
          graph.canonicalize(
              ENode(ENode::Kind::Call, BIGINT(), "f", {right[i - 1]})),
          root);
    }
    EXPECT_EQ(graph.numNodes(), count);
  });
}

TEST_F(EGraphTest, classBudgetAbandonsUnionWithoutDroppingAlternatives) {
  config_.maxNodesPerClass = 2;
  EGraph graph(config_);
  const auto a = graph.addExpression(makeField("a"));
  const auto b = graph.addExpression(makeField("b"));
  const auto c = graph.addExpression(makeField("c"));
  graph.merge(a, b);
  ASSERT_FALSE(graph.overflowed());
  graph.merge(a, c);
  EXPECT_TRUE(graph.overflowed());
  EXPECT_NE(graph.find(a), graph.find(c));
  EXPECT_EQ(graph.getClass(graph.find(a)).nodes().size(), 2);
  EXPECT_EQ(graph.getClass(graph.find(c)).nodes().size(), 1);
  EXPECT_EQ(graph.extractBest(a, CostModel{}), nullptr);
}

TEST_F(EGraphTest, zeroBudgetsReturnUnextractableGraphWithoutInvalidLookup) {
  for (bool classBudget : {false, true}) {
    auto config = config_;
    (classBudget ? config.maxNodesPerClass : config.maxTotalNodes) = 0;
    EGraph graph(config);
    const auto root = graph.addExpression(
        makeCall("+", {makeField("a"), makeConstant(1)}, BIGINT()));
    EXPECT_TRUE(graph.overflowed());
    EXPECT_EQ(graph.numClasses(), 0);
    EXPECT_EQ(graph.extractBest(root, CostModel{}), nullptr);
  }
}

TEST_F(EGraphTest, mutableClassCannotBypassTotalNodeBudget) {
  config_.maxTotalNodes = 2;
  EGraph graph(config_);
  const auto root = graph.addExpression(makeField("a"));
  graph.addExpression(makeField("b"));
  graph.getClassMutable(root).addNode(
      ENode(ENode::Kind::FieldAccess, BIGINT(), "c", {}));
  EXPECT_EQ(graph.extractBest(root, CostModel{}), nullptr);
  EXPECT_TRUE(graph.overflowed());
  EXPECT_EQ(graph.getClass(root).nodes().size(), 2);
}

TEST_F(EGraphTest, mergeChainHasSingleRoot) {
  // Merges in any order and direction collapse to one root; the redundant
  // third merge is a no-op.
  EGraph graph(config_);
  std::vector<uint32_t> ids;
  for (const char* name : {"a", "b", "c"}) {
    ids.push_back(graph.addExpression(makeField(name)));
  }
  graph.merge(ids[0], ids[1]);
  graph.merge(ids[1], ids[2]);
  graph.merge(ids[2], ids[0]);
  for (auto id : ids) {
    EXPECT_EQ(graph.find(id), graph.find(ids[0]));
  }
}

TEST_F(EGraphTest, canonicalizeDeduplicatesEqualNodes) {
  EGraph graph(config_);
  auto idA = graph.addExpression(makeField("a"));
  ENode len(ENode::Kind::Call, BIGINT(), "len", {idA});

  uint32_t first = graph.canonicalize(len);
  uint32_t second = graph.canonicalize(len);
  EXPECT_EQ(first, second);

  const size_t nodesBefore = graph.numNodes();
  EXPECT_EQ(graph.canonicalize(len), first);
  EXPECT_EQ(graph.numNodes(), nodesBefore); // no third class allocated
  {
    const std::string name(128, 'x');
    const auto id = graph.addExpression(makeField(name));
    const auto count = graph.numNodes();
    const auto& borrowed = graph.getClassMutable(id).nodes()[0];
    EXPECT_EQ(graph.canonicalize(borrowed), graph.find(id));
    EXPECT_EQ(graph.getClass(graph.find(id)).nodes()[0].name, name);
    EXPECT_EQ(graph.numNodes(), count);
  }
}

TEST_F(EGraphTest, getClassMutableAddNode) {
  EGraph graph(config_);
  auto idA = graph.addExpression(makeField("a"));
  const size_t before = graph.getClass(idA).nodes().size();
  EXPECT_TRUE(graph.getClassMutable(idA).addNode(
      ENode(ENode::Kind::Call, BOOLEAN(), "isnotnull", {idA})));
  EXPECT_EQ(graph.getClass(idA).nodes().size(), before + 1);
}

TEST_F(EGraphTest, mutableAliasUpdatesCanonicalClassAndHashCons) {
  EGraph graph(config_);
  const auto a = graph.addExpression(makeField("a"));
  const auto b = graph.addExpression(makeField("b"));
  graph.merge(a, b);
  const auto root = graph.find(a);
  const auto alias = root == a ? b : a;
  const auto count = graph.numNodes();
  const ENode alternative(ENode::Kind::FieldAccess, BIGINT(), "c", {});
  auto& mutableClass = graph.getClassMutable(alias);
  EXPECT_EQ(&mutableClass, &graph.getClass(root));
  ASSERT_TRUE(mutableClass.addNode(alternative));
  EXPECT_EQ(graph.canonicalize(alternative), root);
  EXPECT_EQ(graph.numNodes(), count + 1);
  EXPECT_EQ(graph.getClass(root).nodes().size(), 3);
  EXPECT_FALSE(graph.overflowed());
}

TEST_F(EGraphTest, incrementalRepairPreservesUnrelatedNodesAndCachedCosts) {
  EGraph graph(config_);
  const auto a = graph.addExpression(makeField("a"));
  const auto b = graph.addExpression(makeField("b"));
  const auto left =
      graph.canonicalize(ENode(ENode::Kind::Call, BIGINT(), "f", {a}));
  const auto right =
      graph.canonicalize(ENode(ENode::Kind::Call, BIGINT(), "f", {b}));
  const auto unrelated =
      graph.addExpression(makeCall("g", {makeField("unrelated")}, BIGINT()));
  const auto* stableNodes = graph.getClass(unrelated).nodes().data();
  graph.getClass(left).setCachedCost(12);
  graph.getClass(right).setCachedCost(12);
  graph.getClass(unrelated).setCachedCost(12);
  graph.merge(a, b);
  EXPECT_EQ(graph.find(left), graph.find(right)); // Closure is immediate.
  EXPECT_FALSE(graph.getClass(graph.find(left)).cachedCost().has_value());
  EXPECT_EQ(graph.getClass(unrelated).cachedCost(), 12);
  EXPECT_EQ(graph.getClass(unrelated).nodes().data(), stableNodes);

  graph.getClass(graph.find(left)).setCachedCost(12);
  const ENode alternative(ENode::Kind::FieldAccess, BIGINT(), "c", {});
  graph.getClassMutable(b).addNode(alternative);
  EXPECT_FALSE(graph.getClass(graph.find(left)).cachedCost().has_value());
  EXPECT_EQ(graph.canonicalize(alternative), graph.find(a));
  EXPECT_EQ(graph.getClass(unrelated).cachedCost(), 12);
  EXPECT_EQ(graph.getClass(unrelated).nodes().data(), stableNodes);
}

TEST_F(EGraphTest, dirtyClassReplacementsAccountGlobalBudgetAsBatch) {
  config_.maxTotalNodes = 4;
  EGraph graph(config_);
  const auto a = graph.addExpression(makeField("a"));
  const auto b = graph.addExpression(makeField("b"));
  const auto c = graph.addExpression(makeField("c"));
  const auto d = graph.addExpression(makeField("d"));
  graph.merge(a, b);
  graph.merge(c, d);
  const ENode grown(ENode::Kind::FieldAccess, BIGINT(), "grown", {});
  graph.getClassMutable(a).addNode(grown); // 2 -> 3, queued first.
  EClass replacement(graph.find(c), BIGINT());
  const ENode shrunk(ENode::Kind::FieldAccess, BIGINT(), "shrunk", {});
  replacement.addNode(shrunk);
  graph.getClassMutable(c) = std::move(replacement); // 2 -> 1.
  EXPECT_EQ(graph.canonicalize(grown), graph.find(a));
  ASSERT_FALSE(graph.overflowed());
  EXPECT_EQ(graph.numNodes(), 4);
  EXPECT_EQ(graph.canonicalize(shrunk), graph.find(c));

  const auto before = graph.revision();
  EClass sameSize(graph.find(c), BIGINT());
  const ENode renamed(ENode::Kind::FieldAccess, BIGINT(), "renamed", {});
  sameSize.addNode(renamed);
  graph.getClassMutable(c) = std::move(sameSize);
  EXPECT_GT(graph.revision(), before); // Replacement is not just a size delta.
  EXPECT_EQ(graph.canonicalize(renamed), graph.find(c));
  EXPECT_EQ(graph.numNodes(), 4);
  EXPECT_FALSE(graph.overflowed());
}

TEST_F(EGraphTest, mergeOnlyRulesReachSaturationAtExactNodeBudget) {
  withOptimizerWatchdog([&] {
    config_.maxTotalNodes = 3;
    EGraph graph(config_);
    const auto a = graph.addExpression(makeField("a"));
    const auto b = graph.addExpression(makeField("b"));
    const auto c = graph.addExpression(makeField("c"));
    const auto before = graph.revision();
    size_t calls = 0;
    std::vector<std::unique_ptr<RewriteRule>> rules;
    rules.push_back(std::make_unique<MergeOnlyRule>([&](EGraph& g) {
      ++calls;
      if (g.find(a) == g.find(b)) {
        g.merge(a, c); // Enabled by the later rule on the next iteration.
      }
    }));
    rules.push_back(
        std::make_unique<MergeOnlyRule>([&](EGraph& g) { g.merge(a, b); }));
    EXPECT_EQ(graph.applyRules(rules), 0);
    EXPECT_EQ(graph.find(a), graph.find(c));
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(graph.revision(), before + 2);
    EXPECT_EQ(graph.numNodes(), 3);
    EXPECT_FALSE(graph.overflowed());
  });
}

TEST_F(EGraphTest, extractBestPreservesSharedSubtrees) {
  // and(x, and(x, y)) with x = (a = 1): both occurrences of x must
  // materialize as the SAME TypedExprPtr (CSE on extraction).
  auto x = makeCall("=", {makeField("a"), makeConstant(1)}, BOOLEAN());
  auto y = makeCall("=", {makeField("b"), makeConstant(2)}, BOOLEAN());
  auto inner = makeCall("and", {x, y}, BOOLEAN());
  auto expr = makeCall("and", {x, inner}, BOOLEAN());

  EGraph graph(config_);
  uint32_t rootId = graph.addExpression(expr);

  CostModel costModel;
  auto result = graph.extractBest(rootId, costModel);
  ASSERT_NE(result, nullptr);
  auto* outer = dynamic_cast<const CallTypedExpr*>(result.get());
  ASSERT_NE(outer, nullptr);
  ASSERT_EQ(outer->inputs().size(), 2u);
  auto* innerResult =
      dynamic_cast<const CallTypedExpr*>(outer->inputs()[1].get());
  ASSERT_NE(innerResult, nullptr);
  EXPECT_EQ(outer->inputs()[0].get(), innerResult->inputs()[0].get());
}

TEST_F(EGraphTest, extractionRecomputesSelectionAndAncestorCostsAfterMutation) {
  withOptimizerWatchdog([&] {
    for (bool directMutation : {false, true}) {
      EGraph graph(config_);
      const auto field = makeField("x");
      const auto plus = makeCall("+", {field, makeConstant(0)}, BIGINT());
      const auto expression = makeCall("*", {plus, plus}, BIGINT());
      const auto root = graph.addExpression(expression);
      const auto plusId = graph.addExpression(plus);
      graph.extractBest(root, CostModel{});
      ASSERT_TRUE(graph.getClass(root).cachedCost().has_value());
      if (directMutation) {
        graph.getClassMutable(plusId).addNode(
            ENode(ENode::Kind::FieldAccess, BIGINT(), "x", {}));
      } else {
        graph.merge(plusId, graph.addExpression(field));
      }
      EXPECT_FALSE(graph.getClass(root).cachedCost().has_value());
      const auto result = graph.extractBest(root, CostModel{});
      ASSERT_NE(result, nullptr);
      ASSERT_EQ(result->inputs().size(), 2);
      EXPECT_NE(
          dynamic_cast<const FieldAccessTypedExpr*>(result->inputs()[0].get()),
          nullptr);
      EXPECT_TRUE(graph.getClass(graph.find(plusId)).cachedCost().has_value());
      EXPECT_NE(graph.getClass(graph.find(plusId)).bestNodeIndex(), 0);
    }
  });
}

TEST_F(EGraphTest, concatReconstructionPreservesRowNamesAndNamedAccess) {
  const auto row = std::make_shared<ConcatTypedExpr>(
      std::vector<std::string>{"first", "second"},
      std::vector<TypedExprPtr>{makeField("x"), makeField("y")});
  const auto expression =
      std::make_shared<FieldAccessTypedExpr>(BIGINT(), row, "second");
  EGraph graph(config_);
  const auto result =
      graph.extractBest(graph.addExpression(expression), CostModel{});
  ASSERT_NE(result, nullptr);
  ASSERT_EQ(result->inputs().size(), 1);
  EXPECT_TRUE(*result->inputs()[0]->type() == *row->type());
  EXPECT_EQ(
      result->inputs()[0]->type()->asRow().names(),
      row->type()->asRow().names());
  const auto* field = dynamic_cast<const FieldAccessTypedExpr*>(result.get());
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(field->name(), "second");
}

TEST_F(EGraphTest, inputReferenceRoundTripPreservesSchema) {
  const auto rowType = ROW({"a", "b"}, {BIGINT(), VARCHAR()});
  auto input = std::make_shared<InputTypedExpr>(rowType);
  EGraph graph(config_);
  const auto id = graph.addExpression(input);
  const auto& node = graph.getClass(id).nodes()[0];
  EXPECT_EQ(node.kind, ENode::Kind::Input);
  EXPECT_DOUBLE_EQ(CostModel{}.computeCost(node, graph), 0.0);
  auto result = graph.extractBest(id, CostModel{});
  ASSERT_NE(dynamic_cast<const InputTypedExpr*>(result.get()), nullptr);
  EXPECT_EQ(result->type(), rowType);
  EXPECT_TRUE(result->inputs().empty());
  const auto differentSchema = ROW({"a", "b"}, {VARCHAR(), BIGINT()});
  EXPECT_NE(
      id,
      graph.addExpression(std::make_shared<InputTypedExpr>(differentSchema)));
}

TEST_F(EGraphTest, explicitAndImplicitColumnReferencesRoundTrip) {
  auto input = std::make_shared<InputTypedExpr>(ROW({"a"}, {BIGINT()}));
  for (const auto& expression : std::vector<TypedExprPtr>{
           makeField("a"),
           std::make_shared<FieldAccessTypedExpr>(BIGINT(), input, "a")}) {
    EGraph graph(config_);
    const auto id = graph.addExpression(expression);
    auto result = graph.extractBest(id, CostModel{});
    auto field = dynamic_cast<const FieldAccessTypedExpr*>(result.get());
    ASSERT_NE(field, nullptr);
    EXPECT_TRUE(field->isInputColumn());
    EXPECT_EQ(field->name(), "a");
    ASSERT_EQ(field->inputs().size(), expression->inputs().size());
    if (!field->inputs().empty()) {
      EXPECT_NE(
          dynamic_cast<const InputTypedExpr*>(field->inputs()[0].get()),
          nullptr);
      EXPECT_EQ(field->inputs()[0]->type(), input->type());
    }
  }
}

TEST_F(EGraphTest, positionalDereferenceCannotAliasNumericFieldName) {
  // Field "1" is at index 0: the old name="1" encoding selected that field
  // instead of preserving a positional dereference of index 1 ("payload").
  auto row = makeField("r", ROW({"1", "payload"}, {BIGINT(), BIGINT()}));
  auto positional = std::make_shared<DereferenceTypedExpr>(BIGINT(), row, 1);
  auto named = std::make_shared<FieldAccessTypedExpr>(BIGINT(), row, "1");
  EGraph graph(config_);
  auto id = graph.addExpression(positional);
  EXPECT_NE(id, graph.addExpression(named));
  auto result = graph.extractBest(id, CostModel{});
  auto dereference = dynamic_cast<const DereferenceTypedExpr*>(result.get());
  ASSERT_NE(dereference, nullptr);
  EXPECT_EQ(dereference->index(), 1);
  EXPECT_EQ(dereference->name(), "payload");
  auto namedResult = graph.extractBest(graph.addExpression(named), CostModel{});
  auto namedField =
      dynamic_cast<const FieldAccessTypedExpr*>(namedResult.get());
  ASSERT_NE(namedField, nullptr);
  EXPECT_EQ(namedField->name(), "1");
  EXPECT_FALSE(namedField->isInputColumn());
}

TEST_F(EGraphTest, missingMetadataKeepsCallsAndParentsOpaque) {
  EGraph graph(EGraphConfig{});
  const auto first = makeCall("unknown_udf", {}, BOOLEAN());
  const auto second = makeCall("unknown_udf", {}, BOOLEAN());
  EXPECT_NE(graph.addExpression(first), graph.addExpression(second));
  const auto parent = makeCall("and", {first, second}, BOOLEAN());
  const auto root = graph.addExpression(parent);
  EXPECT_EQ(graph.getClass(root).nodes()[0].kind, ENode::Kind::Opaque);
  EXPECT_EQ(graph.extractBest(root, CostModel{}), parent);
}

TEST_F(EGraphTest, volatileOccurrencesAndTheirParentsStayIndependent) {
  EGraph graph(config_);
  const auto first = makeCall("rand", {}, DOUBLE());
  const auto second = makeCall("rand", {}, DOUBLE());
  EXPECT_NE(graph.addExpression(first), graph.addExpression(second));
  const auto parent = makeCall("+", {first, second}, DOUBLE());
  const auto root = graph.addExpression(parent);
  EXPECT_EQ(graph.getClass(root).nodes()[0].kind, ENode::Kind::Opaque);
  EXPECT_EQ(graph.extractBest(root, CostModel{}), parent);
}

TEST_F(EGraphTest, signedZeroConstantsRemainDistinctThroughExtraction) {
  for (const auto& type : std::vector<TypePtr>{REAL(), DOUBLE()}) {
    const auto negative = std::make_shared<ConstantTypedExpr>(
        type, type == REAL() ? variant(-0.0f) : variant(-0.0));
    const auto positive = std::make_shared<ConstantTypedExpr>(
        type, type == REAL() ? variant(0.0f) : variant(0.0));
    EGraph graph(config_);
    EXPECT_NE(graph.addExpression(negative), graph.addExpression(positive));
    const auto expression = makeCall("coalesce", {negative, positive}, type);
    const auto result =
        graph.extractBest(graph.addExpression(expression), CostModel{});
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->inputs().size(), 2);
    for (size_t i = 0; i < 2; ++i) {
      const auto* constant =
          dynamic_cast<const ConstantTypedExpr*>(result->inputs()[i].get());
      ASSERT_NE(constant, nullptr);
      const double value = type == REAL() ? constant->value().value<float>()
                                          : constant->value().value<double>();
      EXPECT_EQ(value, 0.0);
      EXPECT_EQ(std::signbit(value), i == 0);
    }
  }
}

TEST_F(EGraphTest, nanPayloadsRemainDistinctAndRoundTripExactly) {
  for (const auto& type : std::vector<TypePtr>{REAL(), DOUBLE()}) {
    auto literal = [&](uint64_t payload) {
      if (type == REAL()) {
        const uint32_t bits = 0x7fc00000u | static_cast<uint32_t>(payload);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return std::make_shared<ConstantTypedExpr>(type, variant(value));
      }
      const uint64_t bits = 0x7ff8000000000000ULL | payload;
      double value;
      std::memcpy(&value, &bits, sizeof(value));
      return std::make_shared<ConstantTypedExpr>(type, variant(value));
    };
    EGraph graph(config_);
    const auto first = literal(1), second = literal(2);
    const auto firstId = graph.addExpression(first);
    const auto secondId = graph.addExpression(second);
    EXPECT_NE(firstId, secondId);
    EXPECT_EQ(firstId, graph.addExpression(literal(1)));
    for (const auto& original : {first, second}) {
      const auto result =
          graph.extractBest(graph.addExpression(original), CostModel{});
      const auto* constant =
          dynamic_cast<const ConstantTypedExpr*>(result.get());
      ASSERT_NE(constant, nullptr);
      if (type == REAL()) {
        const float expected = original->value().value<float>();
        const float actual = constant->value().value<float>();
        EXPECT_TRUE(std::isnan(actual));
        EXPECT_EQ(std::memcmp(&expected, &actual, sizeof(actual)), 0);
      } else {
        const double expected = original->value().value<double>();
        const double actual = constant->value().value<double>();
        EXPECT_TRUE(std::isnan(actual));
        EXPECT_EQ(std::memcmp(&expected, &actual, sizeof(actual)), 0);
      }
    }
  }
}
