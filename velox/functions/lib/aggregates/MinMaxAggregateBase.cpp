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

#include "velox/functions/lib/aggregates/MinMaxAggregateBase.h"

#include <limits>
#include "velox/exec/AggregationHook.h"
#include "velox/functions/lib/CheckNestedNulls.h"
#include "velox/functions/lib/aggregates/Compare.h"
#include "velox/functions/lib/aggregates/SimpleNumericAggregate.h"
#include "velox/functions/lib/aggregates/SingleValueAccumulator.h"
#include "velox/type/FloatingPointUtil.h"
#include "velox/vector/DecodedVector.h"
#include "velox/vector/VectorEncoding.h"

namespace facebook::velox::functions::aggregate {

namespace {

template <typename T>
struct MinMaxTrait : public std::numeric_limits<T> {};

template <typename T>
class SimpleNumericMinMaxAggregate : public SimpleNumericAggregate<T, T, T> {
  using BaseAggregate = SimpleNumericAggregate<T, T, T>;

 public:
  explicit SimpleNumericMinMaxAggregate(
      TypePtr resultType,
      TimestampPrecision precision)
      : BaseAggregate(resultType), timestampPrecision_(precision) {}

  int32_t accumulatorFixedWidthSize() const override {
    return sizeof(T);
  }

  int32_t accumulatorAlignmentSize() const override {
    if constexpr (std::is_same_v<T, int128_t>) {
      // Override 'accumulatorAlignmentSize' for UnscaledLongDecimal values as
      // it uses int128_t type. Some CPUs don't support misaligned access to
      // int128_t type.
      return static_cast<int32_t>(sizeof(int128_t));
    } else {
      return 1;
    }
  }

  bool supportsToIntermediate() const override {
    return true;
  }

  void toIntermediate(
      const SelectivityVector& rows,
      std::vector<VectorPtr>& args,
      VectorPtr& result) const override {
    this->singleInputAsIntermediate(rows, args, result);
  }

  void extractValues(char** groups, int32_t numGroups, VectorPtr* result)
      override {
    if constexpr (std::is_same_v<T, Timestamp>) {
      // Truncate timestamps to corresponding precision.
      BaseAggregate::template doExtractValues<Timestamp>(
          groups, numGroups, result, [&](char* group) {
            auto ts =
                *BaseAggregate::Aggregate::template value<Timestamp>(group);
            return Timestamp::truncate(ts, timestampPrecision_);
          });
    } else {
      BaseAggregate::template doExtractValues<T>(
          groups, numGroups, result, [&](char* group) {
            return *BaseAggregate::Aggregate::template value<T>(group);
          });
    }
  }

  void extractAccumulators(char** groups, int32_t numGroups, VectorPtr* result)
      override {
    BaseAggregate::template doExtractValues<T>(
        groups, numGroups, result, [&](char* group) {
          return *BaseAggregate::Aggregate::template value<T>(group);
        });
  }

 private:
  const TimestampPrecision timestampPrecision_;
};

template <typename T>
class SimpleNumericMaxAggregate : public SimpleNumericMinMaxAggregate<T> {
  using BaseAggregate = SimpleNumericAggregate<T, T, T>;

 public:
  explicit SimpleNumericMaxAggregate(
      TypePtr resultType,
      TimestampPrecision precision = TimestampPrecision::kMilliseconds)
      : SimpleNumericMinMaxAggregate<T>(resultType, precision) {}

  void addRawInput(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool mayPushdown) override {
    if constexpr (BaseAggregate::template kMayPushdown<T>) {
      if (!args[0]->type()->isDecimal()) {
        if (mayPushdown && args[0]->isLazy()) {
          BaseAggregate::template pushdown<
              velox::aggregate::MinMaxHook<T, false>>(groups, rows, args[0]);
          return;
        }
      } else {
        mayPushdown = false;
      }
    } else {
      mayPushdown = false;
    }
    BaseAggregate::template updateGroups<true, T>(
        groups, rows, args[0], updateGroup, mayPushdown);
  }

  void addIntermediateResults(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool mayPushdown) override {
    addRawInput(groups, rows, args, mayPushdown);
  }

  void addSingleGroupRawInput(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool mayPushdown) override {
    BaseAggregate::updateOneGroup(
        group,
        rows,
        args[0],
        updateGroup,
        [](T& result, T value, int /* unused */) { result = value; },
        mayPushdown,
        kInitialValue_);
  }

  void addSingleGroupIntermediateResults(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool mayPushdown) override {
    addSingleGroupRawInput(group, rows, args, mayPushdown);
  }

 protected:
  void initializeNewGroupsInternal(
      char** groups,
      folly::Range<const vector_size_t*> indices) override {
    exec::Aggregate::setAllNulls(groups, indices);
    for (auto i : indices) {
      *exec::Aggregate::value<T>(groups[i]) = kInitialValue_;
    }
  }

  static inline void updateGroup(T& result, T value) {
    if constexpr (std::is_floating_point_v<T>) {
      if (util::floating_point::NaNAwareLessThan<T>{}(result, value)) {
        result = value;
      }
    } else {
      if (result < value) {
        result = value;
      }
    }
  }

 private:
  static const T kInitialValue_;
};

// max(bigint): micro-tuning for int64_t only (see standalone max_int64 benchmark).
// - Ternary-style max on the hot compare.
// - When input is flat int64, decoded has no nulls, identity mapping, and all
// batch rows are selected, use a 4x unrolled update loop.
template <bool IsMin>
class SimpleNumericInt64Aggregate : public SimpleNumericMinMaxAggregate<int64_t> {
    using BaseAggregate = SimpleNumericAggregate<int64_t, int64_t, int64_t>;

public:
    explicit SimpleNumericInt64Aggregate(TypePtr resultType,
                                         TimestampPrecision precision = TimestampPrecision::kMilliseconds)
        : SimpleNumericMinMaxAggregate<int64_t>(resultType, precision)
    {
    }

    void addRawInput(char **groups, const SelectivityVector &rows, const std::vector<VectorPtr> &args,
                     bool mayPushdown) override
    {
        if constexpr (BaseAggregate::template kMayPushdown<int64_t>) {
            if (!args[0]->type()->isDecimal()) {
                if (mayPushdown && args[0]->isLazy()) {
                    BaseAggregate::template pushdown<velox::aggregate::MinMaxHook<int64_t, IsMin>>(groups, rows,
                                                                                                   args[0]);
                    return;
                }
            } else {
                mayPushdown = false;
            }
        } else {
            mayPushdown = false;
        }

        DecodedVector decoded(*args[0], rows, !mayPushdown);
        const auto encoding = decoded.base()->encoding();
        if constexpr (BaseAggregate::template kMayPushdown<int64_t>) {
            if (encoding == VectorEncoding::Simple::LAZY && !args[0]->type()->isDecimal()) {
                BaseAggregate::template pushdown<velox::aggregate::MinMaxHook<int64_t, IsMin>>(groups, rows, args[0]);
                return;
            }
        }

        if (decoded.isConstantMapping()) {
            if (!decoded.isNullAt(0)) {
                const auto value = decoded.valueAt<int64_t>(0);
                rows.applyToSelected([&](vector_size_t row) { updateNonNullGroup(groups[row], value); });
            }
            return;
        }

        if (decoded.mayHaveNulls()) {
            rows.applyToSelected([&](vector_size_t row) {
                if (decoded.isNullAt(row)) {
                    return;
                }
                updateNonNullGroup(groups[row], decoded.valueAt<int64_t>(row));
            });
            return;
        }

        if (decoded.isIdentityMapping()) {
            const int64_t *data = decoded.data<int64_t>();
            if (rows.isAllSelected()) {
                const vector_size_t begin = rows.begin();
                const vector_size_t end = rows.end();
                vector_size_t row = begin;
                const vector_size_t n = end - begin;
                const vector_size_t unrollEnd = begin + (n & ~static_cast<vector_size_t>(3));
                for (; row < unrollEnd; row += 4) {
                    updateNonNullGroup(groups[row], data[row]);
                    updateNonNullGroup(groups[row + 1], data[row + 1]);
                    updateNonNullGroup(groups[row + 2], data[row + 2]);
                    updateNonNullGroup(groups[row + 3], data[row + 3]);
                }
                for (; row < end; ++row) {
                    updateNonNullGroup(groups[row], data[row]);
                }
                return;
            }
            rows.applyToSelected([&](vector_size_t row) { updateNonNullGroup(groups[row], data[row]); });
            return;
        }

        rows.applyToSelected(
            [&](vector_size_t row) { updateNonNullGroup(groups[row], decoded.valueAt<int64_t>(row)); });
    }

    void addIntermediateResults(char **groups, const SelectivityVector &rows, const std::vector<VectorPtr> &args,
                                bool mayPushdown) override
    {
        addRawInput(groups, rows, args, mayPushdown);
    }

    void addSingleGroupRawInput(char *group, const SelectivityVector &rows, const std::vector<VectorPtr> &args,
                                bool mayPushdown) override
    {
        BaseAggregate::updateOneGroup(
            group, rows, args[0], updateGroup, [](int64_t &result, int64_t value, int /* unused */) { result = value; },
            mayPushdown, kInitialValue_);
    }

    void addSingleGroupIntermediateResults(char *group, const SelectivityVector &rows,
                                           const std::vector<VectorPtr> &args, bool mayPushdown) override
    {
        addSingleGroupRawInput(group, rows, args, mayPushdown);
    }

protected:
    void initializeNewGroupsInternal(char **groups, folly::Range<const vector_size_t *> indices) override
    {
        this->setAllNulls(groups, indices);
        for (auto i : indices) {
            *this->template value<int64_t>(groups[i]) = kInitialValue_;
        }
    }

    static inline void updateGroup(int64_t &result, int64_t value)
    {
        if constexpr (IsMin) {
            result = result <= value ? result : value;
        } else {
            result = result >= value ? result : value;
        }
    }

private:
    static const int64_t kInitialValue_;

    inline void updateNonNullGroup(char *group, int64_t value)
    {
        this->clearNull(group);
        int64_t *acc = this->template value<int64_t>(group);
        const int64_t x = *acc;
        if constexpr (IsMin) {
            *acc = x <= value ? x : value;
        } else {
            *acc = x >= value ? x : value;
        }
    }
};

template <>
const int64_t SimpleNumericInt64Aggregate<false>::kInitialValue_ = MinMaxTrait<int64_t>::lowest();

template <>
const int64_t SimpleNumericInt64Aggregate<true>::kInitialValue_ = MinMaxTrait<int64_t>::max();

template <>
class SimpleNumericMaxAggregate<int64_t> : public SimpleNumericInt64Aggregate<false> {
public:
    using SimpleNumericInt64Aggregate<false>::SimpleNumericInt64Aggregate;
};

template <typename T>
const T SimpleNumericMaxAggregate<T>::kInitialValue_ = MinMaxTrait<T>::lowest();

template <>
const float SimpleNumericMaxAggregate<float>::kInitialValue_ =
    -1 * MinMaxTrait<float>::infinity();

template <>
const double SimpleNumericMaxAggregate<double>::kInitialValue_ =
    -1 * MinMaxTrait<double>::infinity();

template <typename T>
class SimpleNumericMinAggregate : public SimpleNumericMinMaxAggregate<T> {
  using BaseAggregate = SimpleNumericAggregate<T, T, T>;

 public:
  explicit SimpleNumericMinAggregate(
      TypePtr resultType,
      TimestampPrecision precision = TimestampPrecision::kMilliseconds)
      : SimpleNumericMinMaxAggregate<T>(resultType, precision) {}

  void addRawInput(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool mayPushdown) override {
    if constexpr (BaseAggregate::template kMayPushdown<T>) {
      if (!args[0]->type()->isDecimal()) {
        if (mayPushdown && args[0]->isLazy()) {
          BaseAggregate::template pushdown<
              velox::aggregate::MinMaxHook<T, true>>(groups, rows, args[0]);
          return;
        }
      } else {
        mayPushdown = false;
      }
    } else {
      mayPushdown = false;
    }
    BaseAggregate::template updateGroups<true, T>(
        groups, rows, args[0], updateGroup, mayPushdown);
  }

  void addIntermediateResults(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool mayPushdown) override {
    addRawInput(groups, rows, args, mayPushdown);
  }

  void addSingleGroupRawInput(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool mayPushdown) override {
    BaseAggregate::updateOneGroup(
        group,
        rows,
        args[0],
        updateGroup,
        [](T& result, T value, int /* unused */) { result = value; },
        mayPushdown,
        kInitialValue_);
  }

  void addSingleGroupIntermediateResults(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool mayPushdown) override {
    addSingleGroupRawInput(group, rows, args, mayPushdown);
  }

 protected:
  static inline void updateGroup(T& result, T value) {
    if constexpr (std::is_floating_point_v<T>) {
      if (util::floating_point::NaNAwareGreaterThan<T>{}(result, value)) {
        result = value;
      }
    } else {
      if (result > value) {
        result = value;
      }
    }
  }

  void initializeNewGroupsInternal(
      char** groups,
      folly::Range<const vector_size_t*> indices) override {
    exec::Aggregate::setAllNulls(groups, indices);
    for (auto i : indices) {
      *exec::Aggregate::value<T>(groups[i]) = kInitialValue_;
    }
  }

 private:
  static const T kInitialValue_;
};

template <>
class SimpleNumericMinAggregate<int64_t> : public SimpleNumericInt64Aggregate<true> {
public:
    using SimpleNumericInt64Aggregate<true>::SimpleNumericInt64Aggregate;
};

template <typename T>
const T SimpleNumericMinAggregate<T>::kInitialValue_ = MinMaxTrait<T>::max();

// In velox, NaN is considered larger than infinity for floating point types.
template <>
const float SimpleNumericMinAggregate<float>::kInitialValue_ =
    MinMaxTrait<float>::quiet_NaN();

template <>
const double SimpleNumericMinAggregate<double>::kInitialValue_ =
    MinMaxTrait<double>::quiet_NaN();

class MinMaxAggregateBase : public exec::Aggregate {
 public:
  explicit MinMaxAggregateBase(
      const TypePtr& resultType,
      bool throwOnNestedNulls)
      : exec::Aggregate(resultType), throwOnNestedNulls_(throwOnNestedNulls) {}

  int32_t accumulatorFixedWidthSize() const override {
    return sizeof(SingleValueAccumulator);
  }

  bool supportsToIntermediate() const override {
    return true;
  }

  void toIntermediate(
      const SelectivityVector& rows,
      std::vector<VectorPtr>& args,
      VectorPtr& result) const override {
    const auto& input = args[0];

    if (throwOnNestedNulls_) {
      DecodedVector decoded(*input, rows, true);
      auto indices = decoded.indices();
      rows.applyToSelected([&](vector_size_t i) {
        velox::functions::checkNestedNulls(
            decoded, indices, i, throwOnNestedNulls_);
      });
    }

    if (rows.isAllSelected()) {
      result = input;
      return;
    }

    auto* pool = allocator_->pool();

    // Set result to NULL for rows that are masked out.
    BufferPtr nulls = allocateNulls(rows.size(), pool, bits::kNull);
    rows.clearNulls(nulls);

    BufferPtr indices = allocateIndices(rows.size(), pool);
    auto* rawIndices = indices->asMutable<vector_size_t>();
    std::iota(rawIndices, rawIndices + rows.size(), 0);

    result = BaseVector::wrapInDictionary(nulls, indices, rows.size(), input);
  }

  void extractValues(char** groups, int32_t numGroups, VectorPtr* result)
      override {
    VELOX_CHECK(result);
    (*result)->resize(numGroups);

    uint64_t* rawNulls = nullptr;
    if ((*result)->mayHaveNulls()) {
      BufferPtr& nulls = (*result)->mutableNulls((*result)->size());
      rawNulls = nulls->asMutable<uint64_t>();
    }

    for (auto i = 0; i < numGroups; ++i) {
      char* group = groups[i];
      auto accumulator = value<SingleValueAccumulator>(group);
      if (!accumulator->hasValue()) {
        (*result)->setNull(i, true);
      } else {
        if (rawNulls) {
          bits::clearBit(rawNulls, i);
        }
        accumulator->read(*result, i);
      }
    }
  }

  void extractAccumulators(char** groups, int32_t numGroups, VectorPtr* result)
      override {
    // partial and final aggregations are the same
    extractValues(groups, numGroups, result);
  }

 protected:
  template <
      typename TCompareTest,
      CompareFlags::NullHandlingMode nullHandlingMode>
  void doUpdate(
      char** groups,
      const SelectivityVector& rows,
      const VectorPtr& arg,
      TCompareTest compareTest) {
    DecodedVector decoded(*arg, rows, true);
    auto indices = decoded.indices();
    auto baseVector = decoded.base();

    if (decoded.isConstantMapping() && decoded.isNullAt(0)) {
      // nothing to do; all values are nulls
      return;
    }

    rows.applyToSelected([&](vector_size_t i) {
      if (velox::functions::checkNestedNulls(
              decoded, indices, i, throwOnNestedNulls_)) {
        return;
      }

      auto accumulator = value<SingleValueAccumulator>(groups[i]);
      if (!accumulator->hasValue() ||
          compareTest(compare(accumulator, decoded, i, nullHandlingMode))) {
        accumulator->write(baseVector, indices[i], allocator_);
      }
    });
  }

  template <
      typename TCompareTest,
      CompareFlags::NullHandlingMode nullHandlingMode>
  void doUpdateSingleGroup(
      char* group,
      const SelectivityVector& rows,
      const VectorPtr& arg,
      TCompareTest compareTest) {
    DecodedVector decoded(*arg, rows, true);
    auto indices = decoded.indices();
    auto baseVector = decoded.base();

    if (decoded.isConstantMapping()) {
      if (velox::functions::checkNestedNulls(
              decoded, indices, 0, throwOnNestedNulls_)) {
        return;
      }

      auto accumulator = value<SingleValueAccumulator>(group);
      if (!accumulator->hasValue() ||
          compareTest(compare(accumulator, decoded, 0, nullHandlingMode))) {
        accumulator->write(baseVector, indices[0], allocator_);
      }
      return;
    }

    auto accumulator = value<SingleValueAccumulator>(group);
    rows.applyToSelected([&](vector_size_t i) {
      if (velox::functions::checkNestedNulls(
              decoded, indices, i, throwOnNestedNulls_)) {
        return;
      }
      if (!accumulator->hasValue() ||
          compareTest(compare(accumulator, decoded, i, nullHandlingMode))) {
        accumulator->write(baseVector, indices[i], allocator_);
      }
    });
  }

  void initializeNewGroupsInternal(
      char** groups,
      folly::Range<const vector_size_t*> indices) override {
    exec::Aggregate::setAllNulls(groups, indices);
    for (auto i : indices) {
      new (groups[i] + offset_) SingleValueAccumulator();
    }
  }

  void destroyInternal(folly::Range<char**> groups) override {
    for (auto group : groups) {
      if (isInitialized(group)) {
        value<SingleValueAccumulator>(group)->destroy(allocator_);
      }
    }
  }

 private:
  const bool throwOnNestedNulls_;
};

template <CompareFlags::NullHandlingMode nullHandlingMode>
class MaxAggregate : public MinMaxAggregateBase {
 public:
  explicit MaxAggregate(const TypePtr& resultType, bool throwOnNestedNulls)
      : MinMaxAggregateBase(resultType, throwOnNestedNulls) {}

  void addRawInput(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /*mayPushdown*/) override {
    doUpdate<std::function<bool(int32_t)>, nullHandlingMode>(
        groups, rows, args[0], [](int32_t compareResult) {
          return compareResult < 0;
        });
  }

  void addIntermediateResults(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool mayPushdown) override {
    addRawInput(groups, rows, args, mayPushdown);
  }

  void addSingleGroupRawInput(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /*mayPushdown*/) override {
    doUpdateSingleGroup<std::function<bool(int32_t)>, nullHandlingMode>(
        group, rows, args[0], [](int32_t compareResult) {
          return compareResult < 0;
        });
  }

  void addSingleGroupIntermediateResults(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool mayPushdown) override {
    addSingleGroupRawInput(group, rows, args, mayPushdown);
  }
};

template <CompareFlags::NullHandlingMode nullHandlingMode>
class MinAggregate : public MinMaxAggregateBase {
 public:
  explicit MinAggregate(const TypePtr& resultType, bool throwOnNestedNulls)
      : MinMaxAggregateBase(resultType, throwOnNestedNulls) {}

  void addRawInput(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /*mayPushdown*/) override {
    doUpdate<std::function<bool(int32_t)>, nullHandlingMode>(
        groups, rows, args[0], [](int32_t compareResult) {
          return compareResult > 0;
        });
  }

  void addIntermediateResults(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool mayPushdown) override {
    addRawInput(groups, rows, args, mayPushdown);
  }

  void addSingleGroupRawInput(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /*mayPushdown*/) override {
    doUpdateSingleGroup<std::function<bool(int32_t)>, nullHandlingMode>(
        group, rows, args[0], [](int32_t compareResult) {
          return compareResult > 0;
        });
  }

  void addSingleGroupIntermediateResults(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool mayPushdown) override {
    addSingleGroupRawInput(group, rows, args, mayPushdown);
  }
};

template <
    template <typename T>
    class TSimpleNumericAggregate,
    template <CompareFlags::NullHandlingMode nullHandlingMode>
    typename TAggregate>
exec::AggregateFunctionFactory getMinMaxFunctionFactoryInternal(
    const std::string& name,
    CompareFlags::NullHandlingMode nullHandlingMode,
    TimestampPrecision precision) {
  auto factory = [name, nullHandlingMode, precision](
                     core::AggregationNode::Step step,
                     std::vector<TypePtr> argTypes,
                     const TypePtr& resultType,
                     const core::QueryConfig& /*config*/)
      -> std::unique_ptr<exec::Aggregate> {
    auto inputType = argTypes[0];

    if (inputType->providesCustomComparison()) {
      return std::make_unique<
          TAggregate<CompareFlags::NullHandlingMode::kNullAsIndeterminate>>(
          inputType, false);
    }

    switch (inputType->kind()) {
      case TypeKind::BOOLEAN:
        return std::make_unique<TSimpleNumericAggregate<bool>>(resultType);
      case TypeKind::TINYINT:
        return std::make_unique<TSimpleNumericAggregate<int8_t>>(resultType);
      case TypeKind::SMALLINT:
        return std::make_unique<TSimpleNumericAggregate<int16_t>>(resultType);
      case TypeKind::INTEGER:
        return std::make_unique<TSimpleNumericAggregate<int32_t>>(resultType);
      case TypeKind::BIGINT:
        return std::make_unique<TSimpleNumericAggregate<int64_t>>(resultType);
      case TypeKind::REAL:
        return std::make_unique<TSimpleNumericAggregate<float>>(resultType);
      case TypeKind::DOUBLE:
        return std::make_unique<TSimpleNumericAggregate<double>>(resultType);
      case TypeKind::TIMESTAMP:
        return std::make_unique<TSimpleNumericAggregate<Timestamp>>(
            resultType, precision);
      case TypeKind::HUGEINT:
        return std::make_unique<TSimpleNumericAggregate<int128_t>>(resultType);
      case TypeKind::VARBINARY:
        [[fallthrough]];
      case TypeKind::VARCHAR:
        return std::make_unique<
            TAggregate<CompareFlags::NullHandlingMode::kNullAsIndeterminate>>(
            inputType, false);
      case TypeKind::ARRAY:
        [[fallthrough]];
      case TypeKind::ROW:
        if (nullHandlingMode == CompareFlags::NullHandlingMode::kNullAsValue) {
          return std::make_unique<
              TAggregate<CompareFlags::NullHandlingMode::kNullAsValue>>(
              inputType, false);
        } else {
          return std::make_unique<
              TAggregate<CompareFlags::NullHandlingMode::kNullAsIndeterminate>>(
              inputType, true);
        }
      case TypeKind::UNKNOWN:
        return std::make_unique<TSimpleNumericAggregate<UnknownValue>>(
            resultType);
      default:
        VELOX_UNREACHABLE(
            "Unknown input type for {} aggregation {}",
            name,
            inputType->kindName());
    }
  };
  return factory;
}

} // namespace

exec::AggregateFunctionFactory getMinFunctionFactory(
    const std::string& name,
    CompareFlags::NullHandlingMode nullHandlingMode,
    TimestampPrecision precision) {
  return getMinMaxFunctionFactoryInternal<
      SimpleNumericMinAggregate,
      MinAggregate>(name, nullHandlingMode, precision);
}

exec::AggregateFunctionFactory getMaxFunctionFactory(
    const std::string& name,
    CompareFlags::NullHandlingMode nullHandlingMode,
    TimestampPrecision precision) {
  return getMinMaxFunctionFactoryInternal<
      SimpleNumericMaxAggregate,
      MaxAggregate>(name, nullHandlingMode, precision);
}
} // namespace facebook::velox::functions::aggregate
