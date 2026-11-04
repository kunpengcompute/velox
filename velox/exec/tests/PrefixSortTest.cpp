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

#include <functional>
#include <gtest/gtest.h>

#include "velox/common/base/Exceptions.h"
#include "velox/common/testutil/TestValue.h"
#include "velox/exec/PrefixSort.h"
#include "velox/exec/SpillRadixSort.h"
#include "velox/exec/tests/utils/OperatorTestBase.h"

namespace facebook::velox::exec::prefixsort::test {
namespace {

class PrefixSortTest : public exec::test::OperatorTestBase {
 protected:
  std::vector<char*, memory::StlAllocator<char*>>
  storeRows(int numRows, const RowVectorPtr& sortedRows, RowContainer* data);

  static constexpr CompareFlags kAsc{
      true,
      true,
      false,
      CompareFlags::NullHandlingMode::kNullAsValue};

  static constexpr CompareFlags kDesc{
      true,
      false,
      false,
      CompareFlags::NullHandlingMode::kNullAsValue};

  void testPrefixSort(
      const std::vector<CompareFlags>& compareFlags,
      const RowVectorPtr& data) {
    const auto numRows = data->size();
    const auto expectedResult =
        generateExpectedResult(compareFlags, numRows, data);

    const auto rowType = asRowType(data->type());

    // Store data in a RowContainer.
    const std::vector<TypePtr> keyTypes{
        rowType->children().begin(),
        rowType->children().begin() + compareFlags.size()};
    const std::vector<TypePtr> payloadTypes{
        rowType->children().begin() + compareFlags.size(),
        rowType->children().end()};

    RowContainer rowContainer(keyTypes, payloadTypes, pool_.get());
    auto rows = storeRows(numRows, data, &rowContainer);
    const std::shared_ptr<memory::MemoryPool> sortPool =
        rootPool_->addLeafChild("prefixsort");
    const auto maxBytes = PrefixSort::maxRequiredBytes(
        &rowContainer,
        compareFlags,
        common::PrefixSortConfig{
            1024,
            // Set threshold to 0 to enable prefix-sort in small dataset.
            0,
            12},
        sortPool.get());
    const auto beforeBytes = sortPool->peakBytes();
    ASSERT_EQ(sortPool->peakBytes(), 0);
    // Use PrefixSort to sort rows.
    PrefixSort::sort(
        &rowContainer,
        compareFlags,
        common::PrefixSortConfig{
            1024,
            // Set threshold to 0 to enable prefix-sort in small dataset.
            0,
            12},
        sortPool.get(),
        rows);
    ASSERT_GE(maxBytes, sortPool->peakBytes() - beforeBytes);

    // Extract data from the RowContainer in order.
    const RowVectorPtr actual =
        BaseVector::create<RowVector>(rowType, numRows, pool_.get());
    for (int column = 0; column < compareFlags.size(); ++column) {
      rowContainer.extractColumn(
          rows.data(), numRows, column, actual->childAt(column));
    }

    velox::test::assertEqualVectors(actual, expectedResult);
  }

  void testSpillRadixSort(
      const std::vector<CompareFlags>& compareFlags,
      const RowVectorPtr& data) {
    const auto numRows = data->size();
    const auto expectedResult =
        generateExpectedResult(compareFlags, numRows, data);

    const auto rowType = asRowType(data->type());
    const std::vector<TypePtr> keyTypes{
        rowType->children().begin(),
        rowType->children().begin() + compareFlags.size()};
    const std::vector<TypePtr> payloadTypes{
        rowType->children().begin() + compareFlags.size(),
        rowType->children().end()};

    RowContainer rowContainer(keyTypes, payloadTypes, pool_.get());
    auto rows = storeRows(numRows, data, &rowContainer);
    const auto sortPool = rootPool_->addLeafChild("spill-radix-sort");
    const SpillRadixSortConfig config{
        true,
        0,
        1024,
        12,
    };
    ASSERT_TRUE(SpillRadixSort::canSort(
        &rowContainer, compareFlags, rows.size(), config));
    SpillRadixSort::sort(
        &rowContainer, compareFlags, config, sortPool.get(), rows);

    // Compare only the sort-key columns. Building a vector over the full
    // row type would leave the trailing payload children populated with
    // uninitialized buffer data, which mismatches the equally uninitialized
    // payload in the expected vector.
    std::vector<std::string> keyNames(
        rowType->names().begin(),
        rowType->names().begin() + compareFlags.size());
    std::vector<TypePtr> keyTypesCopy = keyTypes;
    const auto keyRowType = ROW(std::move(keyNames), std::move(keyTypesCopy));
    const RowVectorPtr actual =
        BaseVector::create<RowVector>(keyRowType, numRows, pool_.get());
    for (int column = 0; column < compareFlags.size(); ++column) {
      rowContainer.extractColumn(
          rows.data(), numRows, column, actual->childAt(column));
    }

    std::vector<VectorPtr> expectedKeyChildren;
    expectedKeyChildren.reserve(compareFlags.size());
    for (int column = 0; column < compareFlags.size(); ++column) {
      expectedKeyChildren.push_back(expectedResult->childAt(column));
    }
    const auto expectedKeysOnly = std::make_shared<RowVector>(
        pool_.get(),
        keyRowType,
        nullptr,
        numRows,
        std::move(expectedKeyChildren));

    velox::test::assertEqualVectors(actual, expectedKeysOnly);
  }

 private:
  // Use std::sort to generate expected result.
  const RowVectorPtr generateExpectedResult(
      const std::vector<CompareFlags>& compareFlags,
      int numRows,
      const RowVectorPtr& sortedRows);
};

std::vector<char*, memory::StlAllocator<char*>> PrefixSortTest::storeRows(
    int numRows,
    const RowVectorPtr& sortedRows,
    RowContainer* data) {
  std::vector<char*, memory::StlAllocator<char*>> rows(*pool());
  SelectivityVector allRows(numRows);
  rows.resize(numRows);
  for (int row = 0; row < numRows; ++row) {
    rows[row] = data->newRow();
  }
  for (int column = 0; column < sortedRows->childrenSize(); ++column) {
    DecodedVector decoded(*sortedRows->childAt(column), allRows);
    for (int i = 0; i < numRows; ++i) {
      char* row = rows[i];
      data->store(decoded, i, row, column);
    }
  }
  return rows;
}

const RowVectorPtr PrefixSortTest::generateExpectedResult(
    const std::vector<CompareFlags>& compareFlags,
    int numRows,
    const RowVectorPtr& sortedRows) {
  const auto rowType = asRowType(sortedRows->type());
  const int numKeys = compareFlags.size();
  RowContainer rowContainer(rowType->children(), pool_.get());
  auto rows = storeRows(numRows, sortedRows, &rowContainer);

  std::sort(
      rows.begin(), rows.end(), [&](const char* leftRow, const char* rightRow) {
        for (auto i = 0; i < numKeys; ++i) {
          if (auto result =
                  rowContainer.compare(leftRow, rightRow, i, compareFlags[i])) {
            return result < 0;
          }
        }
        return false;
      });

  const RowVectorPtr result =
      BaseVector::create<RowVector>(rowType, numRows, pool_.get());
  for (int column = 0; column < compareFlags.size(); ++column) {
    rowContainer.extractColumn(
        rows.data(), numRows, column, result->childAt(column));
  }
  return result;
}

TEST_F(PrefixSortTest, singleKey) {
  // Vectors without nulls.
  const std::vector<VectorPtr> testData = {
      makeFlatVector<int64_t>({5, 4, 3, 2, 1}),
      makeFlatVector<int32_t>({5, 4, 3, 2, 1}),
      makeFlatVector<int16_t>({5, 4, 3, 2, 1}),
      makeFlatVector<int128_t>(
          {5,
           HugeInt::parse("1234567"),
           HugeInt::parse("12345678901234567890"),
           HugeInt::parse("12345679"),
           HugeInt::parse("-12345678901234567890")}),
      makeFlatVector<float>({5.5, 4.4, 3.3, 2.2, 1.1}),
      makeFlatVector<double>({5.5, 4.4, 3.3, 2.2, 1.1}),
      makeFlatVector<Timestamp>(
          {Timestamp(5, 5),
           Timestamp(4, 4),
           Timestamp(3, 3),
           Timestamp(2, 2),
           Timestamp(1, 1)}),
      makeFlatVector<std::string_view>({"eee", "ddd", "ccc", "bbb", "aaa"}),
      makeFlatVector<std::string_view>({"e", "dddddd", "bbb", "ddd", "aaa"}),
      makeFlatVector<std::string_view>(
          {"ddd_not_inline",
           "aaa_is_not_inline",
           "aaa_is_not_inline_a",
           "ddd_is_not_inline",
           "aaa_is_not_inline_b"}),
      makeNullableFlatVector<std::string_view>(
          {"\u7231",
           "\u671B\u5E0C\u2014\u5FF5\u4FE1",
           "\u671B\u5E0C",
           "\u7231\u2014",
           "\u4FE1 \u7231"},
          VARBINARY())};
  for (int i = 0; i < testData.size(); ++i) {
    const auto data = makeRowVector({testData[i]});

    testPrefixSort({kAsc}, data);
    testPrefixSort({kDesc}, data);
  }
}

TEST_F(PrefixSortTest, spillRadixSortBigintAndString) {
  constexpr vector_size_t kNumRows = 4096;
  auto data = makeRowVector({
      makeFlatVector<int64_t>(
          kNumRows, [](auto row) { return 100000 - (row * 37) % 100000; }),
      makeFlatVector<std::string>(kNumRows, [](auto row) {
        return fmt::format("same_prefix_{:06}", 100000 - row);
      }),
      makeFlatVector<int32_t>(
          kNumRows, [](auto row) { return static_cast<int32_t>(row); }),
  });

  testSpillRadixSort({kAsc, kAsc}, data);
  testSpillRadixSort({kAsc, kDesc}, data);
  testSpillRadixSort({kDesc, kAsc}, data);
}

// Single-key coverage for every type kind supported by SpillRadixSort, with
// nulls. Exercises the null-byte branch of encodeRowColumn for each encoder.
TEST_F(PrefixSortTest, spillRadixSortWithNulls) {
  constexpr vector_size_t kNumRows = 1500;
  // Place a null roughly every 7 rows to exercise both null and non-null
  // encoding paths within the same column.
  auto nullAt = [](vector_size_t row) { return row % 7 == 3; };

  std::vector<VectorPtr> testData;
  testData.push_back(makeFlatVector<int16_t>(
      kNumRows,
      [](vector_size_t row) {
        return static_cast<int16_t>(7919 - (row * 31) % 7919);
      },
      nullAt));
  testData.push_back(makeFlatVector<int32_t>(
      kNumRows,
      [](vector_size_t row) {
        return static_cast<int32_t>(1'000'003 - (row * 131) % 1'000'003);
      },
      nullAt));
  testData.push_back(makeFlatVector<int64_t>(
      kNumRows,
      [](vector_size_t row) {
        return 1'000'000'007LL -
            (static_cast<int64_t>(row) * 1031) % 1'000'000'007LL;
      },
      nullAt));
  testData.push_back(makeFlatVector<int128_t>(
      kNumRows,
      [](vector_size_t row) {
        return HugeInt::build(
            static_cast<int64_t>(row) - 750,
            static_cast<uint64_t>(row) * 6364136223846793005ULL);
      },
      nullAt,
      HUGEINT()));
  // Floats include a mix of negatives, infinities and NaN to exercise the
  // IEEE-754 bit-twiddling in the encoder.
  testData.push_back(makeFlatVector<float>(
      kNumRows,
      [](vector_size_t row) {
        if (row == 0) {
          return std::numeric_limits<float>::quiet_NaN();
        }
        if (row == 1) {
          return -std::numeric_limits<float>::infinity();
        }
        if (row == 2) {
          return std::numeric_limits<float>::infinity();
        }
        return 0.5f - static_cast<float>(row) * 0.25f;
      },
      nullAt));
  testData.push_back(makeFlatVector<double>(
      kNumRows,
      [](vector_size_t row) {
        if (row == 0) {
          return -0.0;
        }
        if (row == 1) {
          return 0.0;
        }
        return 1.5 - static_cast<double>(row) * 0.125;
      },
      nullAt));
  testData.push_back(makeFlatVector<Timestamp>(
      kNumRows,
      [](vector_size_t row) {
        return Timestamp(
            static_cast<int64_t>(1'700'000'000) - row * 13,
            static_cast<uint64_t>((row * 17) % 1'000'000'000));
      },
      nullAt));

  for (const auto& column : testData) {
    SCOPED_TRACE(fmt::format("type {}", column->type()->toString()));
    const auto data = makeRowVector({column});
    testSpillRadixSort({kAsc}, data);
    testSpillRadixSort({kDesc}, data);
  }

  // VARCHAR/VARBINARY exercised separately so we can mix inline (<= 12 bytes)
  // and non-inline values plus nulls. All values are unique to keep the
  // expected order well defined under std::sort.
  {
    auto strings = makeFlatVector<std::string>(
        kNumRows,
        [](vector_size_t row) {
          // Alternate between short (inline) and long (non-inline) variants
          // while keeping every value unique across rows.
          if (row % 5 == 0) {
            return fmt::format("s_{:04}", row);
          }
          return fmt::format("long_string_value_{:08}", kNumRows - row);
        },
        nullAt);
    const auto data = makeRowVector({strings});
    testSpillRadixSort({kAsc}, data);
    testSpillRadixSort({kDesc}, data);
  }
  {
    auto bins = makeFlatVector<std::string>(
        kNumRows,
        [](vector_size_t row) {
          return fmt::format("bin_{:010}_{}", row * 7, row);
        },
        nullAt,
        VARBINARY());
    const auto data = makeRowVector({bins});
    testSpillRadixSort({kAsc}, data);
    testSpillRadixSort({kDesc}, data);
  }
}

// Exercises the residual-compare branch in SpillRadixSort::sort:
//  - hasNonNormalizedKey: more keys than the prefix budget can normalize.
//  - long string keys that are truncated by maxStringPrefixLength so that
//    the radix prefix is identical for many rows and the timsort fallback
//    must order them.
TEST_F(PrefixSortTest, spillRadixSortResidualCompare) {
  constexpr vector_size_t kNumRows = 2048;

  // Case 1: long strings with the same prefix force equal-prefix groups.
  // The radix prefix is bounded by config.maxStringPrefixLength below; the
  // tail (and the second key) must be ordered by the residual comparator.
  {
    auto data = makeRowVector({
        makeFlatVector<std::string>(
            kNumRows,
            [](vector_size_t row) {
              // First 16+ chars are identical; differences only after the
              // prefix window so radix cannot resolve order alone.
              return fmt::format(
                  "common_prefix_padding_{:08}", kNumRows - row);
            }),
        makeFlatVector<int64_t>(
            kNumRows,
            [](vector_size_t row) {
              return static_cast<int64_t>((row * 911) % 7919);
            }),
    });
    testSpillRadixSort({kAsc, kAsc}, data);
    testSpillRadixSort({kAsc, kDesc}, data);
  }

  // Case 2: more keys than the prefix budget can fully normalize. With three
  // BIGINT keys (each 9 bytes including the null byte) and a 16-byte
  // normalized budget, only the first key is normalized; the remaining keys
  // are non-normalized and must be ordered by the residual RowContainer
  // comparator (hasNonNormalizedKey path).
  {
    auto data = makeRowVector({
        makeFlatVector<int64_t>(
            kNumRows, [](vector_size_t row) { return row % 4; }),
        makeFlatVector<int64_t>(
            kNumRows, [](vector_size_t row) { return (row / 4) % 8; }),
        makeFlatVector<int64_t>(
            kNumRows,
            [](vector_size_t row) {
              return static_cast<int64_t>(kNumRows - row);
            }),
    });
    const std::vector<CompareFlags> compareFlags{kAsc, kAsc, kAsc};
    const auto numRows = data->size();
    const auto rowType = asRowType(data->type());
    const std::vector<TypePtr> keyTypes(
        rowType->children().begin(), rowType->children().end());

    RowContainer rowContainer(keyTypes, {}, pool_.get());
    auto rows = storeRows(numRows, data, &rowContainer);
    const auto sortPool =
        rootPool_->addLeafChild("spill-radix-sort-residual");
    // 16 normalized bytes only fits one BIGINT (9 bytes including null
    // byte). The other two keys must be resolved by the timsort fallback.
    const SpillRadixSortConfig config{true, 0, 16, 12};
    ASSERT_TRUE(SpillRadixSort::canSort(
        &rowContainer, compareFlags, rows.size(), config));
    SpillRadixSort::sort(
        &rowContainer, compareFlags, config, sortPool.get(), rows);

    // Compute expected order via std::sort to validate the residual path.
    RowContainer ref(keyTypes, {}, pool_.get());
    auto refRows = storeRows(numRows, data, &ref);
    std::sort(
        refRows.begin(),
        refRows.end(),
        [&](const char* l, const char* r) {
          for (auto i = 0; i < compareFlags.size(); ++i) {
            const auto c = ref.compare(l, r, i, compareFlags[i]);
            if (c != 0) {
              return c < 0;
            }
          }
          return false;
        });

    const auto actual =
        BaseVector::create<RowVector>(rowType, numRows, pool_.get());
    const auto expectedSorted =
        BaseVector::create<RowVector>(rowType, numRows, pool_.get());
    for (int column = 0; column < compareFlags.size(); ++column) {
      rowContainer.extractColumn(
          rows.data(), numRows, column, actual->childAt(column));
      ref.extractColumn(
          refRows.data(), numRows, column, expectedSorted->childAt(column));
    }
    velox::test::assertEqualVectors(actual, expectedSorted);
  }
}

// Fuzz across every type kind that SpillRadixSort supports. Mirrors the
// PrefixSort fuzz tests but restricted to the radix-eligible type set.
TEST_F(PrefixSortTest, spillRadixSortFuzz) {
  std::vector<TypePtr> keyTypes = {
      SMALLINT(),
      INTEGER(),
      BIGINT(),
      HUGEINT(),
      REAL(),
      DOUBLE(),
      TIMESTAMP(),
      VARCHAR(),
      VARBINARY()};

  auto runFuzzTest = [&](double nullRatio) {
    for (const auto& type : keyTypes) {
      SCOPED_TRACE(fmt::format("type {}, nulls {}", type->toString(), nullRatio));
      VectorFuzzer fuzzer(
          {.vectorSize = 4'096, .nullRatio = nullRatio}, pool());
      RowVectorPtr data = fuzzer.fuzzRow(ROW({type}));
      testSpillRadixSort({kAsc}, data);
      testSpillRadixSort({kDesc}, data);
    }
  };

  runFuzzTest(0.0);
  runFuzzTest(0.1);
}

// Verifies the chunked prefix-buffer allocation: with a tiny per-chunk page
// budget the sort must still produce the same stable order as the default
// (single-chunk) path. Exercises many chunks, chunk-boundary crossings, and
// a non-multiple row count so the last chunk is partial.
TEST_F(PrefixSortTest, spillRadixSortChunkedAllocation) {
  constexpr vector_size_t kNumRows = 20'000;

  // Two keys: a BIGINT and a VARCHAR longer than 8 bytes so the normalized
  // prefix is non-trivial and rows cross chunk boundaries.
  auto data = makeRowVector({
      makeFlatVector<int64_t>(
          kNumRows,
          [](vector_size_t row) {
            return static_cast<int64_t>((row * 2654435761U) % 1'000'003);
          }),
      makeFlatVector<std::string>(
          kNumRows,
          [](vector_size_t row) {
            return fmt::format("chunk_test_key_{:012}", row % 4093);
          }),
  });
  const std::vector<CompareFlags> compareFlags{kAsc, kAsc};

  auto runSort = [&](uint32_t chunkPages) {
    const auto rowType = asRowType(data->type());
    RowContainer rowContainer(
        {BIGINT(), VARCHAR()}, {}, pool_.get());
    auto rows = storeRows(kNumRows, data, &rowContainer);
    const auto sortPool =
        rootPool_->addLeafChild("spill-radix-sort-chunked");
    SpillRadixSortConfig config;
    config.enabled = true;
    config.minRows = 0;
    config.maxPrefixBufferChunkPages = chunkPages;
    EXPECT_TRUE(SpillRadixSort::canSort(
        &rowContainer, compareFlags, rows.size(), config));
    SpillRadixSort::sort(
        &rowContainer, compareFlags, config, sortPool.get(), rows);

    std::vector<std::string> keyNames{"c0", "c1"};
    const auto keyRowType = ROW(std::move(keyNames), {BIGINT(), VARCHAR()});
    const RowVectorPtr actual =
        BaseVector::create<RowVector>(keyRowType, kNumRows, pool_.get());
    for (int column = 0; column < compareFlags.size(); ++column) {
      rowContainer.extractColumn(
          rows.data(), kNumRows, column, actual->childAt(column));
    }
    return actual;
  };

  // Default (single large chunk) vs tiny 1-page chunks (forces ~every row in
  // its own chunk) vs a small multi-row chunk that straddles boundaries.
  const auto defaultResult = runSort(256);
  const auto tinyResult = runSort(1);
  const auto smallResult = runSort(4);

  velox::test::assertEqualVectors(defaultResult, tinyResult);
  velox::test::assertEqualVectors(defaultResult, smallResult);
}

// Verifies the fallback to the comparator sort when the prefix-buffer chunk
// allocation fails (e.g. the spill memory allocator is exhausted). The
// fallback must produce the same sorted order as the radix path.
TEST_F(PrefixSortTest, spillRadixSortAllocationFailureFallback) {
  constexpr vector_size_t kNumRows = 5'000;

  auto data = makeRowVector({
      makeFlatVector<int64_t>(
          kNumRows, [](vector_size_t row) { return row % 97; }),
      makeFlatVector<std::string>(
          kNumRows,
          [](vector_size_t row) {
            return fmt::format("fallback_key_{:010}", row % 2039);
          }),
  });
  const std::vector<CompareFlags> compareFlags{kAsc, kAsc};

  auto runSort = [&](bool injectFailure) {
    const auto rowType = asRowType(data->type());
    RowContainer rowContainer({BIGINT(), VARCHAR()}, {}, pool_.get());
    auto rows = storeRows(kNumRows, data, &rowContainer);
    const auto sortPool =
        rootPool_->addLeafChild("spill-radix-sort-fallback");
    SpillRadixSortConfig config;
    config.enabled = true;
    config.minRows = 0;
    EXPECT_TRUE(SpillRadixSort::canSort(
        &rowContainer, compareFlags, rows.size(), config));

    if (injectFailure) {
      // Force the chunk allocation to fail so sort() falls back to the
      // comparator path.
      SCOPED_TESTVALUE_SET(
          "SpillRadixSort::sort::allocateChunks",
          std::function<void(bool*)>([&](bool*) {
            throw VeloxRuntimeError(
                __FILE__,
                __LINE__,
                __FUNCTION__,
                "SpillRadixSort::sort",
                "Injected prefix buffer allocation failure",
                "",
                error_code::kMemAllocError,
                /*isRetriable=*/true);
          }));
    }
    SpillRadixSort::sort(
        &rowContainer, compareFlags, config, sortPool.get(), rows);

    std::vector<std::string> keyNames{"c0", "c1"};
    const auto keyRowType = ROW(std::move(keyNames), {BIGINT(), VARCHAR()});
    const RowVectorPtr actual =
        BaseVector::create<RowVector>(keyRowType, kNumRows, pool_.get());
    for (int column = 0; column < compareFlags.size(); ++column) {
      rowContainer.extractColumn(
          rows.data(), kNumRows, column, actual->childAt(column));
    }
    return actual;
  };

  const auto radixResult = runSort(false);
  const auto fallbackResult = runSort(true);
  velox::test::assertEqualVectors(radixResult, fallbackResult);
}

// Negative tests: configurations and inputs for which canSort must return
// false so the caller falls back to the comparator-based sort path.
TEST_F(PrefixSortTest, spillRadixSortCanSortNegative) {
  const std::vector<TypePtr> keyTypes{BIGINT()};
  RowContainer rowContainer(keyTypes, {}, pool_.get());
  const std::vector<CompareFlags> compareFlags{kAsc};

  // Disabled config.
  {
    SpillRadixSortConfig config;
    config.enabled = false;
    EXPECT_FALSE(
        SpillRadixSort::canSort(&rowContainer, compareFlags, 10'000, config));
  }

  // Below minRows threshold.
  {
    SpillRadixSortConfig config;
    config.enabled = true;
    config.minRows = 1024;
    EXPECT_FALSE(
        SpillRadixSort::canSort(&rowContainer, compareFlags, 1023, config));
    EXPECT_TRUE(
        SpillRadixSort::canSort(&rowContainer, compareFlags, 1024, config));
  }

  // Null rowContainer.
  {
    SpillRadixSortConfig config;
    EXPECT_FALSE(
        SpillRadixSort::canSort(nullptr, compareFlags, 10'000, config));
  }

  // Unsupported type kind (BOOLEAN has no PrefixSort encoder, so the layout
  // produces no normalized keys and radix cannot run).
  {
    const std::vector<TypePtr> boolKeys{BOOLEAN()};
    RowContainer boolContainer(boolKeys, {}, pool_.get());
    SpillRadixSortConfig config;
    EXPECT_FALSE(SpillRadixSort::canSort(
        &boolContainer, std::vector<CompareFlags>{kAsc}, 10'000, config));
  }
}

// Verifies the radix counting pass
// (facebook::velox::exec::detail::spillRadixSortCountingPass):
// it buckets rows by the byte at 'byteOffset' of each prefix and scatters
// them into 'out' in stable order. We compare against a scalar reference.
TEST_F(PrefixSortTest, spillRadixSortCountingPass) {
  constexpr size_t kNumRows = 10'000;
  constexpr size_t kEntrySize = 64;
  constexpr uint32_t kByteOffset = 27;
  constexpr uint32_t kTrialOffset = 13;

  // Reference implementation (scalar).
  auto referenceCountingPass =
      [](char** in,
         char** out,
         size_t numRows,
         uint32_t byteOffset) {
        std::vector<uint32_t> counts(257, 0);
        for (size_t i = 0; i < numRows; ++i) {
          ++counts[1 + static_cast<uint8_t>(*(in[i] + byteOffset))];
        }
        for (int32_t i = 1; i < 256; ++i) {
          counts[i] += counts[i - 1];
        }
        for (size_t i = 0; i < numRows; ++i) {
          const auto key = static_cast<uint8_t>(*(in[i] + byteOffset));
          out[counts[key]++] = in[i];
        }
      };

  // Build a contiguous prefix buffer with unique rows so we can verify
  // stability: row j occupies bytes [j * kEntrySize, (j+1) * kEntrySize).
  const size_t bufferSize = kNumRows * kEntrySize;
  std::vector<char> prefix(bufferSize);
  for (size_t j = 0; j < kNumRows; ++j) {
    // Fill the whole row with deterministic pseudo-random bytes.
    for (size_t b = 0; b < kEntrySize; ++b) {
      prefix[j * kEntrySize + b] =
          static_cast<char>((j * 131 + b * 17 + (b * b) % 251) & 0xff);
    }
  }
  std::vector<char*> rows(kNumRows);
  for (size_t j = 0; j < kNumRows; ++j) {
    rows[j] = prefix.data() + j * kEntrySize;
  }

  // Compare the two byte offsets in both directions: the counting pass must
  // produce the same stable scatter as the scalar reference, and the two
  // offsets must agree with each other (a permutation of all rows).
  for (uint32_t byteOffset : {kByteOffset, kTrialOffset}) {
    std::vector<char*> outSve(kNumRows, nullptr);
    std::vector<char*> outScalar(kNumRows, nullptr);
    facebook::velox::exec::detail::spillRadixSortCountingPass(
        rows.data(), outSve.data(), kNumRows, byteOffset);
    referenceCountingPass(
        rows.data(), outScalar.data(), kNumRows, byteOffset);

    EXPECT_EQ(outSve, outScalar) << "counting pass mismatch at byte "
                                 << byteOffset;

    // Every row must appear exactly once (stable permutation).
    std::vector<bool> seen(kNumRows, false);
    for (size_t j = 0; j < kNumRows; ++j) {
      const auto rowIndex = (outSve[j] - prefix.data()) / kEntrySize;
      ASSERT_LT(rowIndex, kNumRows);
      EXPECT_FALSE(seen[rowIndex]);
      seen[rowIndex] = true;
    }
  }

  // Edge cases: empty input and single row.
  {
    std::vector<char*> out(1, nullptr);
    facebook::velox::exec::detail::spillRadixSortCountingPass(
        rows.data(), out.data(), 0, kByteOffset);
    facebook::velox::exec::detail::spillRadixSortCountingPass(
        rows.data(), out.data(), 1, kByteOffset);
    EXPECT_EQ(out[0], rows[0]);
  }
}

TEST_F(PrefixSortTest, singleKeyWithNulls) {
  // Vectors with nulls.
  const std::vector<VectorPtr> testData = {
      makeNullableFlatVector<int64_t>({5, 4, std::nullopt, 2, 1}),
      makeNullableFlatVector<int32_t>({5, 4, std::nullopt, 2, 1}),
      makeNullableFlatVector<int16_t>({5, 4, std::nullopt, 2, 1}),
      makeNullableFlatVector<int128_t>(
          {5,
           HugeInt::parse("1234567"),
           std::nullopt,
           HugeInt::parse("12345679"),
           HugeInt::parse("-12345678901234567890")}),
      makeNullableFlatVector<float>({5.5, 4.4, std::nullopt, 2.2, 1.1}),
      makeNullableFlatVector<double>({5.5, 4.4, std::nullopt, 2.2, 1.1}),
      makeNullableFlatVector<Timestamp>(
          {Timestamp(5, 5),
           Timestamp(4, 4),
           std::nullopt,
           Timestamp(2, 2),
           Timestamp(1, 1)}),
      makeNullableFlatVector<std::string_view>(
          {"eee", "ddd", std::nullopt, "bbb", "aaa"}),
      makeNullableFlatVector<std::string_view>(
          {"ee", "aaa", std::nullopt, "d", "aaaaaaaaaaaaaa"}),
      makeNullableFlatVector<std::string_view>(
          {"aaa_is_not_inline",
           "aaa_is_not_inline_2",
           std::nullopt,
           "aaa_is_not_inline_1",
           "aaaaaaaaaaaaaa"}),
      makeNullableFlatVector<std::string_view>(
          {"\u7231",
           "\u671B\u5E0C\u2014\u5FF5\u4FE1",
           std::nullopt,
           "\u7231\u2014",
           "\u4FE1 \u7231"},
          VARBINARY())};

  for (int i = 0; i < testData.size(); ++i) {
    const auto data = makeRowVector({testData[i]});

    testPrefixSort({kAsc}, data);
    testPrefixSort({kDesc}, data);
  }
}

TEST_F(PrefixSortTest, multipleKeys) {
  // Test all keys normalized : bigint, integer
  {
    const auto data = makeRowVector({
        makeNullableFlatVector<int64_t>({5, 2, std::nullopt, 2, 1}),
        makeNullableFlatVector<int32_t>({5, 4, std::nullopt, 2, 1}),
    });

    testPrefixSort({kAsc, kAsc}, data);
    testPrefixSort({kDesc, kDesc}, data);
  }

  // Test keys with semi-normalized : bigint, varchar
  {
    const auto data = makeRowVector({
        makeNullableFlatVector<int64_t>({5, 2, std::nullopt, 2, 1}),
        makeNullableFlatVector<std::string_view>(
            {"eee", "aaa", std::nullopt, "bbb", "aaaa"}),
    });

    testPrefixSort({kAsc, kAsc}, data);
    testPrefixSort({kDesc, kDesc}, data);
  }
}

TEST_F(PrefixSortTest, fuzz) {
  std::vector<TypePtr> keyTypes = {
      INTEGER(),
      BOOLEAN(),
      TINYINT(),
      SMALLINT(),
      BIGINT(),
      DECIMAL(12, 2),
      DECIMAL(25, 6),
      REAL(),
      DOUBLE(),
      TIMESTAMP(),
      VARCHAR(),
      VARBINARY()};

  auto runFuzzTest = [&](double nullRatio) {
    for (const auto& type : keyTypes) {
      SCOPED_TRACE(fmt::format("{}", type->toString()));
      VectorFuzzer fuzzer(
          {.vectorSize = 10'240, .nullRatio = nullRatio}, pool());
      RowVectorPtr data = fuzzer.fuzzRow(ROW({type}));

      testPrefixSort({kAsc}, data);
      testPrefixSort({kDesc}, data);
    }
  };

  runFuzzTest(0.1);
  runFuzzTest(0.0);
}

TEST_F(PrefixSortTest, fuzzMulti) {
  std::vector<TypePtr> keyTypes = {
      INTEGER(),
      BOOLEAN(),
      TINYINT(),
      SMALLINT(),
      BIGINT(),
      DECIMAL(12, 2),
      DECIMAL(25, 6),
      REAL(),
      DOUBLE(),
      TIMESTAMP(),
      VARCHAR(),
      VARBINARY()};
  auto runFuzzTest = [&](double nullRatio) {
    VectorFuzzer fuzzer({.vectorSize = 10'240, .nullRatio = nullRatio}, pool());

    for (auto i = 0; i < 20; ++i) {
      auto type1 = fuzzer.randType(keyTypes, 0);
      auto type2 = fuzzer.randType(keyTypes, 0);

      SCOPED_TRACE(fmt::format("{}, {}", type1->toString(), type2->toString()));
      auto data = fuzzer.fuzzRow(ROW({type1, type2, VARCHAR()}));

      testPrefixSort({kAsc, kAsc}, data);
      testPrefixSort({kDesc, kDesc}, data);
    }
  };

  runFuzzTest(0.1);
  runFuzzTest(0.0);
}

TEST_F(PrefixSortTest, checkMaxNormalizedKeySizeForMultipleKeys) {
  // Test the normalizedKeySize doesn't exceed the MaxNormalizedKeySize.
  // The normalizedKeySize for BIGINT should be 8 + 1.
  std::vector<TypePtr> keyTypes = {BIGINT(), BIGINT()};
  std::vector<CompareFlags> compareFlags = {kAsc, kDesc};
  std::vector<std::optional<uint32_t>> maxStringLengths = {
      std::nullopt, std::nullopt};
  std::vector<bool> columnHasNulls = {true, true};
  auto sortLayout = PrefixSortLayout::generate(
      keyTypes, columnHasNulls, compareFlags, 8, 9, maxStringLengths);
  ASSERT_FALSE(sortLayout.hasNormalizedKeys);

  auto sortLayoutOneKey = PrefixSortLayout::generate(
      keyTypes, columnHasNulls, compareFlags, 9, 9, maxStringLengths);
  ASSERT_TRUE(sortLayoutOneKey.hasNormalizedKeys);
  ASSERT_TRUE(sortLayoutOneKey.hasNonNormalizedKey);
  ASSERT_EQ(sortLayoutOneKey.prefixOffsets.size(), 1);
  ASSERT_EQ(sortLayoutOneKey.prefixOffsets[0], 0);

  auto sortLayoutOneKey1 = PrefixSortLayout::generate(
      keyTypes, columnHasNulls, compareFlags, 17, 12, maxStringLengths);
  ASSERT_TRUE(sortLayoutOneKey1.hasNormalizedKeys);
  ASSERT_TRUE(sortLayoutOneKey1.hasNonNormalizedKey);
  ASSERT_EQ(sortLayoutOneKey1.prefixOffsets.size(), 1);
  ASSERT_EQ(sortLayoutOneKey1.prefixOffsets[0], 0);

  auto sortLayoutTwoKeys = PrefixSortLayout::generate(
      keyTypes, columnHasNulls, compareFlags, 18, 12, maxStringLengths);
  ASSERT_TRUE(sortLayoutTwoKeys.hasNormalizedKeys);
  ASSERT_FALSE(sortLayoutTwoKeys.hasNonNormalizedKey);
  ASSERT_FALSE(
      sortLayoutTwoKeys.nonPrefixSortStartIndex <
      sortLayoutTwoKeys.numNormalizedKeys);
  ASSERT_EQ(sortLayoutTwoKeys.prefixOffsets.size(), 2);
  ASSERT_EQ(sortLayoutTwoKeys.prefixOffsets[0], 0);
  ASSERT_EQ(sortLayoutTwoKeys.prefixOffsets[1], 9);
  ASSERT_TRUE(sortLayoutTwoKeys.normalizedKeyHasNullByte[0]);
  ASSERT_TRUE(sortLayoutTwoKeys.normalizedKeyHasNullByte[1]);

  columnHasNulls = {false, false};
  auto sortLayoutTwoKeysNoNulls = PrefixSortLayout::generate(
      keyTypes, columnHasNulls, compareFlags, 18, 12, maxStringLengths);
  ASSERT_TRUE(sortLayoutTwoKeysNoNulls.hasNormalizedKeys);
  ASSERT_FALSE(sortLayoutTwoKeysNoNulls.hasNonNormalizedKey);
  ASSERT_EQ(sortLayoutTwoKeysNoNulls.prefixOffsets.size(), 2);
  ASSERT_EQ(sortLayoutTwoKeysNoNulls.prefixOffsets[0], 0);
  ASSERT_EQ(sortLayoutTwoKeysNoNulls.prefixOffsets[1], 8);
  ASSERT_FALSE(sortLayoutTwoKeysNoNulls.normalizedKeyHasNullByte[0]);
  ASSERT_FALSE(sortLayoutTwoKeysNoNulls.normalizedKeyHasNullByte[1]);
}

TEST_F(PrefixSortTest, optimizeSortKeysOrder) {
  struct {
    RowTypePtr inputType;
    std::vector<column_index_t> keyChannels;
    std::vector<column_index_t> expectedSortedKeyChannels;

    std::string debugString() const {
      return fmt::format(
          "inputType {}, keyChannels {}, expectedSortedKeyChannels {}",
          inputType->toString(),
          fmt::join(keyChannels, ":"),
          fmt::join(expectedSortedKeyChannels, ":"));
    }
  } testSettings[] = {
      {ROW({BIGINT(), BIGINT()}), {0, 1}, {0, 1}},
      {ROW({BIGINT(), BIGINT()}), {1, 0}, {1, 0}},
      {ROW({BIGINT(), BIGINT(), BIGINT()}), {1, 0}, {1, 0}},
      {ROW({BIGINT(), BIGINT(), BIGINT()}), {1, 0}, {1, 0}},
      {ROW({BIGINT(), SMALLINT(), BIGINT()}), {0, 1}, {1, 0}},
      {ROW({BIGINT(), SMALLINT(), VARCHAR()}), {0, 1, 2}, {1, 0, 2}},
      {ROW({TINYINT(), BIGINT(), VARCHAR(), TINYINT(), INTEGER(), VARCHAR()}),
       {2, 1, 0, 4, 5, 3},
       {4, 1, 2, 5, 0, 3}},
      {ROW({INTEGER(), BIGINT(), VARCHAR(), TINYINT(), INTEGER(), VARCHAR()}),
       {5, 4, 3, 2, 1, 0},
       {4, 0, 1, 5, 2, 3}}};

  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());

    std::vector<IdentityProjection> projections;
    for (auto i = 0; i < testData.keyChannels.size(); ++i) {
      projections.emplace_back(testData.keyChannels[i], i);
    }
    PrefixSortLayout::optimizeSortKeysOrder(testData.inputType, projections);
    std::unordered_set<column_index_t> outputChannelSet;
    for (auto i = 0; i < projections.size(); ++i) {
      ASSERT_EQ(
          projections[i].inputChannel, testData.expectedSortedKeyChannels[i]);
      ASSERT_EQ(
          testData.keyChannels[projections[i].outputChannel],
          projections[i].inputChannel);
    }
  }
}

TEST_F(PrefixSortTest, makeSortLayoutForString) {
  std::vector<TypePtr> keyTypes = {VARCHAR(), BIGINT()};
  std::vector<CompareFlags> compareFlags = {kAsc, kDesc};
  std::vector<std::optional<uint32_t>> maxStringLengths = {9, std::nullopt};
  std::vector<bool> columnHasNulls = {true, true};

  auto sortLayoutOneKey = PrefixSortLayout::generate(
      keyTypes, columnHasNulls, compareFlags, 24, 8, maxStringLengths);
  ASSERT_TRUE(sortLayoutOneKey.hasNormalizedKeys);
  ASSERT_TRUE(sortLayoutOneKey.hasNonNormalizedKey);
  ASSERT_TRUE(
      sortLayoutOneKey.nonPrefixSortStartIndex <
      sortLayoutOneKey.numNormalizedKeys);
  ASSERT_EQ(sortLayoutOneKey.encodeSizes.size(), 1);
  ASSERT_EQ(sortLayoutOneKey.encodeSizes[0], 9);

  auto sortLayoutTwoCompleteKeys = PrefixSortLayout::generate(
      keyTypes, columnHasNulls, compareFlags, 26, 9, maxStringLengths);
  ASSERT_TRUE(sortLayoutTwoCompleteKeys.hasNormalizedKeys);
  ASSERT_FALSE(sortLayoutTwoCompleteKeys.hasNonNormalizedKey);
  ASSERT_TRUE(
      sortLayoutTwoCompleteKeys.nonPrefixSortStartIndex ==
      sortLayoutTwoCompleteKeys.numNormalizedKeys);
  ASSERT_EQ(sortLayoutTwoCompleteKeys.encodeSizes.size(), 2);
  ASSERT_EQ(sortLayoutTwoCompleteKeys.encodeSizes[0], 10);
  ASSERT_EQ(sortLayoutTwoCompleteKeys.encodeSizes[1], 9);

  // The last key type is VARBINARY, indicating that only partial data is
  // encoded in the prefix.
  std::vector<TypePtr> keyTypes1 = {BIGINT(), VARBINARY()};
  maxStringLengths = {std::nullopt, 9};
  auto sortLayoutTwoKeys = PrefixSortLayout::generate(
      keyTypes1, columnHasNulls, compareFlags, 26, 8, maxStringLengths);
  ASSERT_TRUE(sortLayoutTwoKeys.hasNormalizedKeys);
  ASSERT_FALSE(sortLayoutTwoKeys.hasNonNormalizedKey);
  ASSERT_TRUE(
      sortLayoutTwoKeys.nonPrefixSortStartIndex <
      sortLayoutTwoKeys.numNormalizedKeys);
  ASSERT_EQ(sortLayoutTwoKeys.encodeSizes.size(), 2);
  ASSERT_EQ(sortLayoutTwoKeys.encodeSizes[0], 9);
  ASSERT_EQ(sortLayoutTwoKeys.encodeSizes[1], 9);
}

} // namespace
} // namespace facebook::velox::exec::prefixsort::test
