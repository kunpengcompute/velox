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
#include <folly/CPortability.h>
#include <folly/Likely.h>
#include <cstdint>
#include <cstring>

#include "velox/common/base/BitUtil.h"
#include "velox/common/base/Exceptions.h"
#include "velox/common/base/SimdUtil.h"
#include "velox/common/memory/Allocation.h"
#include "velox/common/memory/AllocationPool.h"
#include "velox/common/memory/MemoryPool.h"
#include "velox/exec/DebugStat.h"
#include "velox/exec/TaperHashTable.h"
#include "velox/exec/VectorHasher.h"
#include "velox/type/StringView.h"
#include "velox/type/Type.h"
#include "velox/vector/DecodedVector.h"
#include "velox/vector/FlatVector.h"

namespace facebook::velox::exec::taper {

using Hashers = std::vector<std::unique_ptr<VectorHasher>>;
using Groups = std::vector<char*>;

template <typename Key>
struct MethodBase {
  using KeyType = Key;
  using FlatVisitor = typename TaperFlatHashTable<Key>::Visitor;

  MethodBase(memory::MemoryPool& pool, Hashers& hashers)
      : pool_(pool), hashers_(hashers) {}

  int32_t numKeys() {
    return numKeys_;
  }

  virtual const KeyType* getKeys() = 0;

  virtual void clear() {}

 protected:
  memory::MemoryPool& pool_;
  memory::ContiguousAllocation keysAllocation_;
  bool inited_ = false;
  Hashers& hashers_;
  Groups workingGroups_;
  int32_t numKeys_;

  template <TypeKind Kind>
  static bool extractOneKey(
      VectorPtr& vector,
      Groups& groups,
      uint8_t nullByte,
      uint8_t offset,
      uint8_t nullMask) {
    auto* flatResult =
        vector->as<FlatVector<typename TypeTraits<Kind>::NativeType>>();
    for (size_t i = 0; i < groups.size(); ++i) {
      if ((groups[i][nullByte] & nullMask) != 0) {
        vector->setNull(i, true);
      } else {
        flatResult->set(
            i,
            *reinterpret_cast<typename TypeTraits<Kind>::NativeType*>(
                groups[i] + offset));
      }
    }
    return true;
  }
};

template <typename Num>
struct MethodOneNumber : public MethodBase<Num> {
  using Base = MethodBase<Num>;
  using KeyType = typename Base::KeyType;
  using FlatVisitor = typename Base::FlatVisitor;

  MethodOneNumber(memory::MemoryPool& pool, Hashers& hashers)
      : Base(pool, hashers) {}

  static KeyAccessor<KeyType> accessor(bool seedFlag) {
    return KeyAccessor<KeyType>{seedFlag};
  }

  const KeyType* getKeys() override {
    return keys_;
  }

  const KeyType& keyAt(size_t idx) {
    return keys_[idx];
  }

  void clear() override {
    nullValue_.clear();
    shouldExtractNull_ = false;
    Base::pool_.freeContiguous(Base::keysAllocation_);
  }

  template <typename TaperTable, typename FInit, typename FUpdate>
  void emplaceBatch(TaperTable& tbl, FInit&& fInit, FUpdate&& fUpdate) {
    const auto& firstKeyVector = Base::hashers_[0]->decodedVector();
    tbl.emplaceBatch(
        keys_,
        Base::numKeys_,
        [&](uint32_t rowIdx) {
          if (firstKeyVector.isNullAt(rowIdx)) {
            bool initFlag = false;
            if (nullValue_.empty()) {
              shouldExtractNull_ = true;
              nullValue_.resize(tbl.rowSize());
              fInit(rowIdx, nullValue_.data());
              initFlag = true;
            }
            fUpdate(rowIdx, nullValue_.data(), initFlag);
            return true;
          }
          return false;
        },
        fInit,
        fUpdate);
  }

  void initSerializedKeys(int32_t numElems) {
    Base::numKeys_ = numElems;
    auto& keyVector = Base::hashers_[0]->decodedVector();
    if (keyVector.isIdentityMapping()) {
      keys_ = keyVector.template data<KeyType>();
    } else if (!keyVector.isConstantMapping()) {
      const auto numPages =
          memory::AllocationTraits::numPages(sizeof(KeyType) * numElems);
      Base::pool_.allocateContiguous(numPages, Base::keysAllocation_);
      auto* dstKeys = Base::keysAllocation_.template data<KeyType>();
      const auto* indices = keyVector.indices();
      auto* srcKeys = keyVector.template data<KeyType>();
      for (int32_t i = 0; i < numElems; i++) {
        dstKeys[i] = srcKeys[indices[i]];
      }
      keys_ = dstKeys;
    } else {
      VELOX_UNREACHABLE("Const mapping should not processed here");
    }
  }

  void extractKeys(RowVectorPtr& result, Groups& groups) {
    VELOX_CHECK(false, "not implemented");
  }

  template <bool IsMixed = false>
  Groups& extractFlatTable(
      FlatVisitor& tblVisitor,
      int32_t maxOutputRows,
      int32_t maxOutputBytes,
      RowVectorPtr& result) {
    auto* keyVector = result->childAt(0)->as<FlatVector<KeyType>>();
    keyVector->resize(maxOutputRows);
    BufferPtr valuesBuffer = keyVector->mutableValues(maxOutputRows);
    auto* keyData = valuesBuffer->asMutable<KeyType>();
    Base::workingGroups_.resize(maxOutputRows);

    int32_t idx = 0;
    if (shouldExtractNull_) {
      keyVector->setNull(0, true);
      if constexpr (IsMixed) {
        Base::workingGroups_[0] = *reinterpret_cast<char**>(nullValue_.data());
      } else {
        Base::workingGroups_[0] = nullValue_.data();
      }
      shouldExtractNull_ = false;
      idx++;
    }
    while (idx < maxOutputRows && maxOutputBytes > 0 &&
           !tblVisitor.finished()) {
      keyData[idx] = tblVisitor.curKey();
      if constexpr (IsMixed) {
        Base::workingGroups_[idx] = *reinterpret_cast<char**>(
            const_cast<char*>(tblVisitor.curVal().buf));
      } else {
        Base::workingGroups_[idx] = const_cast<char*>(tblVisitor.curVal().buf);
      }
      maxOutputBytes -= tblVisitor.rowSize();
      tblVisitor.next();
      idx++;
    }

    keyVector->resize(idx);
    Base::workingGroups_.resize(idx);
    return Base::workingGroups_;
  }

  Groups& extractNodeTable(
      RowContainer& rows,
      int32_t maxOutputRows,
      int32_t maxOutputBytes,
      RowContainerIterator& iterator,
      RowVectorPtr& result) {
    VELOX_FAIL("not implemented");
  }

  uint32_t keySize() const {
    return sizeof(Num);
  }

 private:
  const KeyType* keys_;
  std::string nullValue_;
  bool shouldExtractNull_ = false;
};

struct MethodSerializedFixed : public MethodBase<taper::FixedBuf> {
  using Base = MethodBase<taper::FixedBuf>;

  MethodSerializedFixed(memory::MemoryPool& pool, Hashers& hashers)
      : Base(pool, hashers) {
    updateKeySize();
  }

  template <typename TaperTable, typename FInit, typename FUpdate>
  void emplaceBatch(TaperTable& tbl, FInit&& fInit, FUpdate&& fUpdate) {
    tbl.emplaceBatch(
        getKeys(),
        Base::numKeys_,
        [&](uint32_t) { return false; },
        std::forward<FInit>(fInit),
        std::forward<FUpdate>(fUpdate));
  }

  void clear() override {
    Base::pool_.freeContiguous(Base::keysAllocation_);
  }

  void initSerializedKeys(int32_t numElems) {
    auto bytesNum = static_cast<uint64_t>(keyBytes_) * numElems;
    if (inited_ && numElems <= Base::numKeys_) {
      auto* dstKeys = keysAllocation_.template data<KeyType>();
      memset(dstKeys, 0x00, bytesNum);
    } else {
      const auto numPages = memory::AllocationTraits::numPages(bytesNum);
      pool_.allocateContiguous(numPages, keysAllocation_);
      auto* dstKeys = keysAllocation_.template data<KeyType>();
      memset(dstKeys, 0x00, bytesNum);
      inited_ = true;
    }
    Base::numKeys_ = numElems;

    uint32_t preKeySize = 0;
    for (uint8_t j = 0; j < hashers_.size(); ++j) {
      auto type = hashers_[j]->type();
      auto& vector = hashers_[j]->decodedVector();
      auto layout = DecodedVector::getLayout(vector);
      auto offset = preKeySize;
      preKeySize += hashers_[j]->type()->cppSizeInBytes();
      uint8_t nullMask = j + 1;
#define DISPATCH(LAYOUT)                     \
  if (layout == (LAYOUT)) {                  \
    VELOX_DYNAMIC_NUMERIC_TYPE_DISPATCH_ALL( \
        writeColValue,                       \
        LAYOUT,                              \
        type->kind(),                        \
        vector,                              \
        numElems,                            \
        nullMask,                            \
        offset);                             \
    continue;                                \
  }
      FOREACH_VALID_LAYOUT(DISPATCH)
#undef DISPATCH
      VELOX_DYNAMIC_NUMERIC_TYPE_DISPATCH_ALL(
          writeColValue,
          DVecLayout::kUnknown,
          type->kind(),
          vector,
          numElems,
          nullMask,
          offset);
    }
  }

  template <bool IsMixed = false>
  Groups& extractFlatTable(
      FlatVisitor& tblVisitor,
      int32_t maxOutputRows,
      int32_t maxOutputBytes,
      RowVectorPtr& result) {
    workingGroups_.resize(maxOutputRows);
    workingKeysGroups_.resize(maxOutputRows);
    int32_t idx = 0;
    while (idx < maxOutputRows && maxOutputBytes > 0 &&
           !tblVisitor.finished()) {
      if constexpr (IsMixed) {
        workingGroups_[idx] = *reinterpret_cast<char**>(
            const_cast<char*>(tblVisitor.curVal().buf));
      } else {
        workingGroups_[idx] = const_cast<char*>(tblVisitor.curVal().buf);
      }
      workingKeysGroups_[idx] = const_cast<char*>(tblVisitor.curKey().buf);
      maxOutputBytes -= tblVisitor.rowSize();
      tblVisitor.next();
      idx++;
    }

    workingGroups_.resize(idx);
    workingKeysGroups_.resize(idx);
    extractKeys(result, workingKeysGroups_);
    return workingGroups_;
  }

  Groups& extractNodeTable(
      RowContainer& rows,
      int32_t maxOutputRows,
      int32_t maxOutputBytes,
      RowContainerIterator& iterator,
      RowVectorPtr& result) {
    workingGroups_.resize(maxOutputRows);
    char** groups = workingGroups_.data();
    const int32_t numGroups =
        rows.listRows(&iterator, maxOutputRows, maxOutputBytes, groups);

    workingGroups_.resize(numGroups);
    extractKeys(result, workingGroups_);
    return workingGroups_;
  }

  KeyAccessor<KeyType> accessor(bool seedFlag) const {
    return KeyAccessor<KeyType>{keyBytes_, seedFlag};
  }

  const KeyType* getKeys() override {
    auto* keys = keysAllocation_.data<uint8_t>();
    return reinterpret_cast<KeyType*>(keys);
  }

  KeyType& keyAt(size_t idx) {
    auto* keys = keysAllocation_.data<uint8_t>();
    return *reinterpret_cast<KeyType*>(keys + idx * keyBytes_);
  }

  void saveKey(size_t idx, char* row) {
    memcpy(row, &keyAt(idx), keyBytes_);
  }

  uint32_t keySize() const {
    return keyBytes_;
  }

 private:
  template <DVecLayout Layout, TypeKind Kind>
  bool writeColValue(
      DecodedVector& vector,
      uint32_t numElems,
      uint8_t nullMask,
      uint32_t offset) {
    for (int32_t i = 0; i < numElems; i++) {
      auto* keys = keysAllocation_.data<uint8_t>() + i * keyBytes_;
      if (vector.template isNullAt<Layout>(i)) {
        keys[nullByte_] |= nullMask;
      } else {
        *reinterpret_cast<typename TypeTraits<Kind>::NativeType*>(
            keys + offset) =
            vector.valueAt<typename TypeTraits<Kind>::NativeType, Layout>(i);
      }
    }
    return true;
  }

  void updateKeySize() {
    keyBytes_ = 1; // has null 涓? ,娌℃湁null鍊间负0
    for (const auto& hasher : hashers_) {
      keyBytes_ += hasher->type()->cppSizeInBytes();
    }
    nullByte_ = keyBytes_ - 1;
  }

  void extractKeys(RowVectorPtr& result, Groups& groups) {
    uint8_t nullByte = keySize() - 1;
    uint8_t preKeySize = 0;
    for (int i = 0; i < hashers_.size(); ++i) {
      auto type = hashers_[i]->type();
      auto& keyVector = result->childAt(i);
      keyVector->resize(groups.size());
      uint8_t offset = preKeySize;
      preKeySize = hashers_[i]->type()->cppSizeInBytes();
      uint8_t nullMask = i + 1;
      VELOX_DYNAMIC_NUMERIC_TYPE_DISPATCH(
          extractOneKey,
          type->kind(),
          keyVector,
          groups,
          nullByte,
          offset,
          nullMask);
    }
  }

  Groups workingKeysGroups_;
  uint32_t keyBytes_;
  uint8_t nullByte_;
};

struct MethodSerializedCompactKeys : public MethodBase<taper::CompactKeys> {
  using Base = MethodBase<taper::CompactKeys>;
  using Size = taper::CompactKeys::Size;
  using Byte = uint8_t;
  using Pointer = taper::CompactKeys::Pointer;
  using KeyGroups = std::vector<CompactKeys*>;

  static constexpr Size kStrKeySize = 16;
  static constexpr Size kAlignment = 4;

  MethodSerializedCompactKeys(memory::MemoryPool& pool, Hashers& hashers)
      : Base(pool, hashers), allocationPool_(&pool) {
    LOG(WARNING) << "init MethodSerializedCompactKeys";

    for (size_t i = 0; i < hashers_.size(); ++i) {
      const auto& type = hashers_[i]->type();

      VELOX_CHECK(
          type->isPrimitiveType(),
          "Kind {} is not supported in CompactKeys",
          type->kindName());

      if (hashers_[i]->type()->isFixedWidth()) {
        numericHashers_.emplace_back(std::pair{i, hashers_[i].get()});
      } else {
        stringHashers_.emplace_back(std::pair{i, hashers_[i].get()});
      }
    }

    initKeyMetrics();
  }

  void saveKey(size_t idx, char* row) {
    simd::memcpy(row, &keyAt(idx), keySizeAligned_);
  }

#ifdef DEBUG_STAT_ENABLED
  virtual ~MethodSerializedCompactKeys() {
    debugStat_.print();
  };
#endif

  template <typename TaperTable, typename FInit, typename FUpdate>
  void emplaceBatch(TaperTable& tbl, FInit&& fInit, FUpdate&& fUpdate) {
    tbl.emplaceBatch(
        getKeys(),
        Base::numKeys_,
        [&](uint32_t) { return false; },
        std::forward<FInit>(fInit),
        std::forward<FUpdate>(fUpdate));
  }

  void initSerializedKeys(int32_t numElems) {
    DEBUG_STAT_TLS_EXPOSE_SCOPE(&debugStat_)
    DEBUG_STAT_TLS_RECORD_SCOPE(debug::DebugStat::InitKeysCalls)
    DEBUG_STAT_TLS_COUNT(debug::DebugStat::InitKeysCounts, numElems)
    initPrefixArea(numElems);

    auto keyOffset = keyOffset_;
    for (const auto& [colIdx, hasher] : numericHashers_) {
      auto type = hasher->type();
      auto nullByte = nullBytes_[colIdx];
      auto nullMask = nullMasks_[colIdx];
      initializeNumericKey(*hasher, numElems, keyOffset, nullByte, nullMask);
      keyOffset += type->cppSizeInBytes();
    }
    if (!stringHashers_.empty()) {
      initializeStringKeys(numElems);
    }
  }

  template <bool IsMixed = false>
  Groups& extractFlatTable(
      FlatVisitor& tblVisitor,
      int32_t maxOutputRows,
      int32_t maxOutputBytes,
      RowVectorPtr& result) {
    DEBUG_STAT_TLS_EXPOSE_SCOPE(&debugStat_)
    workingGroups_.resize(maxOutputRows);
    workingKeysGroups_.resize(maxOutputRows);
    int32_t idx = 0;
    while (idx < maxOutputRows && maxOutputBytes > 0 &&
           !tblVisitor.finished()) {
      if constexpr (IsMixed) {
        workingGroups_[idx] = *reinterpret_cast<char**>(
            const_cast<char*>(tblVisitor.curVal().buf));
      } else {
        workingGroups_[idx] = const_cast<char*>(tblVisitor.curVal().buf);
      }
      workingKeysGroups_[idx] = const_cast<char*>(tblVisitor.curKey().data());
      maxOutputBytes -= tblVisitor.rowSize();
      tblVisitor.next();
      idx++;
    }

    workingGroups_.resize(idx);
    workingKeysGroups_.resize(idx);
    extractKeys(result, workingKeysGroups_);
    return workingGroups_;
  }

  Groups& extractNodeTable(
      RowContainer& rows,
      int32_t maxOutputRows,
      int32_t maxOutputBytes,
      RowContainerIterator& iterator,
      RowVectorPtr& result) {
    DEBUG_STAT_TLS_EXPOSE_SCOPE(&debugStat_)
    workingGroups_.resize(maxOutputRows);
    char** groups = workingGroups_.data();
    const int32_t numGroups =
        rows.listRows(&iterator, maxOutputRows, maxOutputBytes, groups);

    workingGroups_.resize(numGroups);
    extractKeys(result, workingGroups_);
    return workingGroups_;
  }

  KeyAccessor<KeyType> accessor(bool seedFlag) const {
    return {keySize_, sizeOffset_, pointerOffset_, seedFlag};
  }

  const KeyType* getKeys() override {
    auto* keys = keysAllocation_.data<uint8_t>();
    return reinterpret_cast<KeyType*>(keys);
  }

  KeyType& keyAt(size_t idx) {
    auto* keys = keysAllocation_.data<uint8_t>();
    return *reinterpret_cast<KeyType*>(keys + idx * keySizeAligned_);
  }

  uint8_t keySize() const {
    return keySizeAligned_;
  }

  void clear() override {
    Base::pool_.freeContiguous(Base::keysAllocation_);
    allocationPool_.clear();
    numRows_ = 0;
    numExtend_ = 0;
  }

 private:
  FOLLY_ALWAYS_INLINE
  void initializeNumericKey(
      VectorHasher& hasher,
      int32_t numElems,
      Size keyOffset,
      Size nullByte,
      uint8_t nullMask) {
    auto type = hasher.type();
    auto& vector = hasher.decodedVector();
    auto layout = DecodedVector::getLayout(vector);

#define DISPATCH_CALL(LAYOUT)              \
  VELOX_DYNAMIC_NUMERIC_TYPE_DISPATCH_ALL( \
      writeNumericCol,                     \
      LAYOUT,                              \
      type->kind(),                        \
      vector,                              \
      numElems,                            \
      keyOffset,                           \
      nullByte,                            \
      nullMask)
#define DISPATCH(LAYOUT)    \
  if (layout == (LAYOUT)) { \
    DISPATCH_CALL(LAYOUT);  \
    return;                 \
  }
    FOREACH_VALID_LAYOUT(DISPATCH)
    DISPATCH_CALL(DVecLayout::kUnknown);
#undef DISPATCH
#undef DISPATCH_CALL
  }

  template <DVecLayout Layout, TypeKind Kind>
  void writeNumericCol(
      DecodedVector& vector,
      uint32_t numElems,
      Size keyOffset,
      Size nullByte,
      uint8_t nullMask) {
    using T = typename TypeTraits<Kind>::NativeType;
    for (Size rowIdx = 0; rowIdx < numElems; rowIdx++) {
      auto& keys = keyAt(rowIdx);
      if (vector.template isNullAt<Layout>(rowIdx)) {
        keys.setNullAt(nullByte, nullMask);
      } else {
        keys.valueAt<T>(keyOffset) = vector.valueAt<T, Layout>(rowIdx);
      }
    }
  }

  void initializeStringKeys(uint32_t numElems) {
    // temporary strings on stack memory
    MemoryView<StringView> strings{static_cast<StringView*>(
        alloca(sizeof(StringView) * stringHashers_.size()))};

    numRows_ += numElems;
    for (Size rowIdx = 0; rowIdx < numElems; rowIdx++) {
      auto& keys = keyAt(rowIdx);

      // end of each string (in reversed order)
      auto* ends = &keys.valueAt<Size>(stringOffset_);
      auto* end = keys.data() + keySize_;

      // position relative to key end (negative = in key, positive = extend)
      auto cursor = stringOffset_ - keySize_;

      // collect non-null strings
      strings.clear();
      for (const auto& [colIdx, hasher] : stringHashers_) {
        auto& vector = hasher->decodedVector();
        if (vector.isNullAt(rowIdx)) {
          keys.setNullAt(nullBytes_[colIdx], nullMasks_[colIdx]);
          strings.push_back(StringView{});
        } else {
          strings.push_back(vector.valueAt<StringView>(rowIdx));
        }
      }

      // calculate cursor position after all strings written.
      auto lastCursor = [&]() {
        auto temp = cursor;
        for (auto& value : strings) {
          // if string cannot be written in prefix area, skip to extend area
          Size size = value.size();
          if (size > 0) {
            if (temp < 0 && temp + size > 0) {
              temp = 0;
            }
            temp += size;
          }
        }
        return temp;
      };

      // should use extend memory
      if (lastCursor() > 0) {
        // take pointer space, recalculate cursor position later.
        end -= sizeof(Pointer);
        cursor += sizeof(Pointer);
        keys.valueAt<Pointer>(pointerOffset_) =
            allocationPool_.allocateFixed(lastCursor(), kAlignment);
        numExtend_++;
      }

      for (auto& value : strings) {
        Size size = value.size();
        const auto* data = value.data();

        if (size > 0) {
          if (cursor + size <= 0) {
            // all inline
            simd::memcpy(end + cursor, data, size);
            cursor += size;
          } else {
            // all extend
            if (cursor < 0) {
              cursor = 0;
            }
            simd::memcpy(
                keys.valueAt<Pointer>(pointerOffset_) + cursor, data, size);
            cursor += size;
          }
        }

        // record string end
        *(--ends) = cursor;
      }
    }
  }

  void extractKeys(RowVectorPtr& result, Groups& groups) {
    DEBUG_STAT_TLS_COUNT_INCR(debug::DebugStat::ExtractKeysCalls)
    DEBUG_STAT_TLS_COUNT(debug::DebugStat::ExtractKeysCounts, groups.size())
    auto keyOffset = keyOffset_;

    for (const auto& [colIdx, hasher] : numericHashers_) {
      auto type = hasher->type();
      auto& keyVector = result->childAt(colIdx);
      keyVector->resize(groups.size());
      auto nullByte = nullBytes_[colIdx];
      auto nullMask = nullMasks_[colIdx];
      VELOX_DYNAMIC_SCALAR_TYPE_DISPATCH(
          extractNumericKey,
          type->kind(),
          keyVector,
          groups,
          keyOffset,
          nullByte,
          nullMask);
      keyOffset += type->cppSizeInBytes();
    }

    if (!stringHashers_.empty()) {
      if (numExtend_ * 10 >= numRows_) {
        extractStringKeys<true>(result, groups);
      } else {
        extractStringKeys<false>(result, groups);
      }
    }
  }

  template <TypeKind Kind>
  void extractNumericKey(
      VectorPtr& vector,
      Groups& groups,
      Size keyOffset,
      Size nullByte,
      uint8_t nullMask) {
    using T = typename TypeTraits<Kind>::NativeType;

    auto* result = vector->as<FlatVector<T>>();
    for (size_t rowIdx = 0; rowIdx < groups.size(); ++rowIdx) {
#ifdef ENABLE_BOLT_NUMERIC_PREFETCH
      if (rowIdx + kPrefetchDist < groups.size()) {
        __builtin_prefetch(groups[rowIdx + kPrefetchDist]);
      }
#endif
      auto* keys = reinterpret_cast<CompactKeys*>(groups[rowIdx]);
      if (keys->isNullAt(nullByte, nullMask)) {
        vector->setNull(rowIdx, true);
      } else {
        result->set(rowIdx, keys->valueAt<T>(keyOffset));
      }
    }
  }

  static constexpr Size kPrefetchDist = 32;
  enum StringPrefetchBase {
    kPrefetchByString = 0,
    kPrefetchByNull = 1,
    kPrefetchByBoth = 2,
  };

  template <bool PrefetchExtend>
  void extractStringKeys(RowVectorPtr& result, Groups& groups) {
    if (stringOffset_ > 64) {
      extractStringKeysInner<PrefetchExtend, kPrefetchByBoth>(result, groups);
    } else {
      extractStringKeysInner<PrefetchExtend, kPrefetchByNull>(result, groups);
    }
  }

  template <bool PrefetchExtend, StringPrefetchBase PBase>
  void extractStringKeysInner(RowVectorPtr& result, Groups& groups) {
    DEBUG_STAT_TLS_RECORD_SCOPE(debug::DebugStat::ExtractStringKeysCalls)

    // temporary pointers on stack memory
    MemoryView<FlatVector<StringView>*> vectors{
        static_cast<FlatVector<StringView>**>(
            alloca(sizeof(FlatVector<StringView>*) * hashers_.size()))};

    for (const auto& [colIdx, hasher] : stringHashers_) {
      auto* vector =
          result->childAt(colIdx)->template as<FlatVector<StringView>>();
      vector->resize(groups.size());
      vectors[colIdx] = vector;
    }

    for (Size rowIdx = 0; rowIdx < groups.size(); rowIdx++) {
      if constexpr (PrefetchExtend) {
        if (rowIdx + kPrefetchDist * 2 < groups.size()) {
          if constexpr (PBase == kPrefetchByNull || PBase == kPrefetchByBoth) {
            __builtin_prefetch(groups[rowIdx + kPrefetchDist * 2]);
          }
          if constexpr (
              PBase == kPrefetchByString || PBase == kPrefetchByBoth) {
            __builtin_prefetch(
                groups[rowIdx + kPrefetchDist * 2] + stringOffset_);
          }
        }
        if (rowIdx + kPrefetchDist < groups.size() &&
            reinterpret_cast<CompactKeys*>(groups[rowIdx + kPrefetchDist])
                    ->valueAt<Size>(sizeOffset_) > 0) {
          __builtin_prefetch(
              reinterpret_cast<CompactKeys*>(groups[rowIdx + kPrefetchDist])
                  ->valueAt<Pointer>(pointerOffset_));
        }
      } else {
        if (rowIdx + kPrefetchDist < groups.size()) {
          if constexpr (PBase == kPrefetchByNull || PBase == kPrefetchByBoth) {
            __builtin_prefetch(groups[rowIdx + kPrefetchDist]);
          }
          if constexpr (
              PBase == kPrefetchByString || PBase == kPrefetchByBoth) {
            __builtin_prefetch(groups[rowIdx + kPrefetchDist] + stringOffset_);
          }
        }
      }

      auto* keys = reinterpret_cast<CompactKeys*>(groups[rowIdx]);
      auto* ends = &keys->valueAt<Size>(stringOffset_);
      auto* end = keys->data() + keySize_;
      auto cursor = stringOffset_ - keySize_;
      if (keys->valueAt<Size>(sizeOffset_) > 0) {
        end -= sizeof(Pointer);
        cursor += sizeof(Pointer);
      }

      for (const auto& [colIdx, _] : stringHashers_) {
        auto* vector = vectors[colIdx];
        if (keys->isNullAt(nullBytes_[colIdx], nullMasks_[colIdx])) {
          vector->setNull(rowIdx, true);
          --ends;
        } else {
          Size next = *(--ends);
          if (next <= 0) {
            // all inline
            extractStringValue(vector, rowIdx, end + cursor, next - cursor);
          } else {
            // all extend
            if (cursor < 0) {
              cursor = 0;
            }
            extractStringValue(
                vector,
                rowIdx,
                keys->valueAt<Pointer>(pointerOffset_) + cursor,
                next - cursor);
          }
          cursor = next;
        }
      }
    }
  }

  static FOLLY_ALWAYS_INLINE void extractStringValue(
      FlatVector<StringView>* result,
      Size rowIdx,
      const char* data,
      Size size) {
    StringView& dest = result->mutableRawValues<StringView>()[rowIdx];
    if (StringView::isInline(size)) {
      dest = StringView(data, size);
    } else {
      Buffer* buffer = result->getBufferWithSpace(size, false);
      char* ptr = buffer->asMutable<char>() + buffer->size();
      buffer->setSize(buffer->size() + size);
      simd::memcpy(ptr, data, size);
      dest = StringView(ptr, size);
    }
  }

  void initPrefixArea(int32_t numElems) {
    auto bytesNum = keySizeAligned_ * numElems;
    if (inited_ && numElems <= Base::numKeys_) {
      auto* prefixArea = keysAllocation_.template data<KeyType>();
      memset(prefixArea, 0, bytesNum);
    } else {
      const auto numPages = memory::AllocationTraits::numPages(bytesNum);
      pool_.allocateContiguous(numPages, keysAllocation_);
      auto* prefixArea = keysAllocation_.template data<KeyType>();
      memset(prefixArea, 0, bytesNum);
      inited_ = true;
    }
    Base::numKeys_ = numElems;
  }

  void initKeyMetrics() {
    keySize_ = 0;

    // 1. null bytes
    Size nullOffset = keySize_;
    keySize_ += bits::nbytes(hashers_.size());
    nullBytes_.resize(hashers_.size());
    nullMasks_.resize(hashers_.size());

    // 2. fixed keys
    keyOffset_ = keySize_;
    Size nullIdx = 0;
    int32_t keyIdx = 0;
    for (const auto& [idx, hasher] : numericHashers_) {
      auto type = hasher->type();
      keySize_ += type->cppSizeInBytes();
      nullBytes_[idx] = nullOffset + (nullIdx >> 3);
      nullMasks_[idx] = 1 << (nullIdx & 7);
      ++nullIdx;
    }

    // 3. string keys
    if (stringHashers_.empty()) {
      sizeOffset_ = 0;
      stringOffset_ = 0;
      pointerOffset_ = 0;
    } else {
      sizeOffset_ = keySize_;
      stringOffset_ = keySize_ + sizeof(Size) * stringHashers_.size();
      for (const auto& [idx, hasher] : stringHashers_) {
        auto type = hasher->type();
        keySize_ += kStrKeySize;
        nullBytes_[idx] = nullOffset + (nullIdx >> 3);
        nullMasks_[idx] = 1 << (nullIdx & 7);
        ++nullIdx;
      }
      pointerOffset_ = keySize_ - sizeof(Pointer);
    }

    keySizeAligned_ = bits::roundUp(keySize_, kAlignment);
  }

  Size keySize_;
  Size keySizeAligned_;
  Size keyOffset_;
  Size sizeOffset_;
  Size stringOffset_;
  Size pointerOffset_;
  size_t numRows_{0};
  size_t numExtend_{0};
  std::vector<Size> nullBytes_;
  std::vector<Byte> nullMasks_;
  std::vector<std::pair<Size, VectorHasher*>> numericHashers_;
  std::vector<std::pair<Size, VectorHasher*>> stringHashers_;
  memory::AllocationPool allocationPool_;
  Groups workingKeysGroups_;
  DEBUG_STAT_DEFINE_MEMBER(debugStat_, TaperMethod)
};

} // namespace facebook::velox::exec::taper
