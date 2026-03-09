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
// #include "velox/exec/HashAggregation.h"
#include "velox/common/memory/subOp/include/enhanced_hash_agg/EnhancedHashAggregation.h"

#include <optional>
#include <typeinfo>
#include <string>

#include "velox/common/memory/subOp/include/enhanced_hash_agg/EnhancedGroupingSet.h"
#include "velox/common/memory/subOp/include/enhanced_hash_agg/EnhancedSumAggregateBase.h"
#include "velox/exec/AggregateCompanionAdapter.h"
#include "velox/exec/PrefixSort.h"
#include "velox/exec/Task.h"
#include "velox/expression/Expr.h"

namespace facebook::velox::exec {

EnhancedHashAggregation::EnhancedHashAggregation(
    int32_t operatorId,
    DriverCtx* driverCtx,
    const std::shared_ptr<const core::AggregationNode>& aggregationNode)
    : Operator(
          driverCtx,
          aggregationNode->outputType(),
          operatorId,
          aggregationNode->id(),
          aggregationNode->step() == core::AggregationNode::Step::kPartial
              ? "EnhancedPartialAggregation"
              : "EnhancedHashAggregation",
          aggregationNode->canSpill(driverCtx->queryConfig())
              ? driverCtx->makeSpillConfig(operatorId)
              : std::nullopt),
      aggregationNode_(aggregationNode),
      isPartialOutput_(isPartialOutput(aggregationNode->step())),
      isGlobal_(aggregationNode->groupingKeys().empty()),
      isDistinct_(!isGlobal_ && aggregationNode->aggregates().empty()),
      maxExtendedPartialAggregationMemoryUsage_(
          driverCtx->queryConfig().maxExtendedPartialAggregationMemoryUsage()),
      abandonPartialAggregationMinRows_(
          driverCtx->queryConfig().abandonPartialAggregationMinRows()),
      abandonPartialAggregationMinPct_(
          driverCtx->queryConfig().abandonPartialAggregationMinPct()),
      maxPartialAggregationMemoryUsage_(
          driverCtx->queryConfig().maxPartialAggregationMemoryUsage()) {}

void EnhancedHashAggregation::initialize() {
  Operator::initialize();

  VELOX_CHECK(pool()->trackUsage());

  const auto& inputType = aggregationNode_->sources()[0]->outputType();
  std::vector<column_index_t> groupingKeyInputChannels;
  std::vector<column_index_t> groupingKeyOutputChannels;
  setupGroupingKeyChannelProjections(
      groupingKeyInputChannels, groupingKeyOutputChannels);

  auto hashers = createVectorHashers(inputType, groupingKeyInputChannels);
  const auto numHashers = hashers.size();

  std::vector<column_index_t> preGroupedChannels;
  preGroupedChannels.reserve(aggregationNode_->preGroupedKeys().size());
  for (const auto& key : aggregationNode_->preGroupedKeys()) {
    auto channel = exprToChannel(key.get(), inputType);
    preGroupedChannels.push_back(channel);
  }

  std::shared_ptr<core::ExpressionEvaluator> expressionEvaluator;
  std::vector<AggregateInfo> aggregateInfos = toAggregateInfo(
      *aggregationNode_, *operatorCtx_, numHashers, expressionEvaluator);

  // Replace sum aggregate functions with SVE-optimized versions if applicable.
  // Gluten Final stage uses sum_merge_extract / spark_sum_merge_extract (not
  // sum_merge). See SubstraitToVeloxPlan: kFinal -> "_merge_extract" suffix.
  const auto& aggregates = aggregationNode_->aggregates();
  auto isSumLikeForReplacement = [](const std::string& name) {
    return name == "sum" || name == "sum_partial" || name == "sum_merge" ||
           name == "sum_merge_extract" || name == "spark_sum" ||
           name == "spark_sum_partial" || name == "spark_sum_merge" ||
           name == "spark_sum_merge_extract" ||
           name.rfind("sum_merge_extract", 0) == 0 ||
           name.rfind("spark_sum_merge_extract", 0) == 0;
  };

  for (size_t i = 0; i < aggregateInfos.size() && i < aggregates.size(); ++i) {
    if (aggregateInfos[i].function && aggregates[i].call) {
      const std::string& funcName = aggregates[i].call->name();
      const auto& rawInputTypes = aggregates[i].rawInputTypes;
      const auto& resultType = aggregateInfos[i].function->resultType();

      if (!isSumLikeForReplacement(funcName) || rawInputTypes.empty() ||
          !rawInputTypes[0]->isBigint() || !resultType->isBigint()) {
        continue;
      }

      // Determine Overflow: Spark SQL uses true, Presto SQL uses false
      bool useOverflow = true;
      const std::string typeName = typeid(*aggregateInfos[i].function).name();
      if (typeName.find("prestosql") != std::string::npos ||
          typeName.find("presto") != std::string::npos) {
        useOverflow = false;
      }

      std::unique_ptr<Aggregate> baseAgg;
      if (useOverflow) {
        baseAgg = std::make_unique<
            functions::aggregate::EnhancedSumAggregateBase<int64_t, int64_t,
                                                          int64_t, true>>(
            resultType);
      } else {
        baseAgg = std::make_unique<
            functions::aggregate::EnhancedSumAggregateBase<int64_t, int64_t,
                                                          int64_t, false>>(
            resultType);
      }

      // sum_merge / spark_sum_merge: MergeFunction (intermediate stage).
      // *_merge_extract*: MergeExtractFunction (Final stage, used by Gluten).
      // sum / sum_partial: use base directly.
      const bool isMergeExtract = (funcName.find("merge_extract") !=
                                   std::string::npos);
      const bool isMerge =
          (funcName == "sum_merge" || funcName == "spark_sum_merge");
      if (isMergeExtract) {
        aggregateInfos[i].function = std::make_unique<
            AggregateCompanionAdapter::MergeExtractFunction>(std::move(baseAgg),
                                                            resultType);
      } else if (isMerge) {
        aggregateInfos[i].function = std::make_unique<
            AggregateCompanionAdapter::MergeFunction>(std::move(baseAgg),
                                                     resultType);
      } else {
        aggregateInfos[i].function = std::move(baseAgg);
      }
    }
  }

  // Check that aggregate result type match the output type.
  for (auto i = 0; i < aggregateInfos.size(); i++) {
    const auto& aggResultType = aggregateInfos[i].function->resultType();
    const auto& expectedType = outputType_->childAt(numHashers + i);
    VELOX_CHECK(
        aggResultType->kindEquals(expectedType),
        "Unexpected result type for an aggregation: {}, expected {}, step {}",
        aggResultType->toString(),
        expectedType->toString(),
        core::AggregationNode::stepName(aggregationNode_->step()));
  }

  for (auto i = 0; i < hashers.size(); ++i) {
    identityProjections_.emplace_back(
        hashers[groupingKeyOutputChannels[i]]->channel(), i);
  }

  std::optional<column_index_t> groupIdChannel;
  if (aggregationNode_->groupId().has_value()) {
    groupIdChannel = outputType_->getChildIdxIfExists(
        aggregationNode_->groupId().value()->name());
    VELOX_CHECK(groupIdChannel.has_value());
  }

  groupingSet_ = std::make_unique<EnhancedGroupingSet>(
      inputType,
      std::move(hashers),
      std::move(preGroupedChannels),
      std::move(groupingKeyOutputChannels),
      std::move(aggregateInfos),
      aggregationNode_->ignoreNullKeys(),
      isPartialOutput_,
      isRawInput(aggregationNode_->step()),
      aggregationNode_->globalGroupingSets(),
      groupIdChannel,
      spillConfig_.has_value() ? &spillConfig_.value() : nullptr,
      &nonReclaimableSection_,
      operatorCtx_.get(),
      &spillStats_);

  aggregationNode_.reset();
}

void EnhancedHashAggregation::setupGroupingKeyChannelProjections(
    std::vector<column_index_t>& groupingKeyInputChannels,
    std::vector<column_index_t>& groupingKeyOutputChannels) const {
  VELOX_CHECK(groupingKeyInputChannels.empty());
  VELOX_CHECK(groupingKeyOutputChannels.empty());

  const auto& inputType = aggregationNode_->sources()[0]->outputType();
  const auto& groupingKeys = aggregationNode_->groupingKeys();
  // The map from the grouping key output channel to the input channel.
  //
  // NOTE: grouping key output order is specified as 'groupingKeys' in
  // 'aggregationNode_'.
  std::vector<IdentityProjection> groupingKeyProjections;
  groupingKeyProjections.reserve(groupingKeys.size());
  for (auto i = 0; i < groupingKeys.size(); ++i) {
    groupingKeyProjections.emplace_back(
        exprToChannel(groupingKeys[i].get(), inputType), i);
  }

  const bool reorderGroupingKeys =
      canSpill() && spillConfig()->prefixSortEnabled();
  // If prefix sort is enabled, we need to sort the grouping key's layout in the
  // grouping set to maximize the prefix sort acceleration if spill is
  // triggered. The reorder stores the grouping key with smaller prefix sort
  // encoded size first.
  if (reorderGroupingKeys) {
    PrefixSortLayout::optimizeSortKeysOrder(inputType, groupingKeyProjections);
  }

  groupingKeyInputChannels.reserve(groupingKeys.size());
  for (auto i = 0; i < groupingKeys.size(); ++i) {
    groupingKeyInputChannels.push_back(groupingKeyProjections[i].inputChannel);
  }

  groupingKeyOutputChannels.resize(groupingKeys.size());
  if (!reorderGroupingKeys) {
    // If there is no reorder, then grouping key output channels are the same as
    // the column index order int he grouping set.
    std::iota(
        groupingKeyOutputChannels.begin(), groupingKeyOutputChannels.end(), 0);
    return;
  }

  for (auto i = 0; i < groupingKeys.size(); ++i) {
    groupingKeyOutputChannels[groupingKeyProjections[i].outputChannel] = i;
  }
}

bool EnhancedHashAggregation::abandonPartialAggregationEarly(int64_t numOutput) const {
  VELOX_CHECK(isPartialOutput_ && !isGlobal_);
  return numInputRows_ > abandonPartialAggregationMinRows_ &&
      100 * numOutput / numInputRows_ >= abandonPartialAggregationMinPct_;
}

void EnhancedHashAggregation::addInput(RowVectorPtr input) {
  if (!pushdownChecked_) {
    mayPushdown_ = operatorCtx_->driver()->mayPushdownAggregation(this);
    pushdownChecked_ = true;
  }
  if (abandonedPartialAggregation_) {
    input_ = input;
    numInputRows_ += input->size();
    return;
  }
  groupingSet_->addInput(input, mayPushdown_);
  numInputRows_ += input->size();

  updateRuntimeStats();

  // NOTE: we should not trigger partial output flush in case of global
  // aggregation as the final aggregator will handle it the same way as the
  // partial aggregator. Hence, we have to use more memory anyway.
  const bool abandonPartialEarly = isPartialOutput_ && !isGlobal_ &&
      abandonPartialAggregationEarly(groupingSet_->numDistinct());
  if (isPartialOutput_ && !isGlobal_ &&
      (abandonPartialEarly ||
       groupingSet_->isPartialFull(maxPartialAggregationMemoryUsage_))) {
    partialFull_ = true;
  }

  if (isDistinct_) {
    newDistincts_ = !groupingSet_->hasSpilled() &&
        !groupingSet_->hashLookup().newGroups.empty();

    if (newDistincts_) {
      // Save input to use for output in getOutput().
      input_ = input;
    } else {
      // If no new distinct groups (meaning we don't have anything to output),
      // then we need to ensure we 'need input'. For that we need to reset
      // the 'partial full' flag.
      partialFull_ = false;
    }
  }
}

void EnhancedHashAggregation::updateRuntimeStats() {
  // Report range sizes and number of distinct values for the group-by keys.
  const auto& hashers = groupingSet_->hashLookup().hashers;
  uint64_t asRange{0};
  uint64_t asDistinct{0};
  const auto hashTableStats = groupingSet_->hashTableStats();

  auto lockedStats = stats_.wlock();
  auto& runtimeStats = lockedStats->runtimeStats;

  for (auto i = 0; i < hashers.size(); i++) {
    hashers[i]->cardinality(0, asRange, asDistinct);
    if (asRange != VectorHasher::kRangeTooLarge) {
      runtimeStats[fmt::format("rangeKey{}", i)] = RuntimeMetric(asRange);
    }
    if (asDistinct != VectorHasher::kRangeTooLarge) {
      runtimeStats[fmt::format("distinctKey{}", i)] = RuntimeMetric(asDistinct);
    }
  }

  runtimeStats[BaseHashTable::kCapacity] =
      RuntimeMetric(hashTableStats.capacity);
  runtimeStats[BaseHashTable::kNumRehashes] =
      RuntimeMetric(hashTableStats.numRehashes);
  runtimeStats[BaseHashTable::kNumDistinct] =
      RuntimeMetric(hashTableStats.numDistinct);
  runtimeStats[BaseHashTable::kNumTombstones] =
      RuntimeMetric(hashTableStats.numTombstones);
}

void EnhancedHashAggregation::prepareOutput(vector_size_t size) {
  if (output_) {
    VectorPtr output = std::move(output_);
    BaseVector::prepareForReuse(output, size);
    output_ = std::static_pointer_cast<RowVector>(output);
  } else {
    output_ = std::static_pointer_cast<RowVector>(
        BaseVector::create(outputType_, size, pool()));
  }
}

void EnhancedHashAggregation::resetPartialOutputIfNeed() {
  if (!partialFull_) {
    return;
  }
  VELOX_DCHECK(!isGlobal_);
  const double aggregationPct =
      numOutputRows_ == 0 ? 0 : (numOutputRows_ * 1.0) / numInputRows_ * 100;
  {
    auto lockedStats = stats_.wlock();
    lockedStats->addRuntimeStat(
        "flushRowCount", RuntimeCounter(numOutputRows_));
    lockedStats->addRuntimeStat("flushTimes", RuntimeCounter(1));
    lockedStats->addRuntimeStat(
        "partialAggregationPct", RuntimeCounter(aggregationPct));
  }
  groupingSet_->resetTable(/*freeTable=*/false);
  partialFull_ = false;
  if (!finished_) {
    maybeIncreasePartialAggregationMemoryUsage(aggregationPct);
  }
  numOutputRows_ = 0;
  numInputRows_ = 0;
}

void EnhancedHashAggregation::maybeIncreasePartialAggregationMemoryUsage(
    double aggregationPct) {
  // If more than this many are unique at full memory, give up on partial agg.
  constexpr int32_t kPartialMinFinalPct = 40;
  VELOX_DCHECK(isPartialOutput_);
  // If size is at max and there still is not enough reduction, abandon partial
  // aggregation.
  if (abandonPartialAggregationEarly(numOutputRows_) ||
      (aggregationPct > kPartialMinFinalPct &&
       maxPartialAggregationMemoryUsage_ >=
           maxExtendedPartialAggregationMemoryUsage_)) {
    groupingSet_->abandonPartialAggregation();
    pool()->release();
    addRuntimeStat("abandonedPartialAggregation", RuntimeCounter(1));
    abandonedPartialAggregation_ = true;
    return;
  }
  const int64_t extendedPartialAggregationMemoryUsage = std::min(
      maxPartialAggregationMemoryUsage_ * 2,
      maxExtendedPartialAggregationMemoryUsage_);
  // Calculate the memory to reserve to bump up the aggregation buffer size. If
  // the memory reservation below succeeds, it ensures the partial aggregator
  // can allocate that much memory in next run.
  const int64_t memoryToReserve = std::max<int64_t>(
      0,
      extendedPartialAggregationMemoryUsage - groupingSet_->allocatedBytes());
  if (!pool()->maybeReserve(memoryToReserve)) {
    return;
  }
  // Update the aggregation memory usage size limit on memory reservation
  // success.
  maxPartialAggregationMemoryUsage_ = extendedPartialAggregationMemoryUsage;
  addRuntimeStat(
      "maxExtendedPartialAggregationMemoryUsage",
      RuntimeCounter(
          maxPartialAggregationMemoryUsage_, RuntimeCounter::Unit::kBytes));
}

RowVectorPtr EnhancedHashAggregation::getOutput() {
  if (finished_) {
    input_ = nullptr;
    return nullptr;
  }
  if (abandonedPartialAggregation_) {
    if (noMoreInput_) {
      finished_ = true;
    }
    if (!input_) {
      return nullptr;
    }
    prepareOutput(input_->size());
    groupingSet_->toIntermediate(input_, output_);
    numOutputRows_ += input_->size();
    input_ = nullptr;
    return output_;
  }

  // Produce results if one of the following is true:
  // - received no-more-input message;
  // - partial aggregation reached memory limit;
  // - distinct aggregation has new keys;
  // - running in partial streaming mode and have some output ready.
  if (!noMoreInput_ && !partialFull_ && !newDistincts_ &&
      !groupingSet_->hasOutput()) {
    input_ = nullptr;
    return nullptr;
  }

  if (isDistinct_) {
    return getDistinctOutput();
  }

  const auto& queryConfig = operatorCtx_->driverCtx()->queryConfig();
  const auto maxOutputRows =
      isGlobal_ ? 1 : outputBatchRows(estimatedOutputRowSize_);
  // Reuse output vectors if possible.
  prepareOutput(maxOutputRows);

  const bool hasData = groupingSet_->getOutput(
      maxOutputRows,
      queryConfig.preferredOutputBatchBytes(),
      resultIterator_,
      output_);
  if (!hasData) {
    resultIterator_.reset();
    if (noMoreInput_) {
      finished_ = true;
    }
    resetPartialOutputIfNeed();
    return nullptr;
  }
  numOutputRows_ += output_->size();
  return output_;
}

RowVectorPtr EnhancedHashAggregation::getDistinctOutput() {
  VELOX_CHECK(isDistinct_);
  VELOX_CHECK(!finished_);

  if (newDistincts_) {
    VELOX_CHECK_NOT_NULL(input_);

    auto& lookup = groupingSet_->hashLookup();
    const auto size = lookup.newGroups.size();
    BufferPtr indices = allocateIndices(size, operatorCtx_->pool());
    auto* indicesPtr = indices->asMutable<vector_size_t>();
    std::copy(lookup.newGroups.begin(), lookup.newGroups.end(), indicesPtr);
    newDistincts_ = false;
    auto output = fillOutput(size, indices);
    numOutputRows_ += size;

    // Drop reference to input_ to make it singly-referenced at the producer and
    // allow for memory reuse.
    input_ = nullptr;

    resetPartialOutputIfNeed();
    return output;
  }
  VELOX_CHECK(!newDistincts_);

  if (!groupingSet_->hasSpilled()) {
    if (noMoreInput_) {
      finished_ = true;
      if (auto numRows = groupingSet_->numDefaultGlobalGroupingSetRows()) {
        prepareOutput(numRows.value());
        if (groupingSet_->getDefaultGlobalGroupingSetOutput(
                resultIterator_, output_)) {
          numOutputRows_ += output_->size();
          return output_;
        }
      }
    }
    return nullptr;
  }

  if (!noMoreInput_) {
    return nullptr;
  }

  const auto& queryConfig = operatorCtx_->driverCtx()->queryConfig();
  const auto maxOutputRows = outputBatchRows(estimatedOutputRowSize_);
  prepareOutput(maxOutputRows);
  if (!groupingSet_->getOutput(
          maxOutputRows,
          queryConfig.preferredOutputBatchBytes(),
          resultIterator_,
          output_)) {
    finished_ = true;
    return nullptr;
  }
  numOutputRows_ += output_->size();
  return output_;
}

void EnhancedHashAggregation::noMoreInput() {
  updateEstimatedOutputRowSize();
  groupingSet_->noMoreInput();
  Operator::noMoreInput();
  // Release the extra reserved memory right after processing all the inputs.
  pool()->release();
}

bool EnhancedHashAggregation::isFinished() {
  return finished_;
}

void EnhancedHashAggregation::reclaim(
    uint64_t targetBytes,
    memory::MemoryReclaimer::Stats& stats) {
  VELOX_CHECK(canReclaim());
  VELOX_CHECK(!nonReclaimableSection_);

  if (groupingSet_ == nullptr) {
    return;
  }

  updateEstimatedOutputRowSize();

  if (noMoreInput_) {
    if (groupingSet_->hasSpilled()) {
      LOG(WARNING)
          << "Can't reclaim from aggregation operator which has spilled and is under output processing, pool "
          << pool()->name()
          << ", memory usage: " << succinctBytes(pool()->usedBytes())
          << ", reservation: " << succinctBytes(pool()->reservedBytes());
      return;
    }
    if (isDistinct_) {
      // Since we have seen all the input, we can safely reset the hash table.
      groupingSet_->resetTable(/*freeTable=*/true);
      // Release the minimum reserved memory.
      pool()->release();
      return;
    }

    // Spill all the rows starting from the next output row pointed by
    // 'resultIterator_'.
    groupingSet_->spill(resultIterator_);
  } else {
    // TODO: support fine-grain disk spilling based on 'targetBytes' after
    // having row container memory compaction support later.
    groupingSet_->spill();
  }
  VELOX_CHECK_EQ(groupingSet_->numRows(), 0);
  VELOX_CHECK_EQ(groupingSet_->numDistinct(), 0);
  // Release the minimum reserved memory.
  pool()->release();
}

void EnhancedHashAggregation::close() {
  Operator::close();

  output_ = nullptr;
  groupingSet_.reset();
}

void EnhancedHashAggregation::updateEstimatedOutputRowSize() {
  const auto optionalRowSize = groupingSet_->estimateOutputRowSize();
  if (!optionalRowSize.has_value()) {
    return;
  }

  const auto rowSize = optionalRowSize.value();

  if (!estimatedOutputRowSize_.has_value()) {
    estimatedOutputRowSize_ = rowSize;
  } else if (rowSize > estimatedOutputRowSize_.value()) {
    estimatedOutputRowSize_ = rowSize;
  }
}
} // namespace facebook::velox::exec
