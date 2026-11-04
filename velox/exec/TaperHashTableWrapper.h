#pragma once

#include <folly/CPortability.h>
#include <folly/Likely.h>
#include <cstdint>
#include <cstring>
#include <variant>

#include "velox/common/base/Exceptions.h"
#include "velox/common/base/TemplateHelpers.h"
#include "velox/common/memory/MemoryPool.h"
#include "velox/common/base/RawVector.h"
#include "velox/exec/RowContainer.h"
#include "velox/exec/TaperHashTable.h"
#include "velox/exec/VectorHasher.h"
#include "velox/type/Type.h"
#include "velox/vector/DecodedVector.h"

namespace facebook::velox::exec {

class TaperFlatHashTableWrapper {
 public:
  using ScatteredTable =
      TaperFlatHashTable<uint64_t, sizeof(uint64_t) + sizeof(void*), true>;
  using FlatTable =
      TaperFlatHashTable<int64_t, sizeof(int64_t) + 1 + sizeof(double), false>;

  struct FlatRowContainer {
    std::reference_wrapper<TaperFlatHashTableWrapper> wrapper_;

    explicit FlatRowContainer(TaperFlatHashTableWrapper& wrapper)
        : wrapper_{wrapper} {}

    int64_t numRows() const {
      return wrapper_.get().size();
    }

    int32_t listRows(
        RowContainerIterator* FOLLY_NONNULL iter,
        int32_t maxRows,
        uint64_t maxBytes,
        char* FOLLY_NONNULL* FOLLY_NONNULL rows) const {
      return wrapper_.get().listFlatRows(iter, maxRows, maxBytes, rows);
    }

    int32_t listRows(
        RowContainerIterator* FOLLY_NONNULL iter,
        int32_t maxRows,
        char* FOLLY_NONNULL* FOLLY_NONNULL rows) const {
      return wrapper_.get().listFlatRows(
          iter, maxRows, RowContainer::kUnlimited, rows);
    }
  };

 private:
  enum DispatchFallbackAction { kDefault = 0, kReject };
  template <
      DispatchFallbackAction FallbackAction,
      typename... TargetTypes,
      typename FVisit,
      typename VariantType>
  static auto dispatch(FVisit&& visit, VariantType&& variant) {
    using FirstType = std::tuple_element_t<0, std::tuple<TargetTypes...>>;
    using Ret = std::invoke_result_t<FVisit, FirstType&>;
    return std::visit(
        velox::Overload{[&](auto& value) -> Ret {
          using ActualType = std::decay_t<decltype(value)>;
          if constexpr ((std::is_same_v<ActualType, TargetTypes> || ...)) {
            return visit(std::forward<decltype(value)>(value));
          } else if constexpr (FallbackAction == kReject) {
            VELOX_UNSUPPORTED("variant not match");
          } else if constexpr (FallbackAction == kDefault) {
            return Ret();
          }
        }},
        std::forward<VariantType>(variant));
  }

  template <
      DispatchFallbackAction FallbackAction,
      typename... TargetTypes,
      typename FVisit>
  auto dispatch(FVisit&& visit) const {
    return dispatch<FallbackAction, TargetTypes...>(
        std::forward<FVisit>(visit), tableVariant_);
  }

  template <
      DispatchFallbackAction FallbackAction,
      typename... TargetTypes,
      typename FVisit>
  auto dispatch(FVisit&& visit) {
    return dispatch<FallbackAction, TargetTypes...>(
        std::forward<FVisit>(visit), tableVariant_);
  }

 public:
  TaperFlatHashTableWrapper()
      : tableVariant_(std::monostate{}), flatRows_(*this) {}

  void initScattered(memory::MemoryPool& pool, uint32_t initCapacity) {
    VELOX_CHECK(!initialized(), "table is already initialized");
    keySize_ = ScatteredTable::kKeySize;
    valueSize_ = ScatteredTable::kValueSize;
    tableVariant_.emplace<ScatteredTable>(pool, initCapacity);
  }

  void initFlat(
      const TypePtr& keyType,
      int32_t rowSize,
      memory::MemoryPool& pool,
      uint32_t initCapacity) {
    VELOX_CHECK(!initialized(), "table is already initialized");
    if (!supportKeyTypeAndRowSize(keyType, rowSize)) {
      VELOX_UNSUPPORTED(
          "unsupported key type {} and row size {} for flat hash table",
          keyType->kindName(),
          rowSize);
    }
    keySize_ = FlatTable::kKeySize;
    valueSize_ = FlatTable::kValueSize;
    tableVariant_.emplace<FlatTable>(pool, initCapacity);
  }

  void reset() {
    keySize_ = 0;
    valueSize_ = 0;
    tableVariant_ = std::monostate{};
    hasNullGroup_ = false;
    nullGroup_.clear();
  }

  FOLLY_ALWAYS_INLINE bool initialized() const {
    return !std::holds_alternative<std::monostate>(tableVariant_);
  }

  FOLLY_ALWAYS_INLINE bool operator!() const {
    return !initialized();
  }

  void clear() {
    dispatch<kDefault, ScatteredTable, FlatTable>([&](auto& table) {
      table.clear();
      hasNullGroup_ = false;
      nullGroup_.clear();
    });
  }

  size_t allocatedBytes() const {
    return dispatch<kDefault, ScatteredTable, FlatTable>(
        [&](auto& table) { return table.allocatedBytes(); });
  }

  size_t capacity() const {
    return dispatch<kDefault, ScatteredTable, FlatTable>(
        [&](auto& table) { return table.capacity(); });
  }

  size_t size() const {
    return dispatch<kDefault, ScatteredTable, FlatTable>(
        [&](auto& table) { return table.size(); });
  }

  uint64_t hashTableSizeIncrease(int32_t numNewDistinct) const {
    return dispatch<kDefault, ScatteredTable, FlatTable>([&](auto& table) {
      return table.hashTableSizeIncrease(numNewDistinct);
    });
  }

  uint64_t estimateHashTableSize(uint64_t numDistinct) const {
    return dispatch<kDefault, ScatteredTable, FlatTable>(
        [&](auto& table) { return table.estimateHashTableSize(numDistinct); });
  }

  uint32_t expandThreshold() const {
    return dispatch<kDefault, ScatteredTable, FlatTable>(
        [&](auto& table) { return table.expandThreshold(); });
  }

  ScatteredTable* toScattered() {
    return std::get_if<ScatteredTable>(&tableVariant_);
  }

  FlatRowContainer& toFlatRows() {
    VELOX_CHECK(std::holds_alternative<FlatTable>(tableVariant_));
    flatRows_.wrapper_ = *this;
    return flatRows_;
  }

  template <DVecLayout Layout, typename FInit, typename FUpdate>
  void emplaceBatchFlat(
      VectorHasher& hasher,
      const RowColumn& column,
      uint32_t numRows,
      FInit&& fInit,
      FUpdate&& fUpdate) {
    dispatch<kReject, FlatTable>([&](FlatTable& table) {
      auto& vector = hasher.decodedVector();
      VELOX_CHECK_EQ(hasher.typeKind(), TypeKind::BIGINT);

      interceptEmplaceFlat(
          column,
          [&](uint32_t rowIdx) { return vector.isNullAt<Layout>(rowIdx); },
          std::forward<FInit>(fInit),
          std::forward<FUpdate>(fUpdate),
          [&](auto filter, auto init, auto update) {
            table.emplaceBatch<Layout, TypeKind::BIGINT>(
                vector, numRows, filter, init, update);
          });
    });
  }

  template <typename FInit, typename FUpdate>
  void emplaceBatchFlat(
      const RowColumn& column,
      char** groups,
      uint32_t numRows,
      FInit&& fInit,
      FUpdate&& fUpdate) {
    dispatch<kReject, FlatTable>([&](FlatTable& table) {
      auto nullByte = column.nullByte();
      auto nullMask = column.nullMask();

      interceptEmplaceFlat(
          column,
          [&](uint32_t rowIdx) { return groups[rowIdx][nullByte] & nullMask; },
          std::forward<FInit>(fInit),
          std::forward<FUpdate>(fUpdate),
          [&](auto filter, auto init, auto update) {
            table.emplaceBatch(groups, numRows, filter, init, update);
          });
    });
  }

  template <typename FInit, typename FUpdate>
  void emplaceFlat(
      FlatTable::KeyType key,
      bool isNull,
      const RowColumn& column,
      FInit&& fInit,
      FUpdate&& fUpdate) {
    dispatch<kReject, FlatTable>([&](FlatTable& table) {
      interceptEmplaceFlat(
          column,
          [&](uint32_t) { return isNull; },
          [&](uint32_t, char* data) { fInit(data); },
          [&](uint32_t, char* data, bool isInit) { fUpdate(data, isInit); },
          [&](auto filter, auto init, auto update) {
            if (filter(0)) {
              return;
            }
            table.emplace(
                key,
                [&](auto, TaperHashTableChunk& chunk, uint8_t slot) {
                  return key == table.chunkKey(chunk, slot);
                },
                [&](char* data) { init(0, data); },
                [&](char* data, bool isInit) { update(0, data, isInit); });
          });
    });
  }

  int32_t listFlatRows(
      RowContainerIterator* FOLLY_NONNULL iter,
      int32_t maxRows,
      uint64_t maxBytes,
      char* FOLLY_NONNULL* FOLLY_NONNULL rows) {
    return dispatch<kReject, FlatTable>([&](FlatTable& table) {
      int32_t numGroups = 0;
      auto visitor = [&] {
        if (iter->rowBegin) {
          return table.getResultVisitor(
              iter->rowBegin, static_cast<uint16_t>(iter->rowOffset));
        }
        return table.getResultVisitor();
      }();

      int64_t remainingBytes = maxBytes;
      if (hasNullGroup_) {
        rows[numGroups] = nullGroup_.data();
        hasNullGroup_ = false;
        remainingBytes -= visitor.rowSize();
        numGroups++;
      }

      while (numGroups < maxRows && remainingBytes > 0 && !visitor.finished()) {
        rows[numGroups] = visitor.curRow();
        visitor.next();
        remainingBytes -= visitor.rowSize();
        numGroups++;
      }
      visitor.savePos([&](auto chunk, auto tagPos) {
        iter->rowBegin = reinterpret_cast<char*>(chunk);
        iter->rowOffset = tagPos;
      });

      return numGroups;
    });
  }

  static bool supportKeyTypeAndRowSize(const TypePtr& type, size_t rowSize) {
    return type->kind() == TypeKind::BIGINT && rowSize == FlatTable::kRowSize;
  }

 private:
  // intercept emplace callbacks to:
  // 1. maintain an external row for null keys
  // 2. transform value pointer to row pointer for outer callbacks
  template <
      typename FIsNull,
      typename FInit,
      typename FUpdate,
      typename FProcess>
  void interceptEmplaceFlat(
      const RowColumn& column,
      FIsNull&& fIsNull,
      FInit&& fInit,
      FUpdate&& fUpdate,
      FProcess&& fProcess) {
    auto nullByte = column.nullByte();
    auto nullMask = column.nullMask();
    fProcess(
        [&](uint32_t rowIdx) INLINE_LAMBDA {
          if (fIsNull(rowIdx)) {
            bool initFlag = false;
            if (!hasNullGroup_) {
              nullGroup_.resize(FlatTable::kRowSize);
              hasNullGroup_ = true;
              fInit(rowIdx, nullGroup_.data());
              nullGroup_[nullByte] |= nullMask;
              initFlag = true;
            }
            fUpdate(rowIdx, nullGroup_.data(), initFlag);
            return true;
          }
          return false;
        },
        [&](uint32_t rowIdx, char* value) {
          fInit(rowIdx, value - FlatTable::kKeySize);
        },
        [&](uint32_t rowIdx, char* value, bool isNew) {
          fUpdate(rowIdx, value - FlatTable::kKeySize, isNew);
        });
  }

  int32_t keySize_{0};
  int32_t valueSize_{0};
  bool hasNullGroup_{false};
  raw_vector<char> nullGroup_;
  std::variant<std::monostate, ScatteredTable, FlatTable> tableVariant_;
  FlatRowContainer flatRows_;
};

}; // namespace facebook::velox::exec
