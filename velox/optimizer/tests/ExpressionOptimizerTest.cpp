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
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "velox/common/base/Exceptions.h"
#include "velox/common/memory/Memory.h"
#include "velox/optimizer/ExpressionOptimizer.h"
#include "velox/optimizer/RewriteRule.h"
#include "velox/optimizer/tests/OptimizerTestConfig.h"
#include "velox/vector/VariantToVector.h"

DECLARE_bool(velox_optimizer_enabled);

using namespace facebook::velox;
using namespace facebook::velox::core;
using namespace facebook::velox::optimizer;

namespace {
TypedExprPtr equalityCase() {
  auto subject = std::make_shared<FieldAccessTypedExpr>(VARCHAR(), "x");
  std::vector<TypedExprPtr> inputs;
  for (int i = 0; i < 8; ++i) {
    auto literal = std::make_shared<ConstantTypedExpr>(
        VARCHAR(), variant("key_" + std::to_string(i)));
    inputs.push_back(
        std::make_shared<CallTypedExpr>(
            BOOLEAN(), std::vector<TypedExprPtr>{subject, literal}, "equalto"));
    inputs.push_back(literal);
  }
  inputs.push_back(
      std::make_shared<ConstantTypedExpr>(VARCHAR(), variant("other")));
  return std::make_shared<CallTypedExpr>(
      VARCHAR(), std::move(inputs), "switch");
}

class CountingRule : public RewriteRule {
 public:
  explicit CountingRule(size_t& calls) : calls_(calls) {}
  std::string name() const override {
    return "TestCountingRule";
  }
  size_t apply(EGraph&) override {
    ++calls_;
    return 0;
  }

 private:
  size_t& calls_;
};

class ActionRule : public RewriteRule {
 public:
  explicit ActionRule(std::function<size_t(EGraph&)> action)
      : action_(std::move(action)) {}
  std::string name() const override {
    return "TestActionRule";
  }
  size_t apply(EGraph& graph) override {
    return action_(graph);
  }

 private:
  std::function<size_t(EGraph&)> action_;
};

std::unique_ptr<RewriteRule>
replacementRule(ENode::Kind kind, std::string name, ENode replacement) {
  return std::make_unique<ActionRule>(
      [kind,
       name = std::move(name),
       replacement = std::move(replacement),
       applied = false](EGraph& graph) mutable -> size_t {
        if (applied) {
          return 0;
        }
        for (uint32_t id = 0; id < graph.numClasses(); ++id) {
          for (const auto& node : graph.getClass(id).nodes()) {
            if (node.kind == kind && node.name == name) {
              applied = true;
              const auto alternative = graph.canonicalize(replacement);
              graph.merge(id, alternative);
              return 1;
            }
          }
        }
        return 0;
      });
}

template <typename Error>
void expectRuleFailure(std::function<size_t(EGraph&)> action) {
  ExpressionOptimizer optimizer(testGraphConfig());
  optimizer.registerRule(std::make_unique<ActionRule>(std::move(action)));
  EXPECT_THROW(optimizer.optimize(equalityCase()), Error);
}

TEST(ExpressionOptimizerTest, emptyPassPreservesCaseLambdaAndUnknownCall) {
  const auto expression = equalityCase();
  const auto lambda =
      std::make_shared<LambdaTypedExpr>(ROW({"x"}, {VARCHAR()}), expression);
  const auto unknown = std::make_shared<CallTypedExpr>(
      VARCHAR(), std::vector<TypedExprPtr>{}, "unknown_udf");
  ExpressionOptimizer optimizer;
  EXPECT_FALSE(optimizer.hasActiveRules());
  EXPECT_EQ(optimizer.optimize(expression), expression);
  EXPECT_EQ(optimizer.optimize(lambda), lambda);
  EXPECT_EQ(optimizer.optimize(unknown), unknown);
  EXPECT_EQ(optimizer.optimize(nullptr), nullptr);
}

TEST(ExpressionOptimizerTest, batchPreservesSharedPointersAndNullEntries) {
  const auto expression = equalityCase();
  const std::vector<TypedExprPtr> inputs{expression, expression, nullptr};
  ExpressionOptimizer optimizer;
  EXPECT_EQ(optimizer.optimize(inputs), inputs);
}

TEST(ExpressionOptimizerTest, masterSwitchBypassesRegisteredRules) {
  gflags::FlagSaver flags;
  FLAGS_velox_optimizer_enabled = false;
  size_t calls = 0;
  EGraphConfig config;
  config.functionDeterminismCheck = [](const auto&, const auto&, const auto&) {
    return true;
  };
  ExpressionOptimizer optimizer(config);
  optimizer.registerRule(std::make_unique<CountingRule>(calls));
  EXPECT_FALSE(optimizer.hasActiveRules());
  const auto expression = equalityCase();
  EXPECT_EQ(optimizer.optimize(expression), expression);
  EXPECT_EQ(calls, 0);
}

TEST(
    ExpressionOptimizerTest,
    registeredRuleIsAppliedAndCheaperResultExtracted) {
  gflags::FlagSaver flags;
  FLAGS_velox_optimizer_enabled = true;
  const auto expression = std::make_shared<CallTypedExpr>(
      BIGINT(),
      std::vector<TypedExprPtr>{
          std::make_shared<ConstantTypedExpr>(BIGINT(), variant(int64_t{1})),
          std::make_shared<ConstantTypedExpr>(BIGINT(), variant(int64_t{2}))},
      "add");
  ENode folded(ENode::Kind::Constant, BIGINT(), "", {});
  folded.constantValue = variant(int64_t{3});
  ExpressionOptimizer optimizer(testGraphConfig());
  optimizer.registerRule(replacementRule(ENode::Kind::Call, "add", folded));
  ASSERT_TRUE(optimizer.hasActiveRules());
  const auto result = optimizer.optimize(expression);
  ASSERT_NE(result, expression);
  const auto* constant = dynamic_cast<const ConstantTypedExpr*>(result.get());
  ASSERT_NE(constant, nullptr);
  EXPECT_EQ(constant->type(), BIGINT());
  EXPECT_EQ(constant->value().value<int64_t>(), 3);
}

TEST(ExpressionOptimizerTest, mergeOnlyRuleExtractsCheaperExistingExpression) {
  gflags::FlagSaver flags;
  FLAGS_velox_optimizer_enabled = true;
  const auto value = std::make_shared<FieldAccessTypedExpr>(BIGINT(), "x");
  const auto expression = std::make_shared<CallTypedExpr>(
      BIGINT(), std::vector<TypedExprPtr>{value, value}, "coalesce");
  size_t calls = 0;
  ExpressionOptimizer optimizer(testGraphConfig());
  optimizer.registerRule(
      std::make_unique<ActionRule>(
          [&calls, applied = false](EGraph& graph) mutable -> size_t {
            ++calls;
            if (!applied) {
              for (uint32_t id = 0; id < graph.numClasses(); ++id) {
                for (const auto& node : graph.getClass(id).nodes()) {
                  if (node.kind == ENode::Kind::Call &&
                      node.name == "coalesce") {
                    const auto child = node.children[0];
                    applied = true;
                    graph.merge(id, child);
                    return 0; // Existing classes only; no new e-nodes.
                  }
                }
              }
            }
            return 0;
          }));
  const auto result = optimizer.optimize(expression);
  const auto* field = dynamic_cast<const FieldAccessTypedExpr*>(result.get());
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(field->name(), "x");
  EXPECT_EQ(calls, 2); // The merge must permit a follow-up saturation pass.
}

TEST(ExpressionOptimizerTest, vectorConstantsWithoutOriginalRetainValues) {
  memory::MemoryManager::testingSetInstance({});
  auto pool = memory::memoryManager()->addLeafPool("enodeOwnedVectorConst");
  const auto check = [&](const auto& vectorFor) {
    auto first = std::make_shared<ConstantTypedExpr>(vectorFor(1));
    auto second = std::make_shared<ConstantTypedExpr>(vectorFor(2));
    const std::weak_ptr<const ITypedExpr> firstOwner = first;
    const std::weak_ptr<const ITypedExpr> secondOwner = second;
    const auto firstNode = ENode::fromTypedExpr(*first, {});
    const auto secondNode = ENode::fromTypedExpr(*second, {});
    ASSERT_NE(firstNode.originalExpr, nullptr);
    ASSERT_NE(secondNode.originalExpr, nullptr);
    EXPECT_FALSE(firstNode == secondNode);
    EGraph graph;
    const auto firstId = graph.canonicalize(firstNode);
    const auto secondId = graph.canonicalize(secondNode);
    EXPECT_NE(firstId, secondId);
    first.reset();
    second.reset();
    EXPECT_TRUE(firstOwner.expired());
    EXPECT_TRUE(secondOwner.expired());
    for (const auto& [id, value] : std::vector<std::pair<uint32_t, int64_t>>{
             {firstId, 1}, {secondId, 2}}) {
      const auto result = graph.extractBest(id, CostModel{});
      const auto* constant =
          dynamic_cast<const ConstantTypedExpr*>(result.get());
      ASSERT_NE(constant, nullptr);
      ASSERT_TRUE(constant->hasValueVector());
      EXPECT_FALSE(constant->valueVector()->isNullAt(0));
      const auto expected = vectorFor(value);
      EXPECT_TRUE(constant->valueVector()->equalValueAt(expected.get(), 0, 0));
    }
  };
  const auto arrayFor = [&](int64_t value) {
    return variantArrayToVector(ARRAY(BIGINT()), {variant(value)}, pool.get());
  };
  check(arrayFor);
  check([&](int64_t value) {
    const auto keys = arrayFor(1);
    const auto values = arrayFor(value);
    return std::make_shared<MapVector>(
        pool.get(),
        MAP(BIGINT(), BIGINT()),
        nullptr,
        1,
        keys->offsets(),
        keys->sizes(),
        keys->elements(),
        values->elements());
  });
}

TEST(
    ExpressionOptimizerTest,
    unselectedUnsupportedAlternativeIsNotReconstructed) {
  gflags::FlagSaver flags;
  FLAGS_velox_optimizer_enabled = true;
  ExpressionOptimizer optimizer(testGraphConfig());
  optimizer.registerRule(replacementRule(
      ENode::Kind::Switch,
      "switch",
      ENode(ENode::Kind::Lambda, VARCHAR(), "test_unsupported", {})));
  ENode cheaper(ENode::Kind::Constant, VARCHAR(), "", {});
  cheaper.constantValue = variant("selected");
  optimizer.registerRule(
      replacementRule(ENode::Kind::Switch, "switch", cheaper));
  const auto result = optimizer.optimize(equalityCase());
  const auto* constant = dynamic_cast<const ConstantTypedExpr*>(result.get());
  ASSERT_NE(constant, nullptr);
  EXPECT_EQ(constant->value(), variant("selected"));
}

TEST(ExpressionOptimizerTest, unsupportedReconstructionReturnsOriginal) {
  gflags::FlagSaver flags;
  FLAGS_velox_optimizer_enabled = true;
  ExpressionOptimizer optimizer(testGraphConfig());
  // Force the extractor's explicitly unsupported Lambda path with a cheaper
  // test-only alternative; the input itself remains a supported CASE tree.
  optimizer.registerRule(replacementRule(
      ENode::Kind::Switch,
      "switch",
      ENode(ENode::Kind::Lambda, VARCHAR(), "test_unsupported", {})));
  const auto expression = equalityCase();
  EXPECT_EQ(optimizer.optimize(expression), expression);
}

TEST(ExpressionOptimizerTest, callbackAndResourceErrorsPropagate) {
  gflags::FlagSaver flags;
  FLAGS_velox_optimizer_enabled = true;
  auto config = testGraphConfig();
  config.functionDeterminismCheck =
      [](const auto&, const auto&, const auto&) -> bool {
    throw std::runtime_error("metadata failed");
  };
  size_t calls = 0;
  ExpressionOptimizer optimizer(config);
  optimizer.registerRule(std::make_unique<CountingRule>(calls));
  EXPECT_THROW(optimizer.optimize(equalityCase()), std::runtime_error);
  EXPECT_EQ(calls, 0);
  expectRuleFailure<std::runtime_error>([](EGraph&) -> size_t {
    throw std::runtime_error("rule callback failed");
  });
  expectRuleFailure<std::bad_alloc>(
      [](EGraph&) -> size_t { throw std::bad_alloc(); });
  expectRuleFailure<VeloxRuntimeError>(
      [](EGraph&) -> size_t { VELOX_FAIL("rule invariant failed"); });
  // Even an UNSUPPORTED exception from a callback is outside the narrow
  // reconstruction fallback boundary.
  expectRuleFailure<VeloxUserError>(
      [](EGraph&) -> size_t { VELOX_UNSUPPORTED("rule callback declined"); });
  // A malformed, cheap test-only Input wins extraction, then fails its
  // childless-node invariant inside the reconstruction fallback boundary.
  ExpressionOptimizer invalidInputOptimizer(testGraphConfig());
  invalidInputOptimizer.registerRule(replacementRule(
      ENode::Kind::Switch,
      "switch",
      ENode(ENode::Kind::Input, VARCHAR(), "", {0})));
  EXPECT_THROW(
      invalidInputOptimizer.optimize(equalityCase()), VeloxRuntimeError);
}
} // namespace
