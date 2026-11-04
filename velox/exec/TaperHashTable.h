#pragma once

#include <folly/CPortability.h>
#include <folly/Likely.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <type_traits>

#include "velox/common/base/BitUtil.h"
#include "velox/common/base/Exceptions.h"
#include "velox/common/memory/Allocation.h"
#include "velox/common/memory/MemoryPool.h"
#include "velox/common/base/RawVector.h"
#include "velox/exec/DebugStat.h"
#include "velox/exec/VectorHasher.h"
#include "velox/type/Type.h"
#include "velox/vector/DecodedVector.h"
#include "velox/vector/TypeAliases.h"

// #define TAPER_HASH_STAT
// #define HWP_PREFETCH
// #define DISABLE_PREFETCH
#define DUMMY_CMP [](auto, auto, auto) { return false; }

namespace facebook::velox::exec {

// `MemoryView` providing a simple container view on given memory space.
// `MemoryView` won't do memory reset and capacity check, please use with
// caution.
template <class T>
struct MemoryView {
 public:
  // NOTE: memory is not reset here
  template <class P>
  explicit MemoryView(P* base) : base_(reinterpret_cast<T*>(base)) {}

  FOLLY_ALWAYS_INLINE size_t size() const {
    return size_;
  }

  // NOTE: memory is not reset here
  FOLLY_ALWAYS_INLINE void clear() {
    size_ = 0;
  }

  FOLLY_ALWAYS_INLINE bool empty() const {
    return size_ == 0;
  }

  // NOTE: no capacity check here
  FOLLY_ALWAYS_INLINE void push_back(const T& value) {
    base_[size_++] = value;
  }

  FOLLY_ALWAYS_INLINE T& operator[](size_t i) {
    return base_[i];
  }

  FOLLY_ALWAYS_INLINE T* begin() {
    return base_;
  }

  FOLLY_ALWAYS_INLINE T* end() {
    return base_ + size_;
  }

 private:
  T* base_;
  size_t size_{0};
};

static constexpr size_t kHashMapPrefetchDist = 16;

namespace taper {

struct FixedBuf {
  char buf[0];
};

struct CompactKeys {
  using Size = int32_t;
  using Pointer = char*;

  char buf[0];

  template <typename T>
  FOLLY_ALWAYS_INLINE T& valueAt(Size offset) const {
    return *reinterpret_cast<T*>(const_cast<Pointer>(buf) + offset);
  }

  FOLLY_ALWAYS_INLINE
  uint8_t& byteAt(Size offset) const {
    return *reinterpret_cast<uint8_t*>(const_cast<Pointer>(buf) + offset);
  }

  FOLLY_ALWAYS_INLINE
  bool isNullAt(Size offset, uint8_t mask) const {
    return (byteAt(offset) & mask) != 0;
  }

  FOLLY_ALWAYS_INLINE
  void setNullAt(Size offset, uint8_t mask) const {
    byteAt(offset) |= mask;
  }
};

constexpr size_t broadcastByte(uint8_t val, size_t num) {
  size_t ret = 0;
  size_t typedVal = val;
  for (size_t i = 0; i < num; i++) {
    ret |= (typedVal << (i * 8));
  }
  return ret;
}

template <typename T, size_t Size>
class CircularQueue {
  static_assert((Size & (Size - 1)) == 0, "Size must be a power of 2");

 private:
  T data_[Size];
  size_t head_ = 0;
  size_t tail_ = 0;
  static constexpr size_t kMask = Size - 1;

 public:
  FOLLY_ALWAYS_INLINE bool enqueue(T val) {
    if (isFull()) {
      return false;
    }
    data_[tail_ & kMask] = val;
    tail_++;
    return true;
  }

  FOLLY_ALWAYS_INLINE bool dequeue(T& val) {
    if (isEmpty()) {
      return false;
    }
    val = data_[head_ & kMask];
    head_++;
    return true;
  }

  FOLLY_ALWAYS_INLINE bool isEmpty() const {
    return head_ == tail_;
  }
  FOLLY_ALWAYS_INLINE bool isFull() const {
    return (tail_ - head_) == Size;
  }
};

// refer to:
// https://github.com/greg7mdp/parallel-hashmap/blob/f5e1638a912cdb83fde49ed4611acf9832b54586/parallel_hashmap/phmap.h#L230
class PHBitMask {
  static constexpr int kShift = 3;

 public:
  explicit PHBitMask(uint64_t mask) : mask_(mask) {}

  PHBitMask& operator++() { // ++iterator
    mask_ &= (mask_ - 1); // clear the least significant bit set
    return *this;
  }

  explicit operator bool() const {
    return mask_ != 0;
  }
  uint32_t operator*() const {
    return lowestBitSet();
  }

  uint32_t lowestBitSet() const {
    return __builtin_ctzll(mask_) >> kShift;
  }

  PHBitMask begin() const {
    return *this;
  }
  PHBitMask end() const {
    return PHBitMask(0);
  }

  static inline PHBitMask matchTag(uint64_t tagVal, uint8_t tagHash) {
    constexpr uint64_t kMsbs = 0x8080808080808080ULL;
    constexpr uint64_t kLsbs = 0x0101010101010101ULL;
    auto x = tagVal ^ (kLsbs * tagHash);
    return PHBitMask((x - kLsbs) & ~x & kMsbs);
  }

  static inline PHBitMask matchEmpty(
      uint64_t tagVal,
      uint64_t msbs = 0x8080808080808080ULL) {
    return PHBitMask((tagVal & (~tagVal << 7)) & msbs);
  }

 private:
  friend bool operator==(const PHBitMask& a, const PHBitMask& b) {
    return a.mask_ == b.mask_;
  }
  friend bool operator!=(const PHBitMask& a, const PHBitMask& b) {
    return a.mask_ != b.mask_;
  }

  uint64_t mask_;
};

template <typename T, typename Enable = void>
struct KeyAccessor;

template <typename T>
struct KeyAccessor<T, std::enable_if_t<std::is_arithmetic_v<T>>> {
  explicit KeyAccessor(bool seedFlag) : seed_(seedFlag ? 0x37 : 0xe9) {}

  static FOLLY_ALWAYS_INLINE bool equals(const T& l, const T& r) {
    DEBUG_STAT_TLS_COUNT_INCR(debug::DebugStat::NumericEqualsCalls)
    return l == r;
  }

  uint64_t hash(T key) const {
    DEBUG_STAT_TLS_COUNT_INCR(debug::DebugStat::NumericHashCalls)
    reinterpret_cast<char*>(&key)[0] ^= seed_;
    union {
      T in;
      uint64_t out;
    } u;
    u.out = 0;
    u.in = key;
    return intHash64(u.out);
  }

  static FOLLY_ALWAYS_INLINE uint64_t intHash64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;

    return x;
  }

 private:
  uint8_t seed_;
};

template <typename T>
struct KeyAccessor<T, std::enable_if_t<std::is_same_v<T, FixedBuf>>> {
  explicit KeyAccessor(uint32_t keySize, bool seedFlag)
      : keySize_(keySize), seed_(seedFlag ? 0x37 : 0xe9) {}

  FOLLY_ALWAYS_INLINE bool equals(const FixedBuf& l, const FixedBuf& r) const {
    DEBUG_STAT_TLS_COUNT_INCR(debug::DebugStat::FixedBufEqualsCalls)
    return memcmp(&l, &r, keySize_) == 0;
  }

  FOLLY_ALWAYS_INLINE uint64_t hash(const FixedBuf& key) const {
    DEBUG_STAT_TLS_COUNT_INCR(debug::DebugStat::FixedBufHashCalls)
    return facebook::velox::bits::hashBytes(seed_, key.buf, keySize_);
  }

 private:
  uint32_t keySize_;
  uint8_t seed_;
};

template <typename T>
struct KeyAccessor<T, std::enable_if_t<std::is_same_v<T, CompactKeys>>> {
  using Size = CompactKeys::Size;
  using Pointer = CompactKeys::Pointer;

  KeyAccessor(Size keySize, Size sizeOffset, Size pointerOffset, bool seedFlag)
      : keySize_(keySize),
        sizeOffset_(sizeOffset),
        pointerOffset_(pointerOffset),
        seed_(seedFlag ? 0x37 : 0xe9) {}

  bool equals(const CompactKeys& l, const CompactKeys& r) const {
    DEBUG_STAT_TLS_COUNT_INCR(debug::DebugStat::CompactKeyEqualsCalls)
    auto lp = extendPointer(l);
    // no string keys
    if (LIKELY(lp == nullptr)) {
      return memcmp(l.buf, r.buf, keySize_) == 0;
    }

    // inline + extend
    auto rp = extendPointer(r);
    return memcmp(l.buf, r.buf, pointerOffset_) == 0 &&
        memcmp(lp, rp, l.valueAt<Size>(sizeOffset_)) == 0;
  }

  uint64_t hash(const CompactKeys& key) const {
    DEBUG_STAT_TLS_COUNT_INCR(debug::DebugStat::CompactKeyHashCalls)
    auto p = extendPointer(key);
    if (LIKELY(p == nullptr)) {
      return facebook::velox::bits::hashBytes(seed_, key.buf, keySize_);
    }

    uint64_t h = seed_;
    h = facebook::velox::bits::hashBytes(h, key.buf, pointerOffset_);
    h = facebook::velox::bits::hashBytes(h, p, key.valueAt<Size>(sizeOffset_));
    return h;
  }

  FOLLY_ALWAYS_INLINE
  Pointer extendPointer(const CompactKeys& key) const {
    if (LIKELY(pointerOffset_ == 0 || key.valueAt<Size>(sizeOffset_) <= 0)) {
      return nullptr;
    }
    return key.valueAt<Pointer>(pointerOffset_);
  }

 private:
  Size keySize_;
  Size sizeOffset_;
  Size pointerOffset_;
  uint8_t seed_;
};

template <uint8_t Count, uint8_t Step>
static constexpr std::array<uint8_t, Count> genByteSeq() {
  std::array<uint8_t, Count> result{};
  for (uint8_t i = 0; i < Count; ++i) {
    result[i] = i * Step;
  }
  return result;
}

} // namespace taper

class TaperHashTableChunk {
 public:
#if XSIMD_WITH_SSE2
  using TagVector = xsimd::batch<uint8_t, xsimd::sse2>;
#elif XSIMD_WITH_NEON
  using TagVector = xsimd::batch<uint8_t, xsimd::neon>;
#endif

  char* pointerAt(uint8_t idx) {
    auto ptr =
        *reinterpret_cast<uintptr_t*>(u_.s_.pointers_ + kPointersPos[idx]);
    return reinterpret_cast<char*>(ptr & kPointerMask);
  }

  void setPointer(uint8_t idx, const void* pointer) {
    auto* const slot =
        reinterpret_cast<uintptr_t*>(&(u_.s_.pointers_[kPointersPos[idx]]));
    *slot = (*slot & ~kPointerMask) | reinterpret_cast<uintptr_t>(pointer);
  }

  uint8_t* tagsBuf() {
    return buf();
  }
  TagVector tagVec() const {
    return u_.s_.tags_;
  }

  uint8_t* buf() {
    return u_.buf_;
  }
  const uint8_t* buf() const {
    return u_.buf_;
  }

  uint64_t getU64Tags() {
    // TODO ??????????????????????
    return *reinterpret_cast<uint64_t*>(u_.buf_);
  }

 public:
  static constexpr uint8_t kPointerSignificantBits = 48;
  static constexpr uint64_t kPointerMask =
      bits::lowMask(kPointerSignificantBits);
  static constexpr uint8_t kChunkSize = 128;
  static constexpr uint8_t kPointerSize = kPointerSignificantBits / 8;
  static constexpr uint8_t kPointerCount = sizeof(TagVector);

 private:
  static constexpr std::array<uint8_t, kPointerCount> kPointersPos =
      taper::genByteSeq<kPointerCount, kPointerSize>();

  union {
    struct {
      TagVector tags_;
      char pointers_[kPointerCount * kPointerSize];
      char padding_[kChunkSize - sizeof(TagVector) - sizeof(pointers_)];
    } s_;
    uint8_t buf_[kChunkSize];
  } u_;
};

class TaperContainer {
 public:
  virtual size_t allocatedBytes() const = 0;
  virtual size_t size() const = 0;
  virtual void printStats() = 0;
  virtual void clear() = 0;
};

template <typename Key, bool KeyScattered = false>
class TaperHashTableBase : public TaperContainer {
 public:
  using Value = taper::FixedBuf;
  using Chunk = TaperHashTableChunk;
  using ChunkPtr = TaperHashTableChunk*;
  using ChunkPos = uint32_t;

  enum class Operation : int {
    kInsert,
    kRehash,
    kSearch,
  };

  // from raw_hash_set
  // using difference_type = ptrdiff_t;

  TaperHashTableBase(
      memory::MemoryPool& memPool,
      uint32_t keySize,
      uint32_t valueSize,
      taper::KeyAccessor<Key> keyAccessor)
      : pool_(memPool),
        keyAccessor_(keyAccessor),
        keySize_(keySize),
        valueSize_(valueSize),
        rowSize_(keySize + valueSize) {}

  virtual ~TaperHashTableBase() {
#ifdef DEBUG_STAT_ENABLED
    debugStat_.print();
#endif
  }

  TaperHashTableBase(const TaperHashTableBase& other) = delete;
  TaperHashTableBase(TaperHashTableBase&&) noexcept = delete;
  TaperHashTableBase& operator=(const TaperHashTableBase&) = delete;
  TaperHashTableBase& operator=(TaperHashTableBase&&) noexcept = delete;

  size_t size() const override {
    return size_;
  }

  uint32_t rowSize() const {
    return rowSize_;
  }

  size_t capacity() const {
    return getChunksCapacity() * elemNumInChunk_;
  }

  void clear() override {
    init(0);
  }

  FOLLY_ALWAYS_INLINE
  size_t hash(const Key& x) const {
    if constexpr (KeyScattered) {
      return x;
    } else {
      return keyAccessor_.hash(x);
    }
  }

  size_t allocatedBytes() const override {
    return getChunksCapacity() * sizeof(TaperHashTableChunk);
  }

  uint64_t hashTableSizeIncrease(int32_t numNewDistinct) const {
    if (size_ + numNewDistinct > expandThreshold_) {
      return std::max(
          static_cast<uint64_t>(0),
          estimateHashTableSize(size_ + numNewDistinct) - allocatedBytes());
    }
    return 0;
  }

  uint64_t estimateChunksCapacity(uint64_t numDistinct) const {
    // min capacity that ensures expand threshold is not exceeded after adding
    // numDistinct entries
    auto elemCapacity = bits::divRoundUp(numDistinct * 8, 7);
    auto requiredChunks = bits::divRoundUp(elemCapacity, elemNumInChunk_);
    return std::max(1UL, bits::nextPowerOfTwo(requiredChunks));
  }

  uint64_t estimateHashTableSize(uint64_t numDistinct) const {
    return estimateChunksCapacity(numDistinct) * sizeof(TaperHashTableChunk);
  }

  uint32_t expandThreshold() const {
    return expandThreshold_;
  }

  void printStats() override {
#ifdef TAPER_HASH_STAT
    LOG(WARNING) << "key size: " << keySize_;
    LOG(WARNING) << "taper hash p1 statistics: ";
    LOG(WARNING) << "emplace count: " << p1EmplaceCount_
                 << ", collision count: " << p1CollisionCount_
                 << ", first collision count: " << p1FirstCollisionCount_
                 << ", max collision step: " << p1MaxCollisionStep_;
    LOG(WARNING) << "rehash collision count: " << p1RehashCollisionCount_
                 << ", rehash first collision count: "
                 << p1RehashFirstCollisionCount_
                 << ", rehash max collision step: "
                 << p1RehashMaxCollisionStep_;
    LOG(WARNING) << "taper hash p2 statistics: ";
    LOG(WARNING) << "emplace count: " << p2EmplaceCount_
                 << ", collision count: " << p2CollisionCount_
                 << ", first collision count: " << p2FirstCollisionCount_
                 << ", max collision step: " << p2MaxCollisionStep_;
    LOG(WARNING) << "rehash collision count: " << p2RehashCollisionCount_
                 << ", rehash first collision count: "
                 << p2RehashFirstCollisionCount_
                 << ", rehash max collision step: " << p2RehashMaxCollisiontep_;
#endif
#if (defined(HWP_PREFETCH) && !defined(DISABLE_PREFETCH))
    LOG(WARNING) << "hwp prefetch count: " << hwpPrefetchCount_
                 << ", hwp prefetch items num: " << hwpPrefetchItemsNum_
                 << ", hwp terminate num: " << hwpPrefetchTerminateCount_;
#endif
  }

  virtual std::string dbgDump(const std::string& fnPart) = 0;

 private:
  ChunkPos getChunkPos(size_t hashVal) {
    return hashVal & lastChunkIdx_;
  }

  ChunkPos getRehashPos(size_t collisionBatch, ChunkPos curPos) {
    return (curPos + collisionBatch) & lastChunkIdx_;
  }

  bool shouldExpand() {
    return size_ >= expandThreshold_;
  }

  void FOLLY_ALWAYS_INLINE prefetch(ChunkPos pos) {
    auto* chunk = chunks_ + pos;
    __builtin_prefetch(chunk);
    __builtin_prefetch(reinterpret_cast<char*>(chunk) + 64);
    static_assert(sizeof(TaperHashTableChunk) == 128);
  }

  void FOLLY_ALWAYS_INLINE
  prefetchIdx(const raw_vector<ChunkPos>& indices, size_t idx, size_t idxEnd) {
#if (!defined(DISABLE_PREFETCH))
    auto pi = idx + kHashMapPrefetchDist;
    if (LIKELY(pi < idxEnd)) {
      prefetch(indices[pi]);
    }
#endif
  }

  void FOLLY_ALWAYS_INLINE
  prefetchIdx(const raw_vector<ChunkPos>& indices, size_t idx) {
    prefetchIdx(indices, idx, indices.size());
  }

  void hwpPrefetch(const raw_vector<ChunkPos>& indices) {
    hwpPrefetch(indices, indices.size());
  }

  void hwpPrefetch(const raw_vector<ChunkPos>& indices, size_t size) {
#if (defined(HWP_PREFETCH) && !defined(DISABLE_PREFETCH))
    ++hwpPrefetchCount_;
    hwpPrefetchItemsNum_ += size;
    setupLinkHWP(chunks_, sizeof(TaperHashTableChunk), indices.data(), size);
#endif
  }

  void
  hwpPrefetch(const raw_vector<ChunkPos>& indices, size_t begin, size_t end) {
#if (defined(HWP_PREFETCH) && !defined(DISABLE_PREFETCH))
    hwpPrefetchItemsNum_ += (end - begin);
    ++hwpPrefetchCount_;
    setupLinkHWP(
        chunks_,
        sizeof(TaperHashTableChunk),
        indices.data() + begin,
        end - begin);
#endif
  }

  void hwpPrefetchTerminate() {
#if (defined(HWP_PREFETCH) && !defined(DISABLE_PREFETCH))
    ++hwpPrefetchTerminateCount_;
    terminateLinkHWP();
#endif
  }

  static inline bool isValidCapacity(size_t n) {
    return n > 0 && ((n - 1) & n) == 0;
  }

 protected:
  constexpr static uint8_t kEmptyTag = 0x80;

  union VisitorPos {
    using ChunkIdx = uint16_t;

    struct {
      ChunkIdx chunk;
      uint16_t tag;
    } chunkPos;
    uint32_t pos;

    VisitorPos(uint16_t chunk, uint16_t tag) : chunkPos{chunk, tag} {}
    explicit VisitorPos(uint32_t val) : pos{val} {}
  };

  struct BatchContext {
    raw_vector<uint64_t> hashVals;
    raw_vector<ChunkPos> chunkPositions;
    raw_vector<VisitorPos> collisionIndices{};

    void resize(uint32_t size) {
      hashVals.resize(size);
      chunkPositions.resize(size);
      collisionIndices.resize(size);
    }
  };

#ifdef TAPER_HASH_STAT
  size_t p1EmplaceCount_ = 0;
  size_t p1CollisionCount_ = 0;
  size_t p1FirstCollisionCount_ = 0;
  size_t p1MaxCollisionStep_ = 0;
  size_t p1RehashCollisionCount_ = 0;
  size_t p1RehashFirstCollisionCount_ = 0;
  size_t p1RehashMaxCollisionStep_ = 0;

  size_t p2EmplaceCount_ = 0;
  size_t p2CollisionCount_ = 0;
  size_t p2FirstCollisionCount_ = 0;
  size_t p2MaxCollisionStep_ = 0;
  size_t p2RehashFirstCollisionCount_ = 0;
  size_t p2RehashMaxCollisiontep_ = 0;
  size_t p2RehashCollisionCount_ = 0;
#endif

#if (defined(HWP_PREFETCH) && !defined(DISABLE_PREFETCH))
  size_t hwpPrefetchCount_ = 0;
  size_t hwpPrefetchItemsNum_ = 0;
  size_t hwpPrefetchTerminateCount_ = 0;
#endif

  DEBUG_STAT_DEFINE_MEMBER(debugStat_, TaperHashTable)
#ifdef DEBUG_STAT_ENABLED
  debug::DebugStat* debugStat() {
    return &debugStat_;
  }
#endif

 private:
  memory::MemoryPool& pool_;
  memory::ContiguousAllocation tableAllocation_;
  taper::KeyAccessor<Key> keyAccessor_;

  ChunkPtr chunks_ = nullptr;

  BatchContext emplaceContext_;
  BatchContext rehashContext_;
  static constexpr int32_t kStringPrefetchDist = 16;
  taper::CircularQueue<const Key*, kStringPrefetchDist> rehashPrefetchQ1_;
  taper::CircularQueue<const Key*, kStringPrefetchDist> rehashPrefetchQ2_;

  uint32_t size_ = 0;
  uint32_t lastChunkIdx_ = 0;
  uint32_t expandThreshold_ = 0;

  uint32_t keySize_ = 0;
  uint32_t valueSize_ = 0;
  uint32_t rowSize_ = 0;
  uint8_t elemNumInChunk_ = 0;

  bool rehashing_ = false;

 protected:
  ChunkPtr chunks() const {
    return chunks_;
  }
  uint32_t keySize() const {
    return keySize_;
  }
  uint32_t valueSize() const {
    return valueSize_;
  }
  uint8_t elemNumInChunk() const {
    return elemNumInChunk_;
  }
  void setElemNumInChunk(uint8_t val) {
    elemNumInChunk_ = val;
  }
  void incSize(uint32_t delta = 1) {
    size_ += delta;
  }

  void init(uint32_t lastChunkIdx) {
    auto chunkCapacity = lastChunkIdx + 1;
    assert(isValidCapacity(chunkCapacity));

    auto bytesNum = chunkCapacity * sizeof(TaperHashTableChunk);
    const auto numPages = memory::AllocationTraits::numPages(bytesNum);
    pool_.allocateContiguous(numPages, tableAllocation_);
    chunks_ = tableAllocation_.data<TaperHashTableChunk>();
    memset(chunks_, kEmptyTag, bytesNum);
    lastChunkIdx_ = lastChunkIdx;
    size_ = 0;

    // 7*capacity/8
    auto capacity = chunkCapacity * elemNumInChunk_;
    expandThreshold_ = capacity - (capacity >> 3);
    LOG(INFO) << "lastChunkIdx " << lastChunkIdx << " bytesNum: " << bytesNum;
  }

  template <
      Operation Op = Operation::kInsert,
      typename FKCmp,
      typename Derived,
      typename FInit,
      typename FUpdate>
  void operate(
      Derived& derived,
      const Key& key,
      FKCmp&& fKeyCmp,
      FInit&& fInit,
      FUpdate&& fUpdate) {
    DEBUG_STAT_TLS_COUNT_INCR(debug::DebugStat::HashCallsFromEmplaceImpl)
    auto hashVal = hash(key);
    auto chunkPos = getChunkPos(hashVal);
    size_t collisionBatch = 1;
    while (!derived.template tryOperateAtPos<Op>(
        key,
        hashVal,
        chunkPos,
        std::forward<FKCmp>(fKeyCmp),
        std::forward<FInit>(fInit),
        std::forward<FUpdate>(fUpdate))) {
#ifdef TAPER_HASH_STAT
      if (collisionBatch == 1) {
        p1FirstCollisionCount_++;
        if (rehashing_) {
          p1RehashFirstCollisionCount_++;
        }
      }
      p1CollisionCount_++;
      if (rehashing_) {
        p1RehashCollisionCount_++;
      }
#endif
      assert(collisionBatch <= getChunksCapacity());
      if constexpr (Op == Operation::kSearch) {
        chunkPos = getRehashPos(collisionBatch, chunkPos);
        ++collisionBatch;
      } else {
        if (shouldExpand()) {
          expandCapacityDirectly(derived);
          chunkPos = getChunkPos(hashVal);
          collisionBatch = 1;
        } else {
          chunkPos = getRehashPos(collisionBatch, chunkPos);
          ++collisionBatch;
        }
      }
    }
#ifdef TAPER_HASH_STAT
    auto step = collisionBatch - 1;
    if (step > p1MaxCollisionStep_) {
      p1MaxCollisionStep_ = step;
    }
    if (rehashing_ && step > p1RehashMaxCollisionStep_) {
      p1RehashMaxCollisionStep_ = step;
    }
#endif
  }

  template <typename Derived>
  void expandCapacityDirectly(Derived& derived) {
    DEBUG_STAT_TLS_COUNT_INCR(debug::DebugStat::ExpandDirectlyCalls)
    auto vOld = derived.getResultVisitor();
    auto allocBackup(std::move(tableAllocation_));
    init(expandLastChunkIdx());
    VELOX_CHECK(!rehashing_);
    rehashing_ = true;
    while (!vOld.finished()) {
      derived.rehashEmplace(vOld);
      vOld.next();
    }
    rehashing_ = false;
  }

  template <
      Operation Op = Operation::kInsert,
      typename Derived,
      typename FKGet,
      typename Filter,
      typename FKCmp,
      typename FInit,
      typename FUpdate>
  void operateBatch(
      Derived& derived,
      FKGet&& fKeyGet,
      uint32_t numRows,
      Filter&& filter,
      FKCmp&& fKeyCmp,
      FInit&& fInit,
      FUpdate&& fUpdate) {
    if (capacity() < numRows) {
      // ????????????????????????????????????
      emplaceBatchDirectly<Op>(
          derived,
          fKeyGet,
          numRows,
          std::forward<Filter>(filter),
          std::forward<FKCmp>(fKeyCmp),
          std::forward<FInit>(fInit),
          std::forward<FUpdate>(fUpdate));
      return;
    }

    resetEmplaceContext(fKeyGet, numRows);
    uint32_t collisionBatch = 1;
    int32_t collisionCount = 0;
    bool resized = false;
    auto resetPositions = [&](size_t begin, size_t end) {
      for (auto i = begin; i != end; i++) {
        emplaceContext_.chunkPositions[i] =
            getChunkPos(emplaceContext_.hashVals[i]);
      }
    };

    auto tryEmplaceIdx = [&]<typename ResizeProc>(
                             uint32_t rowIdx,
                             int32_t hashIdx,
                             ResizeProc&& resizeProc) {
      auto succeed = derived.template tryOperateAtPos<Op>(
          fKeyGet(rowIdx),
          emplaceContext_.hashVals[hashIdx],
          emplaceContext_.chunkPositions[hashIdx],
          [&](const Key& key, Chunk& chunk, uint8_t slot) {
            return fKeyCmp(rowIdx, key, chunk, slot);
          },
          [&](auto data) { fInit(rowIdx, data); },
          [&](auto data, bool initFlag) { fUpdate(rowIdx, data, initFlag); });
      if (!succeed) {
#ifdef TAPER_HASH_STAT
        p2CollisionCount_++;
        if (collisionBatch == 1) {
          p2FirstCollisionCount_++;
        }
#endif
        emplaceContext_.collisionIndices[collisionCount].pos = rowIdx;
        emplaceContext_.hashVals[collisionCount] =
            emplaceContext_.hashVals[hashIdx];
        emplaceContext_.chunkPositions[collisionCount] = getRehashPos(
            collisionBatch, emplaceContext_.chunkPositions[hashIdx]);
        collisionCount++;
        if constexpr (Op == Operation::kInsert) {
          if (shouldExpand()) {
            expandCapacityIteratively(derived);
            resized = true;
            resizeProc();
          }
        }
      }
#ifdef TAPER_HASH_STAT
      if (collisionBatch - 1 > p2MaxCollisionStep_) {
        p2MaxCollisionStep_ = collisionBatch - 1;
      }
#endif
    };

    auto tryEmplaceRehashedCollisions = [&] {
      resetPositions(0, collisionCount);
      auto curCount = collisionCount;
      collisionCount = 0;
      hwpPrefetch(emplaceContext_.chunkPositions, curCount);
      for (int32_t idx = 0; idx < curCount; idx++) {
        prefetchIdx(emplaceContext_.chunkPositions, idx, curCount);
        // ???????rehash???hash?????num_rows????????????????rehash?????????
        tryEmplaceIdx(emplaceContext_.collisionIndices[idx].pos, idx, [&] {
          assert(false);
        });
      }
    };

    auto resizeProc = [&](size_t remainIdxFrom, size_t remainIdxTo) {
      // ????????????????????????????
      // 1.
      // ???????????????????????????????????????????????1
      // 2. ??????????????????chunk??
      collisionBatch = 1;
      tryEmplaceRehashedCollisions();
      resetPositions(remainIdxFrom, remainIdxTo);
      hwpPrefetch(emplaceContext_.chunkPositions, remainIdxFrom, remainIdxTo);
    };

    hwpPrefetch(emplaceContext_.chunkPositions);
    // ????????????????????????????
    for (uint32_t i = 0; i < numRows; ++i) {
      if (filter(i)) {
        continue;
      }
#ifdef TAPER_HASH_STAT
      p2EmplaceCount_++;
#endif
      prefetchIdx(emplaceContext_.chunkPositions, i, numRows);
      tryEmplaceIdx(i, i, [&] { resizeProc(i + 1, numRows); });
    }

    // ????????????????????????????
    while (collisionCount > 0) {
      auto curCount = collisionCount;
      collisionCount = 0;
      collisionBatch++;
      hwpPrefetch(emplaceContext_.chunkPositions, curCount);
      for (int32_t idx = 0; idx < curCount; idx++) {
        prefetchIdx(emplaceContext_.chunkPositions, idx, curCount);
        tryEmplaceIdx(emplaceContext_.collisionIndices[idx].pos, idx, [&] {
          resizeProc(idx + 1, curCount);
        });
      }
    }
  }

  template <
      Operation Op = Operation::kInsert,
      typename Derived,
      typename FKGet,
      typename Filter,
      typename FKCmp,
      typename FInit,
      typename FUpdate>
  void emplaceBatchDirectly(
      Derived& derived,
      FKGet&& fKeyGet,
      uint32_t numRows,
      Filter&& filter,
      FKCmp&& fKeyCmp,
      FInit&& fInit,
      FUpdate&& fUpdate) {
    for (uint32_t i = 0; i < numRows; ++i) {
      if (filter(i)) {
        continue;
      }
#ifdef TAPER_HASH_STAT
      p1EmplaceCount_++;
#endif
      operate<Op>(
          derived,
          fKeyGet(i),
          [&](const Key& key, Chunk& chunk, uint8_t slot) {
            return fKeyCmp(i, key, chunk, slot);
          },
          [&](auto data) { fInit(i, data); },
          [&](auto data, bool initFlag) { fUpdate(i, data, initFlag); });
    }
  }

  template <typename Derived>
  void expandCapacityIteratively(Derived& derived) {
    DEBUG_STAT_TLS_COUNT_INCR(debug::DebugStat::ExpandIterativelyCalls)
    auto oldChunksNum = getChunksCapacity();
    auto* oldChunks = chunks_;

    auto allocBackup(std::move(tableAllocation_));
    hwpPrefetchTerminate();
    init(expandLastChunkIdx());

    derived.rehashChunksIteratively(oldChunks, oldChunksNum);
  }

  template <typename Derived, typename Visitor>
  void rehashBatch(Derived& derived, Visitor visitor) {
    VELOX_CHECK(!rehashing_);
    rehashing_ = true;
    // ????????????????????????hash?????chunk??
    // ?????????src_collision_positions?????????????chunk????tag???????hash?????chunk??
    // ??src_hash_values?dst_chunk_positions??????????????src?????match?
    resetRehashContext(visitor);
    uint32_t collisionBatch = 1;
    uint32_t collisionCount = 0;
    // ???????dst_chunk_positions[srcItemIdx]????????????????????????
    auto tryEmplace =
        [&]<typename VisitorPosRecorder>(
            VisitorPos vPos, size_t srcItemIdx, VisitorPosRecorder recorder) {
          auto succeed = derived.tryRehashAtPos(
              visitor,
              vPos,
              rehashContext_.hashVals[srcItemIdx],
              rehashContext_.chunkPositions[srcItemIdx]);
          if (!succeed) {
#ifdef TAPER_HASH_STAT
            p2RehashCollisionCount_++;
            if (collisionBatch == 1) {
              p2RehashFirstCollisionCount_++;
            }
#endif
            recorder();
            rehashContext_.hashVals[collisionCount] =
                rehashContext_.hashVals[srcItemIdx];
            rehashContext_.chunkPositions[collisionCount] = getRehashPos(
                collisionBatch, rehashContext_.chunkPositions[srcItemIdx]);
            collisionCount++;
          }
        };

    hwpPrefetch(rehashContext_.chunkPositions);
    // ????????????????????????????
    for (size_t srcItemIdx = 0; !visitor.finished(); srcItemIdx++) {
      prefetchIdx(rehashContext_.chunkPositions, srcItemIdx);
      auto visitorPos = visitor.curPos();
      tryEmplace(visitorPos, srcItemIdx, [&] {
        rehashContext_.collisionIndices[collisionCount] = visitorPos;
      });
      visitor.next();
    }

    // ????????????????????????????
    while (collisionCount > 0) {
      auto curCount = collisionCount;
      collisionCount = 0;
      assert(collisionBatch < getChunksCapacity());
      collisionBatch++;
      hwpPrefetch(rehashContext_.chunkPositions, curCount);
      for (size_t srcItemIdx = 0; srcItemIdx < curCount; srcItemIdx++) {
        prefetchIdx(rehashContext_.chunkPositions, srcItemIdx, curCount);
        tryEmplace(
            rehashContext_.collisionIndices[srcItemIdx], srcItemIdx, [&] {
              rehashContext_.collisionIndices[collisionCount] =
                  rehashContext_.collisionIndices[srcItemIdx];
            });
      }
    }
    rehashing_ = false;
#ifdef TAPER_HASH_STAT
    if (collisionBatch - 1 > p2RehashMaxCollisiontep_) {
      p2RehashMaxCollisiontep_ = collisionBatch - 1;
    }
#endif
  }

  size_t getChunksCapacity() const {
    return lastChunkIdx_ + 1;
  }

  uint32_t expandLastChunkIdx() {
    return 2 * lastChunkIdx_ + 1;
  }

  FOLLY_ALWAYS_INLINE
  bool keyEquals(const Key& l, const Key& r) {
    return keyAccessor_.equals(l, r);
  }

  const Key& keyAt(const Key* keys, size_t idx) {
    if constexpr (
        std::is_same_v<Key, taper::CompactKeys> ||
        std::is_same_v<Key, taper::FixedBuf>) {
      return *reinterpret_cast<const Key*>(
          reinterpret_cast<const uint8_t*>(keys) + idx * keySize_);
    } else {
      return keys[idx];
    }
  }

  const Key& keyAt(char** rows, size_t idx) {
    return *reinterpret_cast<const Key*>(rows[idx]);
  }

  template <typename FKGet>
  void resetEmplaceContext(FKGet&& fKeyGet, uint32_t numRows) {
    emplaceContext_.resize(numRows);
    DEBUG_STAT_TLS_RECORD_SCOPE(debug::DebugStat::ResetEmplaceContextCalls)
    DEBUG_STAT_TLS_COUNT(
        debug::DebugStat::HashCallsFromResetEmplaceContext, numRows)
    for (uint32_t i = 0; i < numRows; i++) {
      auto val = hash(fKeyGet(i));
      emplaceContext_.hashVals[i] = val;
      emplaceContext_.chunkPositions[i] = getChunkPos(val);
    }
  }

  template <typename Visitor>
  void resetRehashContext(Visitor v) {
    rehashContext_.resize(v.maxElems());
    DEBUG_STAT_TLS_RECORD_SCOPE(debug::DebugStat::ResetRehashContextCalls)
    DEBUG_STAT_TLS_COUNT(
        debug::DebugStat::HashCallsFromResetRehashContext, v.maxElems())

    if constexpr (std::is_same_v<Key, taper::CompactKeys>) {
      const Key* key;
      VELOX_CHECK(rehashPrefetchQ1_.isEmpty());
      VELOX_CHECK(rehashPrefetchQ2_.isEmpty());
      uint32_t i = 0;

      auto consumeQ1 = [&]() {
        rehashPrefetchQ1_.dequeue(key);
        auto p = keyAccessor_.extendPointer(*key);
        if (p != nullptr) {
          __builtin_prefetch(p);
        }
        rehashPrefetchQ2_.enqueue(key);
      };

      auto consumeQ2 = [&]() {
        rehashPrefetchQ2_.dequeue(key);
        auto val = hash(*key);
        rehashContext_.hashVals[i] = val;
        rehashContext_.chunkPositions[i] = getChunkPos(val);
        i++;
      };

      while (!v.finished()) {
        if (rehashPrefetchQ2_.isFull()) {
          consumeQ2();
        }
        if (rehashPrefetchQ1_.isFull()) {
          consumeQ1();
        }
        __builtin_prefetch(&v.curKey());
        rehashPrefetchQ1_.enqueue(&v.curKey());
        v.next();
      }
      while (!rehashPrefetchQ1_.isEmpty()) {
        if (rehashPrefetchQ2_.isFull()) {
          consumeQ2();
        }
        consumeQ1();
      }
      while (!rehashPrefetchQ2_.isEmpty()) {
        consumeQ2();
      }
    } else {
      for (uint32_t i = 0; !v.finished(); i++) {
        auto val = hash(v.curKey());
        rehashContext_.hashVals[i] = val;
        rehashContext_.chunkPositions[i] = getChunkPos(val);
        v.next();
      }
    }
  }

  static void setupLinkHWP(
      const void* elementBaseAddr,
      uint8_t elemSize,
      const uint32_t* indexBaseAddr,
      uint32_t idxSize) {
    constexpr uint32_t LAZY_STEP = 64;
    if (LIKELY(idxSize > LAZY_STEP)) {
      indexBaseAddr += LAZY_STEP;
      idxSize -= LAZY_STEP;
    }

    uint64_t elementSize = elemSize;
    uint64_t indexSize = (idxSize << 2);
    // build data for HWP_LINK_RPRFM_EL0
    uint64_t msrParam = 0;
    msrParam |= reinterpret_cast<uint64_t>(elementBaseAddr) & 0x1ffffffffffff;
    msrParam |= (elementSize & 0xff) << 49;
    msrParam |= (1ULL) << 63;
    asm volatile("msr S3_3_c15_c2_0, %0" : : "r"(msrParam));
    asm volatile("isb");

    register uint64_t x3 asm("x3") = indexSize;
    register uint64_t x6 asm("x6") = (uint64_t)indexBaseAddr;
    __asm__ __volatile__(".inst 0xF8A348D8" : : "r"(x3), "r"(x6) : "memory");
  }

  static void terminateLinkHWP() {
    // build data for HWP_LINK_RPRFM_EL0
    uint64_t msrParam = 0;
    asm volatile("msr S3_3_c15_c2_0, %0" : : "r"(msrParam));
  }

  template <typename ChunkDumper>
  std::string dbgDumpImpl(
      const std::string& fileNamePart,
      ChunkDumper cDumper) {
    std::ostringstream oss;
    oss << "/tmp/hash-result-" << fileNamePart << "-"
        << reinterpret_cast<void*>(this);
    std::ofstream outfile(oss.str());
    auto endChunk = chunks() + getChunksCapacity();
    for (auto chunk = chunks(); chunk != endChunk; chunk++) {
      int cnt = 0;
      for (int tagPos = 0; tagPos < elemNumInChunk(); tagPos++) {
        if (chunk->tagsBuf()[tagPos] != kEmptyTag) {
          cDumper(chunk, tagPos, outfile);
          cnt++;
        }
      }
      outfile << "------- " << cnt << "\n";
    }
    outfile.close();
    return oss.str();
  }
};

// `RowSize` is raised into template args, because it will be heavily used in
// hot path to calculate key position or value position.
template <typename Key, size_t RowSize, bool KeyScattered = false>
class TaperFlatHashTable : public TaperHashTableBase<Key, KeyScattered> {
  using Base = TaperHashTableBase<Key, KeyScattered>;
  using Chunk = TaperHashTableChunk;
  using ChunkPtr = TaperHashTableChunk*;
  using ChunkPos = typename Base::ChunkPos;
  using VisitorPos = typename Base::VisitorPos;
  using Value = typename Base::Value;
  using PHBitMask = taper::PHBitMask;
  using Operation = typename Base::Operation;

  friend class TaperHashTableBase<Key, KeyScattered>;

  constexpr static size_t kL1DataSize = 64 * 1024;

 public:
  using KeyType = Key;
  constexpr static bool kKeyScattered = KeyScattered;
  constexpr static size_t kRowSize = RowSize;
  constexpr static size_t kRowStride = kRowSize;
  constexpr static size_t kKeySize = sizeof(Key);
  constexpr static size_t kValueSize = kRowSize - kKeySize;
  static_assert(std::is_arithmetic_v<Key>, "Key must be an arithmetic type");
  static_assert(kRowSize > kKeySize, "Row size must be greater than key size");

  class Visitor {
    friend class TaperFlatHashTable<Key, RowSize, KeyScattered>;

   public:
    void next() {
      do {
        tagPos_++;
        if (tagPos_ >= table_.elemNumInChunk()) {
          tagPos_ = 0;
          ++curChunk_;
        }
      } while (curChunk_ != endChunk_ &&
               curChunk_->tagsBuf()[tagPos_] == Base::kEmptyTag);
    }

    bool finished() const {
      return curChunk_ >= endChunk_;
    }

    uint32_t maxElems() const {
      return (endChunk_ - fromChunk_) * table_.elemNumInChunk();
    }

    Key& curKey() const {
      return table_.chunkKey(*curChunk_, tagPos_);
    }

    char* curVal() const {
      return table_.chunkVal(*curChunk_, tagPos_);
    }

    char* curRow() const {
      return table_.chunkRow(*curChunk_, tagPos_);
    }

    template <typename Fn>
    void savePos(Fn&& fn) {
      fn(curChunk_, tagPos_);
    }

    uint8_t rowSize() {
      return table_.rowSize();
    }

   protected:
    TaperFlatHashTable& table_;
    ChunkPtr curChunk_;
    ChunkPtr fromChunk_;
    ChunkPtr endChunk_;
    uint16_t tagPos_;

    // friend class TaperHashTable;

    explicit Visitor(
        TaperFlatHashTable& table,
        ChunkPtr fromChunk = nullptr,
        ChunkPtr endChunk = nullptr,
        uint16_t tagPos = 0)
        : table_(table),
          curChunk_{fromChunk},
          fromChunk_{fromChunk},
          endChunk_{endChunk},
          tagPos_{tagPos} {
      if (curChunk_ < endChunk_ &&
          curChunk_->tagsBuf()[tagPos_] == Base::kEmptyTag) {
        next();
      }
    }

    ChunkPtr getChunk(uint32_t offset) const {
      return fromChunk_ + offset;
    }
  };

  class ShortVisitor : public Visitor {
    using ChunkIdx = typename VisitorPos::ChunkIdx;

   public:
    explicit ShortVisitor(
        TaperFlatHashTable& table,
        ChunkPtr fromChunk = nullptr,
        ChunkPtr endChunk = nullptr,
        uint16_t tagPos = 0)
        : Visitor{table, fromChunk, endChunk, tagPos} {
      VELOX_CHECK_LT(Visitor::maxElems(), std::numeric_limits<ChunkIdx>::max());
    }

    VisitorPos curPos() const {
      auto c = static_cast<ChunkIdx>(Visitor::curChunk_ - Visitor::fromChunk_);
      return VisitorPos{c, Visitor::tagPos_};
    }
  };

  explicit TaperFlatHashTable(
      memory::MemoryPool& memPool,
      uint32_t initCapacity = 0)
      : Base(memPool, kKeySize, kValueSize, taper::KeyAccessor<Key>{false}) {
    constexpr size_t kTagByte = 1;
    VELOX_CHECK_EQ(Base::rowSize(), kRowSize);
    auto estimateElemSize = kTagByte + kRowSize;
    VELOX_CHECK(estimateElemSize < sizeof(TaperHashTableChunk));
    // Currenly, not support for more than 8 elements in a chunk, as tag
    // comparison is limited to 8 bytes.
    uint8_t elemNum = std::min(
        8UL, bits::divRoundUp(sizeof(TaperHashTableChunk), estimateElemSize));
    while (elemNum > 0 && !initChunkOffsets(elemNum)) {
      elemNum--;
    }
    VELOX_CHECK_GT(elemNum, 0);
    Base::setElemNumInChunk(elemNum);
    emptyTags_ = taper::broadcastByte(Base::kEmptyTag, elemNum);

    uint32_t initChunkNum = std::max(
        1UL,
        expandChunkNumIfNeeded(bits::divRoundUp(initCapacity + 1, elemNum)));
    Base::init(initChunkNum - 1);
  }

  ~TaperFlatHashTable() override = default;

  template <typename Filter, typename FInit, typename FUpdate>
  void emplaceBatch(
      const Key* keys,
      uint32_t numRows,
      Filter&& filter,
      FInit&& fInit,
      FUpdate&& fUpdate) {
    DEBUG_STAT_TLS_EXPOSE_SCOPE(Base::debugStat())
    DEBUG_STAT_TLS_COUNT_INCR(debug::DebugStat::EmplaceBatchCalls)
    DEBUG_STAT_TLS_COUNT(debug::DebugStat::EmplaceCounts, numRows)
    Base::template operateBatch<Operation::kInsert>(
        *this,
        [&](uint32_t idx) { return Base::keyAt(keys, idx); },
        numRows,
        std::forward<Filter>(filter),
        [&](uint32_t, const Key& key, Chunk& chunk, uint8_t slot) {
          return Base::keyEquals(key, chunkKey(chunk, slot));
        },
        std::forward<FInit>(fInit),
        std::forward<FUpdate>(fUpdate));
  }

  // Preserve HashLookup's selected row numbers through all batch callbacks.
  template <typename Filter, typename FInit, typename FUpdate>
  void emplaceBatch(
      const Key* keys,
      const vector_size_t* indices,
      uint32_t numIndices,
      Filter&& filter,
      FInit&& fInit,
      FUpdate&& fUpdate) {
    DEBUG_STAT_TLS_EXPOSE_SCOPE(Base::debugStat())
    DEBUG_STAT_TLS_COUNT_INCR(debug::DebugStat::EmplaceBatchCalls)
    DEBUG_STAT_TLS_COUNT(debug::DebugStat::EmplaceCounts, numIndices)
    Base::template operateBatch<Operation::kInsert>(
        *this,
        [&](uint32_t idx) { return Base::keyAt(keys, indices[idx]); },
        numIndices,
        [&](uint32_t idx) { return filter(indices[idx]); },
        [&](uint32_t, const Key& key, Chunk& chunk, uint8_t slot) {
          return Base::keyEquals(key, chunkKey(chunk, slot));
        },
        [&](uint32_t idx, auto data) { fInit(indices[idx], data); },
        [&](uint32_t idx, auto data, bool isInit) {
          fUpdate(indices[idx], data, isInit);
        });
  }

  template <typename Filter, typename FEmpty, typename FFound>
  void probeBatch(
      const Key* keys,
      const vector_size_t* indices,
      uint32_t numIndices,
      Filter&& filter,
      FEmpty&& fEmpty,
      FFound&& fFound) {
    DEBUG_STAT_TLS_EXPOSE_SCOPE(Base::debugStat())
    DEBUG_STAT_TLS_COUNT_INCR(debug::DebugStat::EmplaceBatchCalls)
    DEBUG_STAT_TLS_COUNT(debug::DebugStat::EmplaceCounts, numIndices)
    Base::template operateBatch<Operation::kSearch>(
        *this,
        [&](uint32_t idx) { return Base::keyAt(keys, indices[idx]); },
        numIndices,
        std::forward<Filter>(filter),
        [&](uint32_t, const Key& key, Chunk& chunk, uint8_t slot) {
          return Base::keyEquals(key, chunkKey(chunk, slot));
        },
        [&](auto, auto) {},
        [&](auto idx, auto data, bool isInit) {
          if (isInit) {
            fEmpty(idx, data);
          } else {
            fFound(idx, data);
          }
        });
  }

  template <typename Filter, typename FInit, typename FUpdate>
  void emplaceBatch(
      char** groups,
      uint32_t numRows,
      Filter&& filter,
      FInit&& fInit,
      FUpdate&& fUpdate) {
    DEBUG_STAT_TLS_EXPOSE_SCOPE(Base::debugStat())
    DEBUG_STAT_TLS_COUNT_INCR(debug::DebugStat::EmplaceBatchCalls)
    DEBUG_STAT_TLS_COUNT(debug::DebugStat::EmplaceCounts, numRows)
    Base::template operateBatch<Operation::kInsert>(
        *this,
        [&](uint32_t idx) { return Base::keyAt(groups, idx); },
        numRows,
        std::forward<Filter>(filter),
        [&](uint32_t, const Key& key, Chunk& chunk, uint8_t slot) {
          return Base::keyEquals(key, chunkKey(chunk, slot));
        },
        std::forward<FInit>(fInit),
        std::forward<FUpdate>(fUpdate));
  }

  template <
      DVecLayout Layout,
      TypeKind Kind,
      typename Filter,
      typename FInit,
      typename FUpdate>
  void emplaceBatch(
      DecodedVector& vector,
      uint32_t numRows,
      Filter&& filter,
      FInit&& fInit,
      FUpdate&& fUpdate) {
    DEBUG_STAT_TLS_EXPOSE_SCOPE(Base::debugStat())
    DEBUG_STAT_TLS_COUNT_INCR(debug::DebugStat::EmplaceBatchCalls)
    DEBUG_STAT_TLS_COUNT(debug::DebugStat::EmplaceCounts, numRows)

    using T = typename TypeTraits<Kind>::NativeType;
    Base::template operateBatch<Operation::kInsert>(
        *this,
        [&](uint32_t idx) { return vector.valueAt<T, Layout>(idx); },
        numRows,
        std::forward<Filter>(filter),
        [&](uint32_t, const Key& key, Chunk& chunk, uint8_t slot) {
          return Base::keyEquals(key, chunkKey(chunk, slot));
        },
        std::forward<FInit>(fInit),
        std::forward<FUpdate>(fUpdate));
  }

  template <typename FKCmp, typename FInit, typename FUpdate>
  void
  emplace(const Key& key, FKCmp&& fKeyCmp, FInit&& fInit, FUpdate&& fUpdate) {
    DEBUG_STAT_TLS_EXPOSE_SCOPE(Base::debugStat())
    DEBUG_STAT_TLS_COUNT_INCR(debug::DebugStat::EmplaceCalls)
    DEBUG_STAT_TLS_COUNT_INCR(debug::DebugStat::EmplaceCounts)
    Base::template operate<Operation::kInsert>(
        *this, key, fKeyCmp, fInit, fUpdate);
  }

  template <typename FKCmp, typename FEmpty, typename FFound>
  void
  probe(const Key& key, FKCmp&& fKeyCmp, FEmpty&& fEmpty, FFound&& fFound) {
    DEBUG_STAT_TLS_EXPOSE_SCOPE(Base::debugStat())
    DEBUG_STAT_TLS_COUNT_INCR(debug::DebugStat::EmplaceCalls)
    DEBUG_STAT_TLS_COUNT_INCR(debug::DebugStat::EmplaceCounts)
    Base::template operate<Operation::kSearch>(
        *this,
        key,
        fKeyCmp,
        [&](auto) {},
        [&](auto data, bool isInit) {
          if (isInit) {
            fEmpty(data);
          } else {
            fFound(data);
          }
        });
  }

  FOLLY_ALWAYS_INLINE
  char* chunkRow(TaperHashTableChunk& chunk, uint32_t idx) {
    return reinterpret_cast<char*>(
        chunk.buf() + keyOffsetInChunk_ + idx * kRowStride);
  }

  FOLLY_ALWAYS_INLINE
  const char* chunkRow(const TaperHashTableChunk& chunk, uint32_t idx) {
    return reinterpret_cast<const char*>(
        chunk.buf() + keyOffsetInChunk_ + idx * kRowStride);
  }

  FOLLY_ALWAYS_INLINE
  Key& chunkKey(TaperHashTableChunk& chunk, uint32_t idx) {
    return *reinterpret_cast<Key*>(chunkRow(chunk, idx));
  }

  FOLLY_ALWAYS_INLINE
  const Key& chunkKey(const TaperHashTableChunk& chunk, uint32_t idx) {
    return *reinterpret_cast<const Key*>(chunkRow(chunk, idx));
  }

  FOLLY_ALWAYS_INLINE
  char* chunkVal(TaperHashTableChunk& chunk, uint32_t idx) {
    return chunkRow(chunk, idx) + kKeySize;
  }

  FOLLY_ALWAYS_INLINE
  const char* chunkVal(const TaperHashTableChunk& chunk, uint32_t idx) {
    return chunkRow(chunk, idx) + kKeySize;
  }

  FOLLY_ALWAYS_INLINE
  const uint64_t expandChunkNumIfNeeded(uint64_t chunkNum) {
    uint64_t newChunkNum = bits::nextPowerOfTwo(chunkNum);
    if (chunkNum > newChunkNum - newChunkNum / 8) {
      newChunkNum *= 2;
    }
    return newChunkNum;
  }

  Visitor getResultVisitor() {
    auto end = Base::chunks() + Base::getChunksCapacity();
    return Visitor(*this, Base::chunks(), end);
  }

  Visitor getResultVisitor(char* from, uint16_t tagPos) {
    return Visitor(
        *this,
        reinterpret_cast<ChunkPtr>(from),
        Base::chunks() + Base::getChunksCapacity(),
        tagPos);
  }

  std::string dbgDump(const std::string& fnPart) override {
    return Base::dbgDumpImpl(fnPart, [&](auto chunk, auto tagPos, auto& os) {
      auto key = *reinterpret_cast<const int*>(&chunkKey(*chunk, tagPos));
      // auto val = *reinterpret_cast<double*>(&chunkVal(*chunk, tagPos));
      os << key << "\n";
    });
  }

 private:
  uint64_t emptyTags_ = 0;
  uint8_t keyOffsetInChunk_ = 0;

  bool initChunkOffsets(uint8_t elemNum) {
    keyOffsetInChunk_ = (elemNum + 7) & 0xF8;
    return keyOffsetInChunk_ + elemNum * kRowStride <=
        sizeof(TaperHashTableChunk);
  }

  void copyValue(char* dst, char* src) {
    simd::memcpy(dst, src, kValueSize);
  }

  template <Operation Op, typename FKCmp, typename FInit, typename FUpdate>
  bool tryOperateAtPos(
      const Key& key,
      size_t hashVal,
      ChunkPos chunkPos,
      FKCmp&& fKeyCmp,
      FInit&& fInit,
      FUpdate&& fUpdate) {
    auto curChunk = Base::chunks() + chunkPos;
    uint8_t tagHash = (hashVal >> 16) & 0x7F;

    auto tags = curChunk->getU64Tags();
    if constexpr (Op != Operation::kRehash) {
      for (auto i : PHBitMask::matchTag(tags, tagHash)) {
        if (fKeyCmp(key, *curChunk, i)) {
          fUpdate(chunkVal(*curChunk, i), false);
          return true;
        }
      }
    }
    for (auto i : PHBitMask::matchEmpty(tags, emptyTags_)) {
      // In probe mode, we only invoke fUpdate to send "not found" result
      if constexpr (Op == Operation::kSearch) {
        fUpdate(chunkVal(*curChunk, i), true);
        return true;
      }
      Base::incSize();
      curChunk->tagsBuf()[i] = tagHash;
      auto* value = chunkVal(*curChunk, i);

      // set key after fInit, as in some cases, fInit may inits whole row
      // semantically.
      fInit(value);
      chunkKey(*curChunk, i) = key;
      fUpdate(value, true);
      return true;
    }
    return false;
  }

  bool tryRehashAtPos(
      const Visitor& visitor,
      VisitorPos visitorPos,
      size_t hashVal,
      ChunkPos chunkPos) {
    auto chunk = visitor.getChunk(visitorPos.chunkPos.chunk);
    return tryOperateAtPos<Operation::kRehash>(
        chunkKey(*chunk, visitorPos.chunkPos.tag),
        hashVal,
        chunkPos,
        [](auto, auto, auto) { return false; },
        [&](char* dstVal) {
          copyValue(dstVal, chunkVal(*chunk, visitorPos.chunkPos.tag));
        },
        [](auto, bool) {});
  }

  void rehashEmplace(const Visitor& visitor) {
    Base::template operate<Operation::kRehash>(
        *this,
        visitor.curKey(),
        [](auto, auto, auto) { return false; },
        [&](char* dstVal) { copyValue(dstVal, visitor.curVal()); },
        [](auto, bool) {});
  }

  void rehashChunksIteratively(ChunkPtr chunks, size_t chunksNum) {
    constexpr size_t kStep = kL1DataSize / sizeof(TaperHashTableChunk) * 3 / 4;
    for (size_t chunkEndIdx = 0; chunkEndIdx < chunksNum;) {
      auto* fromChunk = chunks + chunkEndIdx;
      chunkEndIdx = std::min(chunkEndIdx + kStep, chunksNum);
      auto* endChunk = chunks + chunkEndIdx;
      Base::rehashBatch(*this, ShortVisitor{*this, fromChunk, endChunk});
    }
  }
};

} // namespace facebook::velox::exec

#undef DUMMY_CMP
