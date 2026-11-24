/*
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

#include <unordered_map>

#include "velox/common/base/RuntimeMetrics.h"
#include "velox/common/base/SpillStats.h"
#include "velox/exec/AggregateInfo.h"
#include "velox/exec/HashTable.h"
#include "velox/vector/ComplexVector.h"

namespace facebook::velox::exec {

using RuntimeStats = std::unordered_map<std::string, RuntimeMetric>;

class GroupAggregation {
 public:
  virtual void addInput(const RowVectorPtr& input, bool mayPushdown) = 0;

  virtual void noMoreInput() = 0;

  /// Typically, the output is not available until all input has been added.
  /// However, in case when input is clustered on some of the grouping keys, the
  /// output becomes available every time one of these grouping keys changes
  /// value. This method returns true if no-more-input message has been received
  /// or if some groups are ready for output because pre-grouped keys values
  /// have changed.
  virtual bool hasOutput() = 0;

  /// Called if partial aggregation has reached memory limit or if hasOutput()
  /// returns true. 'maxOutputRows' and 'maxOutputBytes' specify the max number
  /// of rows/bytes to return in 'result' respectively. The function stops
  /// producing output if it exceeds either limit.
  virtual bool getOutput(
      int32_t maxOutputRows,
      int32_t maxOutputBytes,
      RowContainerIterator& iterator,
      RowVectorPtr& result) = 0;

  virtual uint64_t allocatedBytes() const = 0;

  /// Resets the hash table inside the grouping set when partial aggregation
  /// is full or reclaims memory from distinct aggregation after it has received
  /// all the inputs. If 'freeTable' is false, then hash table itself is not
  /// freed but only table content.
  virtual void resetTable(bool freeTable = true) = 0;

  /// Returns true if 'this' should start producing partial
  /// aggregation results. Checks the memory consumption against
  /// 'maxBytes'. If exceeding 'maxBytes', sees if changing hash mode
  /// can free up space and rehashes and returns false if significant
  /// space was recovered. In specific, changing from an array hash
  /// based on value ranges to one based on value ids can save a lot.
  virtual bool isPartialFull(int64_t maxBytes) = 0;

  /// Returns the count of the hash table, if any.
  virtual int64_t numDistinct() const = 0;

  /// Returns number of global grouping sets rows if there is default output.
  virtual std::optional<vector_size_t> numDefaultGlobalGroupingSetRows()
      const = 0;

  /// Spills all the rows in container.
  virtual void spill() = 0;

  /// Spills all the rows in container starting from the offset specified by
  /// 'rowIterator'. This should be only called during output processing and
  /// when no spill has occurred previously.
  virtual void spill(const RowContainerIterator& rowIterator) = 0;

  /// Returns the spiller stats including total bytes and rows spilled so far.
  virtual std::optional<common::SpillStats> spilledStats() const = 0;

  /// Returns true if spilling has triggered on this grouping set.
  virtual bool hasSpilled() const = 0;

  /// Return the number of rows kept in memory.
  virtual int64_t numRows() const = 0;

  /// Frees hash tables and other state when giving up partial aggregation as
  /// non-productive. Must be called before toIntermediate() is used.
  virtual void abandonPartialAggregation() = 0;

  /// Translates the raw input in input to accumulators initialized from a
  /// single input row. Passes grouping keys through.
  virtual void toIntermediate(
      const RowVectorPtr& input,
      RowVectorPtr& result) = 0;

  /// Returns default global grouping sets output if there are no input rows.
  /// The default global grouping set output is a single row per global grouping
  /// set with the groupId key and the default aggregate value.
  /// This function can also be used with distinct aggregations.
  virtual bool getDefaultGlobalGroupingSetOutput(
      RowContainerIterator& iterator,
      RowVectorPtr& result) = 0;

  virtual memory::MemoryPool& testingPool() const = 0;

  virtual std::optional<int64_t> estimateOutputRowSize() const = 0;

  virtual void updateRuntimeStats(RuntimeStats& runtimeStats) = 0;

  virtual size_t distinctNewGroupsNum() = 0;

  virtual void resetDistinctNewGroups() {}

  virtual void putDistinctNewGroupsIndices(int64_t offset) {}

  virtual void getNewGroupIndices(BufferPtr indices) = 0;

  virtual ~GroupAggregation() {}
};

struct GroupAggParams {
  const RowTypePtr& inputType;
  std::vector<std::unique_ptr<VectorHasher>> hashers;
  std::vector<column_index_t> preGroupedKeys;
  std::vector<AggregateInfo> aggregates;
  bool isPartial;
  bool isRawInput;
  const std::vector<vector_size_t>& globalGroupingSets;
  const std::optional<column_index_t>& groupIdChannel;
  const common::SpillConfig* spillConfig;
  tsan_atomic<bool>* nonReclaimableSection;
  OperatorCtx* operatorCtx;
  bool supportFlatAgg;
};

} // namespace facebook::velox::exec
