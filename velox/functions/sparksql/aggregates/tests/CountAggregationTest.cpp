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
#include "velox/exec/PlanNodeStats.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/functions/lib/aggregates/tests/utils/AggregationTestBase.h"
#include "velox/functions/sparksql/aggregates/Register.h"

using namespace facebook::velox::exec::test;
using namespace facebook::velox::functions::aggregate::test;

namespace facebook::velox::functions::aggregate::sparksql::test {

namespace {

// Tests for the Spark-side CountAggregate (sparksql/aggregates/CountAggregate
// .cpp). Mirrors the Presto-side tests where the behaviour is identical and
// adds dedicated coverage for the optimisation fast paths:
//   - toIntermediate bitmap fast path (FLAT + nulls, FLAT no-null, CONSTANT)
//   - addRawInput FLAT fast path (no-null, with-null, partial selection)
//   - addSingleGroupRawInput popcount path
//   - dictionary / lazy fallback (no fast path)
//   - high-cardinality GROUP BY triggering abandonPartialAggregation, which
//     is the path that actually routes through CountAggregate::toIntermediate
class CountAggregationTest : public AggregationTestBase {
 protected:
  void SetUp() override {
    AggregationTestBase::SetUp();
    // Register Spark aggregates under the "spark_" prefix so they don't
    // clash with the Presto count that AggregationTestBase brings in.
    registerAggregateFunctions("spark_", true);
  }

  RowTypePtr rowType_{
      ROW({"c0", "c1", "c2", "c3", "c4", "c5", "c6", "c7"},
          {BIGINT(),
           SMALLINT(),
           INTEGER(),
           BIGINT(),
           REAL(),
           DOUBLE(),
           VARCHAR(),
           TINYINT()})};
};

// ---------------------------------------------------------------------------
// Mirror of the Presto tests: end-to-end semantics for count(*) / count(col).
// ---------------------------------------------------------------------------
TEST_F(CountAggregationTest, count) {
  auto vectors = makeVectors(rowType_, 10, 100);
  createDuckDbTable(vectors);

  testAggregations(
      vectors, {}, {"spark_count()"}, "SELECT count(1) FROM tmp");

  testAggregations(
      vectors, {}, {"spark_count(1)"}, "SELECT count(1) FROM tmp");

  // count over a column with nulls; only non-null rows should be counted.
  testAggregations(
      vectors, {}, {"spark_count(c1)"}, "SELECT count(c1) FROM tmp");

  // count over zero rows; the result should be 0, not null.
  testAggregations(
      [&](PlanBuilder& builder) {
        builder.values(vectors).filter("c0 % 3 > 5");
      },
      {},
      {"spark_count(c1)"},
      "SELECT count(c1) FROM tmp WHERE c0 % 3 > 5");

  testAggregations(
      [&](PlanBuilder& builder) {
        builder.values(vectors).project({"c0 % 10 AS c0_mod_10", "c1"});
      },
      {"c0_mod_10"},
      {"spark_count(1)"},
      "SELECT c0 % 10, count(1) FROM tmp GROUP BY 1");

  testAggregations(
      [&](PlanBuilder& builder) {
        builder.values(vectors).project({"c0 % 10 AS c0_mod_10", "c7"});
      },
      {"c0_mod_10"},
      {"spark_count(c7)"},
      "SELECT c0 % 10, count(c7) FROM tmp GROUP BY 1");
}

TEST_F(CountAggregationTest, mask) {
  std::vector<RowVectorPtr> data;
  constexpr int32_t kNumBatches = 10;
  constexpr int32_t kRowsInBatch = 100;
  for (auto counter = 0; counter < kNumBatches; ++counter) {
    data.push_back(makeRowVector(
        {"k", "c", "m"},
        {makeFlatVector<int64_t>(
             kRowsInBatch, [](auto row) { return row / 10; }),
         makeFlatVector<int64_t>(
             kRowsInBatch,
             [](auto row) { return row; },
             [](auto row) { return row % 3 == 0; }),
         makeFlatVector<bool>(kRowsInBatch, [&](auto row) {
           return counter % 3 == 0 ? false
               : counter % 3 == 1  ? false
                                   : row % 2 == 0;
         })}));
  }

  createDuckDbTable(data);

  auto plan = PlanBuilder()
                  .values(data)
                  .singleAggregation({}, {"spark_count(c)"}, {"m"})
                  .planNode();
  assertQuery(plan, "SELECT count(c) FILTER (where m) FROM tmp");

  plan = PlanBuilder()
             .values(data)
             .partialAggregation({}, {"spark_count(c)"}, {"m"})
             .finalAggregation()
             .planNode();
  assertQuery(plan, "SELECT count(c) FILTER (where m) FROM tmp");

  // count(*) with mask.
  plan = PlanBuilder()
             .values(data)
             .singleAggregation({}, {"spark_count()"}, {"m"})
             .planNode();
  assertQuery(plan, "SELECT count(1) FILTER (where m) FROM tmp");

  plan = PlanBuilder()
             .values(data)
             .partialAggregation({}, {"spark_count()"}, {"m"})
             .finalAggregation()
             .planNode();
  assertQuery(plan, "SELECT count(1) FILTER (where m) FROM tmp");

  // This GROUP BY variant forces abandonPartialAggregation, which is the
  // path that ends up calling CountAggregate::toIntermediate (the main
  // optimisation introduced in this patch).
  core::PlanNodeId partialNodeId;
  plan = PlanBuilder()
             .values(data)
             .partialAggregation({"k"}, {"spark_count(c)"}, {"m"})
             .capturePlanNodeId(partialNodeId)
             .finalAggregation()
             .planNode();
  auto task =
      AssertQueryBuilder(plan, duckDbQueryRunner_)
          .maxDrivers(1)
          .config(core::QueryConfig::kAbandonPartialAggregationMinRows, "1")
          .config(core::QueryConfig::kAbandonPartialAggregationMinPct, "0")
          .assertResults(
              "SELECT k, count(c) FILTER (where m) FROM tmp GROUP BY k");
  auto taskStats = toPlanStats(task->taskStats());
  auto partialStats = taskStats.at(partialNodeId).customStats;
  EXPECT_LT(0, partialStats.at("abandonedPartialAggregation").count);
}

TEST_F(CountAggregationTest, distinct) {
  auto data = makeRowVector({
      makeFlatVector<int16_t>({1, 2, 1, 2, 1, 1, 2, 2}),
      makeFlatVector<int32_t>({1, 1, 1, 2, 1, 1, 1, 2}),
      makeNullableFlatVector<int64_t>(
          {std::nullopt, 1, std::nullopt, 2, std::nullopt, 1, std::nullopt, 1}),
      makeNullConstant(TypeKind::DOUBLE, 8),
  });
  createDuckDbTable({data});

  auto testGlobal = [&](const std::string& input) {
    auto plan = PlanBuilder()
                    .values({data})
                    .singleAggregation(
                        {}, {fmt::format("spark_count(distinct {})", input)})
                    .planNode();
    AssertQueryBuilder(plan, duckDbQueryRunner_)
        .assertResults(
            fmt::format("SELECT count(distinct {}) FROM tmp", input));
  };

  testGlobal("c1");
  testGlobal("c2");
  testGlobal("c3");

  // Global. Empty input.
  auto plan = PlanBuilder()
                  .values({makeRowVector(ROW({"c0"}, {BIGINT()}), 0)})
                  .singleAggregation({}, {"spark_count(distinct c0)"})
                  .planNode();
  AssertQueryBuilder(plan, duckDbQueryRunner_).assertResults("SELECT 0");

  auto testGroupBy = [&](const std::string& input) {
    auto plan = PlanBuilder()
                    .values({data})
                    .singleAggregation(
                        {"c0"},
                        {fmt::format("spark_count(distinct {})", input)})
                    .planNode();
    AssertQueryBuilder(plan, duckDbQueryRunner_)
        .assertResults(fmt::format(
            "SELECT c0, count(distinct {}) FROM tmp GROUP BY 1", input));
  };

  testGroupBy("c1");
  testGroupBy("c2");
  testGroupBy("c3");
}

TEST_F(CountAggregationTest, distinctMask) {
  auto data = makeRowVector({
      makeFlatVector<int16_t>({1, 2, 1, 2, 1, 1, 2, 2}),
      makeFlatVector<bool>(
          {true, false, false, true, false, true, false, true}),
      makeFlatVector<int32_t>({1, -1, -1, 2, -1, 1, -1, 1}),
  });
  createDuckDbTable({data});

  auto plan = PlanBuilder()
                  .values({data})
                  .singleAggregation({}, {"spark_count(distinct c2)"}, {"c1"})
                  .planNode();
  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults("SELECT count(distinct c2) FILTER (WHERE c1) FROM tmp");

  plan = PlanBuilder()
             .values({data})
             .singleAggregation({"c0"}, {"spark_count(distinct c2)"}, {"c1"})
             .planNode();
  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults(
          "SELECT c0, count(distinct c2) FILTER (WHERE c1) FROM tmp GROUP BY 1");
}

// ---------------------------------------------------------------------------
// Targeted coverage for the new optimisation fast paths.
// ---------------------------------------------------------------------------

// FLAT input with no null bitmap (rawNulls() == nullptr).
// Hits the "addOneToEachGroupBulk" / "fill_n(1)" branch in both addRawInput
// and toIntermediate.
TEST_F(CountAggregationTest, flatNoNullFastPath) {
  constexpr int32_t kSize = 10'003; // not a multiple of 64 -> exercise tail
  auto vectors = {makeRowVector({
      makeFlatVector<int32_t>(kSize, [](auto i) { return i % 7; }),
      makeFlatVector<int64_t>(kSize, [](auto i) { return i; }),
  })};
  createDuckDbTable(vectors);

  testAggregations(
      vectors,
      {},
      {"spark_count(c1)"},
      "SELECT count(c1) FROM tmp");

  testAggregations(
      vectors,
      {"c0"},
      {"spark_count(c1)"},
      "SELECT c0, count(c1) FROM tmp GROUP BY 1");
}

// FLAT input with nulls. Exercises bits::forEachWord + __builtin_ctzll
// enumeration in both the all-selected and partially-selected sub-branches.
TEST_F(CountAggregationTest, flatWithNullsFastPath) {
  constexpr int32_t kSize = 8'191;
  auto vectors = {makeRowVector({
      makeFlatVector<int32_t>(kSize, [](auto i) { return i % 11; }),
      // ~50% nulls scattered with a non-power-of-two stride to avoid
      // pathologically aligned bitmap words.
      makeFlatVector<int64_t>(
          kSize,
          [](auto i) { return i; },
          [](auto i) { return (i * 13 + 7) % 5 == 0; }),
  })};
  createDuckDbTable(vectors);

  // count(*) -- no nulls considered, hits addOneToEachGroupBulk.
  testAggregations(
      vectors, {}, {"spark_count()"}, "SELECT count(1) FROM tmp");

  // count(col) -- hits addOneToEachNonNullGroupBulk (group-by) and the
  // popcount path inside addSingleGroupRawInput (global).
  testAggregations(
      vectors,
      {},
      {"spark_count(c1)"},
      "SELECT count(c1) FROM tmp");

  testAggregations(
      vectors,
      {"c0"},
      {"spark_count(c1)"},
      "SELECT c0, count(c1) FROM tmp GROUP BY 1");

  // Partial selectivity via a filter -- exercises the
  // (selectedBits & rawNulls) branch of the bitmap loop.
  testAggregations(
      [&](PlanBuilder& builder) {
        builder.values(vectors).filter("c0 % 2 = 0");
      },
      {"c0"},
      {"spark_count(c1)"},
      "SELECT c0, count(c1) FROM tmp WHERE c0 % 2 = 0 GROUP BY 1");
}

// CONSTANT input. Exercises the encoding == CONSTANT branch in addRawInput,
// addSingleGroupRawInput and toIntermediate, for both null and non-null
// constants.
TEST_F(CountAggregationTest, constantFastPath) {
  constexpr int32_t kSize = 4'096;
  auto input = makeRowVector(
      {"k", "non_null_c", "null_c"},
      {makeFlatVector<int32_t>(kSize, [](auto i) { return i % 5; }),
       makeConstant<int64_t>(42, kSize),
       makeNullConstant(TypeKind::BIGINT, kSize)});
  createDuckDbTable({input});

  // Non-null constant -> every selected row counts.
  testAggregations(
      {input},
      {},
      {"spark_count(non_null_c)"},
      "SELECT count(non_null_c) FROM tmp");
  testAggregations(
      {input},
      {"k"},
      {"spark_count(non_null_c)"},
      "SELECT k, count(non_null_c) FROM tmp GROUP BY 1");

  // All-null constant -> result is 0.
  testAggregations(
      {input},
      {},
      {"spark_count(null_c)"},
      "SELECT count(null_c) FROM tmp");
  testAggregations(
      {input},
      {"k"},
      {"spark_count(null_c)"},
      "SELECT k, count(null_c) FROM tmp GROUP BY 1");
}

// Dictionary-encoded input. Falls through to the DecodedVector slow path in
// addRawInput / addSingleGroupRawInput / toIntermediate. Verifies the
// fallback still produces correct results.
TEST_F(CountAggregationTest, dictionaryFallback) {
  constexpr int32_t kSize = 2'048;
  auto baseValues = makeNullableFlatVector<int64_t>([&]() {
    std::vector<std::optional<int64_t>> values;
    values.reserve(kSize);
    for (int32_t i = 0; i < kSize; ++i) {
      values.push_back(i % 4 == 0 ? std::nullopt : std::optional<int64_t>(i));
    }
    return values;
  }());

  // Reverse-index dictionary -- not the identity mapping, so the underlying
  // base vector cannot be peeled back to FLAT.
  BufferPtr indices = makeIndicesInReverse(kSize);
  auto dictValues = BaseVector::wrapInDictionary(
      /*nulls=*/nullptr, indices, kSize, baseValues);

  auto keys = makeFlatVector<int32_t>(kSize, [](auto i) { return i % 6; });
  auto input = makeRowVector({"k", "v"}, {keys, dictValues});
  createDuckDbTable({input});

  testAggregations(
      {input}, {}, {"spark_count(v)"}, "SELECT count(v) FROM tmp");
  testAggregations(
      {input},
      {"k"},
      {"spark_count(v)"},
      "SELECT k, count(v) FROM tmp GROUP BY 1");
}

// Stress the toIntermediate path. Uses a high-cardinality GROUP BY key with
// kAbandonPartialAggregationMinRows=1 / kAbandonPartialAggregationMinPct=0
// so the partial-agg hash table is abandoned and the engine routes per
// batch through CountAggregate::toIntermediate. count(*) exercises the
// args.empty() branch, count(col_with_nulls) exercises the FLAT-with-nulls
// branch.
TEST_F(CountAggregationTest, toIntermediateOnAbandonedPartialAggregation) {
  std::vector<RowVectorPtr> batches;
  constexpr int32_t kNumBatches = 8;
  constexpr int32_t kRowsInBatch = 1'024;
  for (int32_t b = 0; b < kNumBatches; ++b) {
    batches.push_back(makeRowVector(
        {"k", "v"},
        {makeFlatVector<int64_t>(
             kRowsInBatch,
             [&](auto row) {
               // Every row gets a unique key across the whole input so the
               // partial-agg hash table cannot benefit from de-duplication.
               return static_cast<int64_t>(b) * kRowsInBatch + row;
             }),
         makeFlatVector<int64_t>(
             kRowsInBatch,
             [](auto row) { return row; },
             [](auto row) { return row % 3 == 0; })}));
  }
  createDuckDbTable(batches);

  auto runWithAbandon = [&](const std::string& aggExpr,
                            const std::string& duckSql) {
    core::PlanNodeId partialNodeId;
    auto plan = PlanBuilder()
                    .values(batches)
                    .partialAggregation({"k"}, {aggExpr})
                    .capturePlanNodeId(partialNodeId)
                    .finalAggregation()
                    .planNode();
    auto task = AssertQueryBuilder(plan, duckDbQueryRunner_)
                    .maxDrivers(1)
                    .config(
                        core::QueryConfig::kAbandonPartialAggregationMinRows,
                        "1")
                    .config(
                        core::QueryConfig::kAbandonPartialAggregationMinPct,
                        "0")
                    .assertResults(duckSql);
    auto taskStats = toPlanStats(task->taskStats());
    auto partialStats = taskStats.at(partialNodeId).customStats;
    // Sanity check: the path we care about really did fire.
    EXPECT_LT(0, partialStats.at("abandonedPartialAggregation").count)
        << "abandonPartialAggregation did not trigger for " << aggExpr;
  };

  runWithAbandon(
      "spark_count(v)", "SELECT k, count(v) FROM tmp GROUP BY k");
  runWithAbandon(
      "spark_count()", "SELECT k, count(1) FROM tmp GROUP BY k");
}

// All-null input column -> count == 0 in every group, exercising the
// "rawNulls all zero" worst case of the bitmap loop.
TEST_F(CountAggregationTest, allNullColumn) {
  constexpr int32_t kSize = 1'000;
  auto input = makeRowVector(
      {"k", "v"},
      {makeFlatVector<int32_t>(kSize, [](auto i) { return i % 4; }),
       makeAllNullFlatVector<int64_t>(kSize)});
  createDuckDbTable({input});

  testAggregations(
      {input}, {}, {"spark_count(v)"}, "SELECT count(v) FROM tmp");
  testAggregations(
      {input},
      {"k"},
      {"spark_count(v)"},
      "SELECT k, count(v) FROM tmp GROUP BY 1");
}

// Single row + single group -- catches off-by-one mistakes in the tail
// handling of forEachWord / countBits.
TEST_F(CountAggregationTest, singleRow) {
  auto nonNull = makeRowVector(
      {"k", "v"},
      {makeFlatVector<int32_t>(std::vector<int32_t>{0}),
       makeFlatVector<int64_t>(std::vector<int64_t>{7})});
  createDuckDbTable({nonNull});
  testAggregations(
      {nonNull}, {}, {"spark_count(v)"}, "SELECT count(v) FROM tmp");

  auto nullOnly = makeRowVector(
      {"k", "v"},
      {makeFlatVector<int32_t>(std::vector<int32_t>{0}),
       makeNullableFlatVector<int64_t>({std::nullopt})});
  createDuckDbTable({nullOnly});
  testAggregations(
      {nullOnly}, {}, {"spark_count(v)"}, "SELECT count(v) FROM tmp");
}

// count(unknown) should yield 0 (all values are null by definition).
TEST_F(CountAggregationTest, unknownType) {
  constexpr int kSize = 10;
  auto input = makeRowVector({
      makeFlatVector<int32_t>(kSize, [](auto i) { return i % 2; }),
      makeAllNullFlatVector<UnknownValue>(kSize),
  });

  auto globalPlan = PlanBuilder()
                        .values({input})
                        .singleAggregation({}, {"spark_count(c1)"})
                        .planNode();
  AssertQueryBuilder(globalPlan).assertResults(
      makeRowVector({makeConstant<int64_t>(0, 1)}));

  auto groupedPlan = PlanBuilder()
                         .values({input})
                         .singleAggregation({"c0"}, {"spark_count(c1)"})
                         .planNode();
  AssertQueryBuilder(groupedPlan).assertResults(makeRowVector({
      makeFlatVector<int32_t>({0, 1}),
      makeFlatVector<int64_t>({0, 0}),
  }));
}

} // namespace
} // namespace facebook::velox::functions::aggregate::sparksql::test
