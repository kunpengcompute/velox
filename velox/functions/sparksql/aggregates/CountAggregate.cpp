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
#include "velox/functions/sparksql/aggregates/CountAggregate.h"

#include "velox/common/base/BitUtil.h"
#include "velox/common/base/Exceptions.h"
#include "velox/exec/AggregationHook.h"
#include "velox/expression/FunctionSignature.h"
#include "velox/functions/lib/aggregates/SumAggregateBase.h"
#include "velox/vector/FlatVector.h"
#include "velox/vector/LazyVector.h"

using namespace facebook::velox::functions::aggregate;

namespace facebook::velox::functions::aggregate::sparksql {

namespace {

class CountAggregate : public SimpleNumericAggregate<bool, int64_t, int64_t> {
  using BaseAggregate = SimpleNumericAggregate<bool, int64_t, int64_t>;

 public:
  explicit CountAggregate() : BaseAggregate(BIGINT()) {}

  int32_t accumulatorFixedWidthSize() const override {
    return sizeof(int64_t);
  }

  bool supportsToIntermediate() const override {
    return true;
  }

  void toIntermediate(
      const SelectivityVector& rows,
      std::vector<VectorPtr>& args,
      VectorPtr& result) const override {
    auto* flat = result->asFlatVector<int64_t>();
    VELOX_CHECK_NOT_NULL(flat);
    flat->resize(rows.size());
    flat->clearAllNulls();
    int64_t* values = flat->mutableRawValues();

    // count(*): one per selected row, zero for masked-out rows.
    if (args.empty()) {
      if (rows.isAllSelected()) {
        std::fill_n(values, rows.size(), int64_t(1));
      } else {
        std::memset(values, 0, rows.size() * sizeof(int64_t));
        rows.applyToSelected(
            [&](vector_size_t i) { values[i] = 1; });
      }
      return;
    }

    const auto& arg = args[0];
    LazyVector::ensureLoadedRows(arg, rows);
    const BaseVector* base = arg->loadedVector();

    // Zero the unselected slots first; they will be left as 0.
    if (!rows.isAllSelected()) {
      std::memset(values, 0, rows.size() * sizeof(int64_t));
    }

    // Fast path: FLAT input.
    if (base->isFlatEncoding()) {
      const uint64_t* rawNulls = base->rawNulls();
      if (rawNulls == nullptr) {
        if (rows.isAllSelected()) {
          std::fill_n(values, rows.size(), int64_t(1));
        } else {
          rows.applyToSelected(
              [&](vector_size_t i) { values[i] = 1; });
        }
      } else {
        // bit == 1 means non-null.
        if (rows.isAllSelected()) {
          std::memset(values, 0, rows.size() * sizeof(int64_t));
          bits::forEachWord(
              rows.begin(),
              rows.end(),
              [&](int32_t wordIdx, uint64_t mask) {
                uint64_t word = rawNulls[wordIdx] & mask;
                while (word) {
                  const auto bit = __builtin_ctzll(word);
                  values[static_cast<vector_size_t>(wordIdx) * 64 + bit] = 1;
                  word &= word - 1;
                }
              });
        } else {
          const uint64_t* selectedBits = rows.allBits();
          bits::forEachWord(
              rows.begin(),
              rows.end(),
              [&](int32_t wordIdx, uint64_t mask) {
                uint64_t word =
                    (selectedBits[wordIdx] & rawNulls[wordIdx]) & mask;
                while (word) {
                  const auto bit = __builtin_ctzll(word);
                  values[static_cast<vector_size_t>(wordIdx) * 64 + bit] = 1;
                  word &= word - 1;
                }
              });
        }
      }
      return;
    }

    // Fast path: CONSTANT.
    if (base->encoding() == VectorEncoding::Simple::CONSTANT) {
      if (!base->isNullAt(0)) {
        if (rows.isAllSelected()) {
          std::fill_n(values, rows.size(), int64_t(1));
        } else {
          rows.applyToSelected(
              [&](vector_size_t i) { values[i] = 1; });
        }
      } else if (rows.isAllSelected()) {
        std::memset(values, 0, rows.size() * sizeof(int64_t));
      }
      return;
    }

    // Fallback: dictionary / unusual encodings.
    DecodedVector decoded(*arg, rows);
    if (decoded.isConstantMapping()) {
      const int64_t v = decoded.isNullAt(0) ? 0 : 1;
      if (rows.isAllSelected()) {
        std::fill_n(values, rows.size(), v);
      } else if (v) {
        rows.applyToSelected(
            [&](vector_size_t i) { values[i] = 1; });
      }
    } else if (decoded.mayHaveNulls()) {
      rows.applyToSelected(
          [&](vector_size_t i) { values[i] = decoded.isNullAt(i) ? 0 : 1; });
    } else {
      rows.applyToSelected([&](vector_size_t i) { values[i] = 1; });
    }
  }

  void extractValues(char** groups, int32_t numGroups, VectorPtr* result)
      override {
    BaseAggregate::doExtractValues(groups, numGroups, result, [&](char* group) {
      return *value<int64_t>(group);
    });
  }

  void addRawInput(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /*mayPushdown*/) override {
    // count(*) fast path.
    if (args.empty()) {
      addOneToEachGroupBulk(groups, rows);
      return;
    }

    const auto& arg = args[0];
    LazyVector::ensureLoadedRows(arg, rows);
    const BaseVector* base = arg->loadedVector();

    // ---- Fast path: FLAT input. Bypass DecodedVector entirely. ----
    if (base->isFlatEncoding()) {
      const uint64_t* rawNulls = base->rawNulls();
      if (rawNulls == nullptr) {
        addOneToEachGroupBulk(groups, rows);
      } else {
        addOneToEachNonNullGroupBulk(groups, rows, rawNulls);
      }
      return;
    }

    // ---- Fast path: CONSTANT input. ----
    if (base->encoding() == VectorEncoding::Simple::CONSTANT) {
      if (!base->isNullAt(0)) {
        addOneToEachGroupBulk(groups, rows);
      }
      return;
    }

    // ---- Fallback: generic decoded path. ----
    DecodedVector decoded(*arg, rows);
    if (decoded.isConstantMapping()) {
      if (!decoded.isNullAt(0)) {
        addOneToEachGroupBulk(groups, rows);
      }
    } else if (decoded.mayHaveNulls()) {
      rows.applyToSelected([&](vector_size_t i) {
        if (decoded.isNullAt(i)) {
          return;
        }
        addToGroup(groups[i], 1);
      });
    } else {
      addOneToEachGroupBulk(groups, rows);
    }
  }

  void addIntermediateResults(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /*mayPushdown*/) override {
    const auto& arg = args[0];
    LazyVector::ensureLoadedRows(arg, rows);
    const BaseVector* base = arg->loadedVector();

    // Fast path: partial-stage output is a FLAT BIGINT column with no nulls.
    if (base->isFlatEncoding()) {
      const int64_t* rawValues =
          base->asUnchecked<FlatVector<int64_t>>()->rawValues();
      const uint64_t* rawNulls = base->rawNulls();
      if (rawNulls == nullptr) {
        if (rows.isAllSelected()) {
          const auto end = rows.end();
          for (vector_size_t i = rows.begin(); i < end; ++i) {
            *value<int64_t>(groups[i]) += rawValues[i];
          }
        } else {
          rows.applyToSelected([&](vector_size_t i) {
            *value<int64_t>(groups[i]) += rawValues[i];
          });
        }
      } else {
        rows.applyToSelected([&](vector_size_t i) {
          if (bits::isBitSet(rawNulls, i)) {
            *value<int64_t>(groups[i]) += rawValues[i];
          }
        });
      }
      return;
    }

    // Fallback: generic decoded path.
    decodedIntermediate_.decode(*arg, rows);
    rows.applyToSelected([&](vector_size_t i) {
      addToGroup(groups[i], decodedIntermediate_.valueAt<int64_t>(i));
    });
  }

  void addSingleGroupRawInput(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /*mayPushdown*/) override {
    if (args.empty()) {
      addToGroup(group, rows.countSelected());
      return;
    }

    const auto& arg = args[0];
    LazyVector::ensureLoadedRows(arg, rows);
    const BaseVector* base = arg->loadedVector();

    // Fast path: FLAT input - count non-nulls via popcount.
    if (base->isFlatEncoding()) {
      const uint64_t* rawNulls = base->rawNulls();
      if (rawNulls == nullptr) {
        addToGroup(group, rows.countSelected());
      } else {
        addToGroup(group, countNonNulls(rows, rawNulls));
      }
      return;
    }

    if (base->encoding() == VectorEncoding::Simple::CONSTANT) {
      if (!base->isNullAt(0)) {
        addToGroup(group, rows.countSelected());
      }
      return;
    }

    // Fallback.
    DecodedVector decoded(*arg, rows);
    if (decoded.isConstantMapping()) {
      if (!decoded.isNullAt(0)) {
        addToGroup(group, rows.countSelected());
      }
    } else if (decoded.mayHaveNulls()) {
      int64_t nonNullCount = 0;
      rows.applyToSelected([&](vector_size_t i) {
        if (!decoded.isNullAt(i)) {
          ++nonNullCount;
        }
      });
      addToGroup(group, nonNullCount);
    } else {
      addToGroup(group, rows.countSelected());
    }
  }

  void addSingleGroupIntermediateResults(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /*mayPushdown*/) override {
    const auto& arg = args[0];
    LazyVector::ensureLoadedRows(arg, rows);
    const BaseVector* base = arg->loadedVector();

    // Fast path: FLAT BIGINT, no nulls.
    if (base->isFlatEncoding() && base->rawNulls() == nullptr) {
      const int64_t* rawValues =
          base->asUnchecked<FlatVector<int64_t>>()->rawValues();
      int64_t sum = 0;
      if (rows.isAllSelected()) {
        const auto end = rows.end();
        // Compiler auto-vectorizes this reduction (NEON/SVE/AVX).
        for (vector_size_t i = rows.begin(); i < end; ++i) {
          sum += rawValues[i];
        }
      } else {
        rows.applyToSelected(
            [&](vector_size_t i) { sum += rawValues[i]; });
      }
      addToGroup(group, sum);
      return;
    }

    // Fallback.
    decodedIntermediate_.decode(*arg, rows);
    int64_t count = 0;
    if (decodedIntermediate_.mayHaveNulls()) {
      rows.applyToSelected([&](vector_size_t i) {
        if (!decodedIntermediate_.isNullAt(i)) {
          count += decodedIntermediate_.valueAt<int64_t>(i);
        }
      });
    } else {
      rows.applyToSelected([&](vector_size_t i) {
        count += decodedIntermediate_.valueAt<int64_t>(i);
      });
    }
    addToGroup(group, count);
  }

 protected:
  void initializeNewGroupsInternal(
      char** groups,
      folly::Range<const vector_size_t*> indices) override {
    for (auto i : indices) {
      // Result of count is never null.
      *value<int64_t>(groups[i]) = static_cast<int64_t>(0);
    }
  }

 private:
  inline void addToGroup(char* group, int64_t count) {
    *value<int64_t>(group) += count;
  }

  inline void addOneToEachGroupBulk(
      char** groups,
      const SelectivityVector& rows) {
    if (rows.isAllSelected()) {
      const auto end = rows.end();
      for (vector_size_t i = rows.begin(); i < end; ++i) {
        ++*value<int64_t>(groups[i]);
      }
      return;
    }
    const uint64_t* selectedBits = rows.allBits();
    bits::forEachWord(
        rows.begin(),
        rows.end(),
        [&](int32_t wordIdx, uint64_t mask) {
          uint64_t word = selectedBits[wordIdx] & mask;
          while (word) {
            const auto bit = __builtin_ctzll(word);
            const vector_size_t row =
                static_cast<vector_size_t>(wordIdx) * 64 + bit;
            ++*value<int64_t>(groups[row]);
            word &= word - 1;
          }
        });
  }

  inline void addOneToEachNonNullGroupBulk(
      char** groups,
      const SelectivityVector& rows,
      const uint64_t* rawNulls) {
    if (rows.isAllSelected()) {
      bits::forEachWord(
          rows.begin(),
          rows.end(),
          [&](int32_t wordIdx, uint64_t mask) {
            uint64_t word = rawNulls[wordIdx] & mask;
            while (word) {
              const auto bit = __builtin_ctzll(word);
              const vector_size_t row =
                  static_cast<vector_size_t>(wordIdx) * 64 + bit;
              ++*value<int64_t>(groups[row]);
              word &= word - 1;
            }
          });
      return;
    }
    const uint64_t* selectedBits = rows.allBits();
    bits::forEachWord(
        rows.begin(),
        rows.end(),
        [&](int32_t wordIdx, uint64_t mask) {
          uint64_t word = (selectedBits[wordIdx] & rawNulls[wordIdx]) & mask;
          while (word) {
            const auto bit = __builtin_ctzll(word);
            const vector_size_t row =
                static_cast<vector_size_t>(wordIdx) * 64 + bit;
            ++*value<int64_t>(groups[row]);
            word &= word - 1;
          }
        });
  }

  inline int64_t countNonNulls(
      const SelectivityVector& rows,
      const uint64_t* rawNulls) {
    if (rows.isAllSelected()) {
      return bits::countBits(rawNulls, rows.begin(), rows.end());
    }
    const uint64_t* selectedBits = rows.allBits();
    int64_t cnt = 0;
    bits::forEachWord(
        rows.begin(),
        rows.end(),
        [&](int32_t wordIdx, uint64_t mask) {
          cnt += __builtin_popcountll(
              (selectedBits[wordIdx] & rawNulls[wordIdx]) & mask);
        });
    return cnt;
  }

  DecodedVector decodedIntermediate_;
};

} // namespace

exec::AggregateRegistrationResult registerCount(
    const std::string& name,
    bool withCompanionFunctions,
    bool overwrite) {
  std::vector<std::shared_ptr<exec::AggregateFunctionSignature>> signatures{
      exec::AggregateFunctionSignatureBuilder()
          .returnType("bigint")
          .intermediateType("bigint")
          .build(),
      exec::AggregateFunctionSignatureBuilder()
          .typeVariable("T")
          .returnType("bigint")
          .intermediateType("bigint")
          .argumentType("T")
          .build(),
  };

  return exec::registerAggregateFunction(
      name,
      std::move(signatures),
      [name](
          core::AggregationNode::Step /*step*/,
          const std::vector<TypePtr>& argTypes,
          const TypePtr& /*resultType*/,
          const core::QueryConfig& /*config*/)
          -> std::unique_ptr<exec::Aggregate> {
        VELOX_CHECK_LE(
            argTypes.size(), 1, "{} takes at most one argument", name);
        return std::make_unique<CountAggregate>();
      },
      {false /*orderSensitive*/},
      withCompanionFunctions,
      overwrite);
}

} // namespace facebook::velox::functions::aggregate::sparksql
