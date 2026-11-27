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

#include <folly/Benchmark.h>
#include <folly/init/Init.h>

#include "velox/exec/RowContainer.h"
#include "velox/exec/SpillRadixSort.h"
#include "velox/external/timsort/TimSort.hpp"
#include "velox/vector/fuzzer/VectorFuzzer.h"

DEFINE_double(data_null_ratio, 0.7, "Data null ratio");

using namespace facebook::velox;
using namespace facebook::velox::exec;

namespace {

class TestCase {
 public:
  TestCase(
      memory::MemoryPool* pool,
      const std::string& testName,
      size_t numRows,
      const RowTypePtr& rowType,
      int numKeys)
      : testName_(testName), numRows_(numRows), pool_(pool), rowType_(rowType) {
    // Initialize a RowContainer that holds fuzzed rows to be sorted.
    std::vector<TypePtr> keyTypes;
    std::vector<TypePtr> dependentTypes;
    for (auto i = 0; i < rowType->size(); ++i) {
      if (i < numKeys) {
        keyTypes.push_back(rowType->childAt(i));
      } else {
        dependentTypes.push_back(rowType->childAt(i));
      }
    }
    data_ = std::make_unique<RowContainer>(keyTypes, dependentTypes, pool);
    RowVectorPtr sortedRows = fuzzRows(numRows, numKeys);
    storeRows(numRows, sortedRows);

    // Initialize CompareFlags, it could be same for each key in benchmark.
    for (int i = 0; i < numKeys; ++i) {
      compareFlags_.push_back(
          {true, true, false, CompareFlags::NullHandlingMode::kNullAsValue});
    }
  };

  const std::string& testName() const {
    return testName_;
  }

  size_t numRows() const {
    return numRows_;
  }

  const std::vector<char*>& rows() const {
    return rows_;
  }

  RowContainer* rowContainer() const {
    return data_.get();
  }

  const std::vector<CompareFlags>& compareFlags() const {
    return compareFlags_;
  }

 private:
  // Store data into the RowContainer to mock the behavior of SortBuffer.
  void storeRows(int numRows, const RowVectorPtr& data) {
    rows_.resize(numRows);
    for (auto row = 0; row < numRows; ++row) {
      rows_[row] = rowContainer()->newRow();
    }
    for (auto column = 0; column < data->childrenSize(); ++column) {
      DecodedVector decoded(*data->childAt(column));
      rowContainer()->store(
          decoded, folly::Range(rows_.data(), numRows), column);
    }
  }

  RowVectorPtr fuzzRows(size_t numRows, int numKeys) {
    VectorFuzzer fuzzer({.vectorSize = numRows}, pool_);
    VectorFuzzer fuzzerWithNulls(
        {.vectorSize = numRows, .nullRatio = FLAGS_data_null_ratio}, pool_);
    std::vector<VectorPtr> children;

    // Fuzz keys: for front keys (column 0 to numKeys - 2) use high
    // nullRatio to enforce all columns to be compared.
    {
      for (auto i = 0; i < numKeys - 1; ++i) {
        children.push_back(fuzzerWithNulls.fuzz(rowType_->childAt(i)));
      }
      children.push_back(fuzzer.fuzz(rowType_->childAt(numKeys - 1)));
    }
    // Fuzz payload
    {
      for (auto i = numKeys; i < rowType_->size(); ++i) {
        children.push_back(fuzzer.fuzz(rowType_->childAt(i)));
      }
    }
    return std::make_shared<RowVector>(
        pool_, rowType_, nullptr, numRows, std::move(children));
  }

  const std::string testName_;
  const size_t numRows_;
  // Rows address stored in RowContainer
  std::vector<char*> rows_;
  std::unique_ptr<RowContainer> data_;
  memory::MemoryPool* const pool_;
  const RowTypePtr rowType_;
  std::vector<CompareFlags> compareFlags_;
};

class SpillRadixSortBenchmark {
 public:
  SpillRadixSortBenchmark(memory::MemoryPool* pool) : pool_(pool) {}

  void runSpillRadixSort(
      const std::vector<char*>& rows,
      RowContainer* rowContainer,
      const std::vector<CompareFlags>& compareFlags) {
    // Copy rows to avoid sorting rows that are already sorted.
    auto sortedRows = std::vector<char*, memory::StlAllocator<char*>>(
        rows.begin(), rows.end(), *pool_);
    // Set minRows=0 to enable the radix path for the small benchmark
    // datasets (SpillRadixSort::sort validates minRows >= 1024 by default).
    SpillRadixSortConfig config;
    config.enabled = true;
    config.minRows = 0;
    // Mirror the production spill path (SpillerBase::ensureSorted): when
    // canSort rejects radix (e.g. two or more truncated string keys) fall
    // back to the comparator sort. Without this the benchmark bypasses the
    // gate and would show the radix regression that production avoids.
    if (SpillRadixSort::canSort(
            rowContainer, compareFlags, sortedRows.size(), config)) {
      SpillRadixSort::sort(
          rowContainer, compareFlags, config, pool_, sortedRows);
    } else {
      gfx::timsort(
          sortedRows.begin(),
          sortedRows.end(),
          [&](const char* left, const char* right) {
            return rowContainer->compareRows(
                       left, right, compareFlags) < 0;
          });
    }
  }

  void runTimSort(
      const std::vector<char*>& rows,
      RowContainer* rowContainer,
      const std::vector<CompareFlags>& compareFlags) {
    auto sortedRows = std::vector<char*, memory::StlAllocator<char*>>(
        rows.begin(), rows.end(), *pool_);
    gfx::timsort(
        sortedRows.begin(),
        sortedRows.end(),
        [&](const char* left, const char* right) {
          return rowContainer->compareRows(
                     left, right, compareFlags) < 0;
        });
  }

  // Add benchmark manually to avoid writing a lot of BENCHMARK.
  void addBenchmark(
      const std::string& testName,
      size_t numRows,
      const RowTypePtr& rowType,
      int iterations,
      int numKeys) {
    auto testCase =
        std::make_unique<TestCase>(pool_, testName, numRows, rowType, numKeys);
    // Add benchmarks for radix-sort and timsort.
    folly::addBenchmark(
        __FILE__,
        "TimSort_" + testCase->testName(),
        [rows = testCase->rows(),
         container = testCase->rowContainer(),
         sortFlags = testCase->compareFlags(),
         iterations = iterations,
         this]() {
          for (auto i = 0; i < iterations; ++i) {
            runTimSort(rows, container, sortFlags);
          }
          return rows.size() * iterations;
        });
    folly::addBenchmark(
        __FILE__,
        "%RadixSort_" + testCase->testName(),
        [rows = testCase->rows(),
         container = testCase->rowContainer(),
         sortFlags = testCase->compareFlags(),
         iterations = iterations,
         this]() {
          for (auto i = 0; i < iterations; ++i) {
            runSpillRadixSort(rows, container, sortFlags);
          }
          return rows.size() * iterations;
        });
    testCases_.push_back(std::move(testCase));
  }

  void benchmark(
      const std::string& prefix,
      const std::string& keyName,
      const std::vector<vector_size_t>& batchSizes,
      const std::vector<RowTypePtr>& rowTypes,
      const std::vector<int>& numKeys,
      int32_t iterations) {
    for (auto batchSize : batchSizes) {
      for (auto i = 0; i < rowTypes.size(); ++i) {
        const auto name = fmt::format(
            "{}_{}_{}_{}k", prefix, numKeys[i], keyName, batchSize / 1000.0);
        addBenchmark(
            name, batchSize, rowTypes[i], iterations, numKeys[i]);
      }
    }
  }

  void bigint(bool noPayload, int numIterations, const std::vector<vector_size_t>& batchSizes) {
    std::vector<RowTypePtr> rowTypes;
    if (noPayload) {
      rowTypes = {
          ROW({BIGINT()}),
          ROW({BIGINT(), BIGINT()}),
          ROW({BIGINT(), BIGINT(), BIGINT()}),
          ROW({BIGINT(), BIGINT(), BIGINT(), BIGINT()}),
      };
    } else {
      rowTypes = {
          ROW({BIGINT(), VARCHAR(), VARCHAR()}),
          ROW({BIGINT(), BIGINT(), VARCHAR(), VARCHAR()}),
          ROW({BIGINT(), BIGINT(), BIGINT(), VARCHAR(), VARCHAR()}),
          ROW({BIGINT(), BIGINT(), BIGINT(), BIGINT(), VARCHAR(), VARCHAR()}),
      };
    }
    std::vector<int> numKeys = {1, 2, 3, 4};
    benchmark(
        noPayload ? "no-payload" : "payload",
        "bigint",
        batchSizes,
        rowTypes,
        numKeys,
        numIterations);
  }

  void largeBigint() {
    const auto iterations = 10;
    const std::vector<vector_size_t> batchSizes = {
        1'000, 10'000, 100'000, 1'000'000};
    bigint(true, iterations, batchSizes);
  }

  void largeBigintWithPayloads() {
    const auto iterations = 10;
    const std::vector<vector_size_t> batchSizes = {
        1'000, 10'000, 100'000, 1'000'000};
    bigint(false, iterations, batchSizes);
  }

  void largeVarchar() {
    const auto iterations = 10;
    const std::vector<vector_size_t> batchSizes = {
        1'000, 10'000, 100'000, 1'000'000};
    std::vector<RowTypePtr> rowTypes = {
        ROW({VARCHAR()}),
        ROW({VARCHAR(), VARCHAR()}),
        ROW({VARCHAR(), VARCHAR(), VARCHAR()}),
        ROW({VARCHAR(), VARCHAR(), VARCHAR(), VARCHAR()}),
    };
    std::vector<int> numKeys = {1, 2, 3, 4};
    benchmark(
        "no-payloads", "varchar", batchSizes, rowTypes, numKeys, iterations);
  }

 private:
  std::vector<std::unique_ptr<TestCase>> testCases_;
  memory::MemoryPool* pool_;
};
} // namespace

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);

  memory::MemoryManager::initialize({});
  auto rootPool = memory::memoryManager()->addRootPool();
  auto leafPool = rootPool->addLeafChild("leaf");

  SpillRadixSortBenchmark bm(leafPool.get());

  bm.largeBigint();
  bm.largeBigintWithPayloads();
  bm.largeVarchar();

  folly::runBenchmarks();
  return 0;
}
