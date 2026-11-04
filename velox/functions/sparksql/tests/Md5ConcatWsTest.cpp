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

#include "velox/core/Expressions.h"
#include "velox/expression/SimpleFunctionRegistry.h"
#include "velox/expression/VectorFunction.h"
#include "velox/functions/sparksql/FusedMd5ConcatWs.h"
#include "velox/functions/sparksql/tests/SparkFunctionBaseTest.h"

namespace facebook::velox::functions::sparksql::test {
namespace {

constexpr const char* kEmptyMd5 = "d41d8cd98f00b204e9800998ecf8427e";
constexpr const char* kAbcMd5 = "900150983cd24fb0d6963f7d28e17f72";
constexpr const char* kInfinityMd5 = "eb2ac5b04180d8d6011a016aeb8f75b3";
constexpr const char* kAbCommaMd5 = "b345e1dc09f20fdefdea469f09167892";
// concat_ws(',', 'a', '') => 'a,' (empty string is kept; only NULL is skipped)
constexpr const char* kACommaEmptyMd5 = "3ded2184a3e467984dba5788f82cc430";
// concat_ws(',', 'a', '', 'b') => 'a,,b' (Omni 3-arg golden; not 2-arg 'a,')
constexpr const char* kAEmptyBMd5 = "9ec2fb2c0f61fa8033289ce3710b4634";
constexpr const char* kUnusedMd5 = "fd94c6a26d6b6571e8d9398446227ae8";
constexpr const char* kX55Md5 = "04364420e25c512fd958a70738aa8f72";
constexpr const char* kX56Md5 = "668a72d5ba17f08e62dabcafad6db14b";

class Md5ConcatWsTest : public SparkFunctionBaseTest {
 protected:
  std::optional<std::string> md5Varbinary(
      const std::optional<std::string>& arg) {
    return evaluateOnce<std::string>("md5(c0)", VARBINARY(), arg);
  }

  // Materialize concat_ws, then md5. evaluate("md5(cast(concat_ws...))")
  // goes through rewriteMd5ConcatWs and is not an unfused golden.
  std::shared_ptr<SimpleVector<StringView>> md5OfConcatWsUnfused(
      const std::string& concatSql,
      const RowVectorPtr& rows) {
    auto concat = evaluate<SimpleVector<StringView>>(concatSql, rows);
    return evaluate<SimpleVector<StringView>>(
        "md5(cast(c0 as varbinary))", makeRowVector({concat}));
  }
};

TEST_F(Md5ConcatWsTest, registerMd5Xor) {
  const bool hasVector = exec::getVectorFunctionSignatures("md5").has_value();
  const bool hasSimple =
      !exec::simpleFunctions().getFunctionSignatures("md5").empty();
  EXPECT_TRUE(hasVector ^ hasSimple);
  EXPECT_TRUE(hasVector || hasSimple);
  EXPECT_TRUE(
      exec::getVectorFunctionSignatures("fused_md5_concat_ws").has_value());
}

TEST_F(Md5ConcatWsTest, scalarAndLongStrings) {
  EXPECT_EQ(md5Varbinary(std::nullopt), std::nullopt);
  EXPECT_EQ(md5Varbinary(""), kEmptyMd5);
  EXPECT_EQ(md5Varbinary("abc"), kAbcMd5);
  EXPECT_EQ(md5Varbinary("Infinity"), kInfinityMd5);
  EXPECT_EQ(md5Varbinary(std::string(55, 'x')), kX55Md5);
  EXPECT_EQ(md5Varbinary(std::string(56, 'x')), kX56Md5);
}

TEST_F(Md5ConcatWsTest, mixedBatchPreservesRowOrder) {
  auto input = makeNullableFlatVector<std::string>(
      {std::string(55, 'x'),
       std::string(56, 'x'),
       std::nullopt,
       "",
       "abc",
       std::string(80, 'a')});
  auto result = evaluate<SimpleVector<StringView>>(
      "md5(cast(c0 as varbinary))", makeRowVector({input}));
  ASSERT_EQ(result->size(), 6);
  EXPECT_EQ(result->valueAt(0).str(), kX55Md5);
  EXPECT_EQ(result->valueAt(1).str(), kX56Md5);
  EXPECT_TRUE(result->isNullAt(2));
  EXPECT_EQ(result->valueAt(3).str(), kEmptyMd5);
  EXPECT_EQ(result->valueAt(4).str(), kAbcMd5);
  EXPECT_EQ(
      result->valueAt(5).str(),
      md5Varbinary(std::string(80, 'a')).value());
}

TEST_F(Md5ConcatWsTest, rewriteMd5ConcatWs) {
  auto concat = std::make_shared<core::CallTypedExpr>(
      VARCHAR(),
      std::vector<core::TypedExprPtr>{
          std::make_shared<core::FieldAccessTypedExpr>(VARCHAR(), "c0"),
          std::make_shared<core::FieldAccessTypedExpr>(VARCHAR(), "c1"),
          std::make_shared<core::FieldAccessTypedExpr>(VARCHAR(), "c2"),
      },
      "concat_ws");
  auto cast = std::make_shared<core::CastTypedExpr>(VARBINARY(), concat, false);
  auto md5 = std::make_shared<core::CallTypedExpr>(
      VARCHAR(), std::vector<core::TypedExprPtr>{cast}, "md5");

  auto rewritten = rewriteMd5ConcatWs("", md5);
  ASSERT_NE(rewritten, nullptr);
  auto fused = std::dynamic_pointer_cast<const core::CallTypedExpr>(rewritten);
  ASSERT_NE(fused, nullptr);
  EXPECT_EQ(fused->name(), "fused_md5_concat_ws");
  EXPECT_EQ(fused->inputs().size(), 3);

  auto noCast = std::make_shared<core::CallTypedExpr>(
      VARCHAR(), std::vector<core::TypedExprPtr>{concat}, "md5");
  auto rewrittenNoCast = rewriteMd5ConcatWs("", noCast);
  ASSERT_NE(rewrittenNoCast, nullptr);
  EXPECT_EQ(
      std::dynamic_pointer_cast<const core::CallTypedExpr>(rewrittenNoCast)
          ->name(),
      "fused_md5_concat_ws");

  auto plain = std::make_shared<core::CallTypedExpr>(
      VARCHAR(),
      std::vector<core::TypedExprPtr>{
          std::make_shared<core::FieldAccessTypedExpr>(VARBINARY(), "c0")},
      "md5");
  EXPECT_EQ(rewriteMd5ConcatWs("", plain), nullptr);
}

TEST_F(Md5ConcatWsTest, rewriteLandsOnFused) {
  auto c0 = makeFlatVector<std::string>({"a", "a", "unused"});
  auto c1 = makeNullableFlatVector<std::string>({"b", "", std::nullopt});
  auto rows = makeRowVector({c0, c1});

  // ExprCompiler runs rewriteMd5ConcatWs; this is not an unfused compare.
  auto fromRewrite = evaluate<SimpleVector<StringView>>(
      "md5(cast(concat_ws(',', c0, c1) as varbinary))", rows);
  auto direct = evaluate<SimpleVector<StringView>>(
      "fused_md5_concat_ws(',', c0, c1)", rows);

  ASSERT_EQ(fromRewrite->size(), 3);
  EXPECT_EQ(fromRewrite->valueAt(0).str(), kAbCommaMd5);
  EXPECT_EQ(fromRewrite->valueAt(1).str(), kACommaEmptyMd5);
  // concat_ws skips null args: concat_ws(',', 'unused', NULL) => 'unused'
  EXPECT_FALSE(fromRewrite->isNullAt(2));
  EXPECT_EQ(fromRewrite->valueAt(2).str(), kUnusedMd5);
  velox::test::assertEqualVectors(fromRewrite, direct);
}

TEST_F(Md5ConcatWsTest, fusedMatchesMaterializedConcatThenMd5) {
  auto c0 = makeFlatVector<std::string>({"a", "a", "unused"});
  auto c1 = makeNullableFlatVector<std::string>({"b", "", std::nullopt});
  auto rows = makeRowVector({c0, c1});

  auto concat = evaluate<SimpleVector<StringView>>(
      "concat_ws(',', c0, c1)", rows);
  ASSERT_EQ(concat->size(), 3);
  EXPECT_EQ(concat->valueAt(0).str(), "a,b");
  EXPECT_EQ(concat->valueAt(1).str(), "a,");
  EXPECT_EQ(concat->valueAt(2).str(), "unused");

  auto unfused = evaluate<SimpleVector<StringView>>(
      "md5(cast(c0 as varbinary))", makeRowVector({concat}));
  auto fused = evaluate<SimpleVector<StringView>>(
      "fused_md5_concat_ws(',', c0, c1)", rows);

  EXPECT_EQ(unfused->valueAt(0).str(), kAbCommaMd5);
  EXPECT_EQ(unfused->valueAt(1).str(), kACommaEmptyMd5);
  EXPECT_EQ(unfused->valueAt(2).str(), kUnusedMd5);
  velox::test::assertEqualVectors(unfused, fused);

  // Three string args: empty middle piece is kept (same as Omni a,,b golden).
  auto c2 = makeFlatVector<std::string>({"b"});
  auto aEmpty = makeFlatVector<std::string>({""});
  auto a = makeFlatVector<std::string>({"a"});
  auto three = makeRowVector({a, aEmpty, c2});
  auto unfused3 = md5OfConcatWsUnfused("concat_ws(',', c0, c1, c2)", three);
  auto fused3 = evaluate<SimpleVector<StringView>>(
      "fused_md5_concat_ws(',', c0, c1, c2)", three);
  EXPECT_EQ(unfused3->valueAt(0).str(), kAEmptyBMd5);
  velox::test::assertEqualVectors(unfused3, fused3);
}

TEST_F(Md5ConcatWsTest, allNullDataArgs) {
  auto c0 = makeNullableFlatVector<std::string>({std::nullopt});
  auto c1 = makeNullableFlatVector<std::string>({std::nullopt});
  auto rows = makeRowVector({c0, c1});

  // Spark / Velox concat_ws skips NULLs; no remaining pieces → "".
  auto concat = evaluate<SimpleVector<StringView>>(
      "concat_ws(',', c0, c1)", rows);
  ASSERT_FALSE(concat->isNullAt(0));
  EXPECT_EQ(concat->valueAt(0).str(), "");

  auto unfused = evaluate<SimpleVector<StringView>>(
      "md5(cast(c0 as varbinary))", makeRowVector({concat}));
  EXPECT_FALSE(unfused->isNullAt(0));
  EXPECT_EQ(unfused->valueAt(0).str(), kEmptyMd5);

  auto fused = evaluate<SimpleVector<StringView>>(
      "fused_md5_concat_ws(',', c0, c1)", rows);
  auto rewritten = evaluate<SimpleVector<StringView>>(
      "md5(cast(concat_ws(',', c0, c1) as varbinary))", rows);
  velox::test::assertEqualVectors(unfused, fused);
  velox::test::assertEqualVectors(unfused, rewritten);
}

TEST_F(Md5ConcatWsTest, fusedOnlySeparatorAndNullSeparator) {
  auto dummy = makeFlatVector<int32_t>({1});
  auto onlySep = evaluate<SimpleVector<StringView>>(
      "fused_md5_concat_ws(',')", makeRowVector({dummy}));
  EXPECT_EQ(onlySep->valueAt(0).str(), kEmptyMd5);

  auto fromRewrite = evaluate<SimpleVector<StringView>>(
      "md5(cast(concat_ws(',') as varbinary))", makeRowVector({dummy}));
  EXPECT_EQ(fromRewrite->valueAt(0).str(), kEmptyMd5);

  auto nullSep = evaluate<SimpleVector<StringView>>(
      "fused_md5_concat_ws(NULL::VARCHAR, 'a', 'b')", makeRowVector({dummy}));
  EXPECT_TRUE(nullSep->isNullAt(0));
}

} // namespace
} // namespace facebook::velox::functions::sparksql::test
