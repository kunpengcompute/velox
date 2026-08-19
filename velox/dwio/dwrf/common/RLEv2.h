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

#pragma once

#include "velox/common/memory/Memory.h"
#include "velox/common/process/ProcessBase.h"
#include "velox/dwio/common/Adaptor.h"
#include "velox/dwio/common/DataBuffer.h"
#include "velox/dwio/common/DecoderUtil.h"
#include "velox/dwio/common/IntDecoder.h"
#include "velox/dwio/common/exception/Exception.h"

#include <vector>

namespace facebook::velox::dwrf {

template <bool isSigned>
class RleDecoderV2 : public dwio::common::IntDecoder<isSigned> {
 public:
  enum EncodingType {
    SHORT_REPEAT = 0,
    DIRECT = 1,
    PATCHED_BASE = 2,
    DELTA = 3
  };

  RleDecoderV2(
      std::unique_ptr<dwio::common::SeekableInputStream> input,
      memory::MemoryPool& pool);

  /**
   * Seek to a specific row group.
   */
  void seekToRowGroup(dwio::common::PositionProvider&) override;

  void skipPending() override;

  /**
   * Read a number of values into the batch.
   */
  void next(int64_t* data, uint64_t numValues, const uint64_t* nulls) override;

  void nextLengths(int32_t* const data, const int32_t numValues) override {
    skipPending();
    for (int i = 0; i < numValues; ++i) {
      data[i] = readValue();
    }
  }

  template <bool hasNulls, typename Visitor>
  void readWithVisitor(const uint64_t* nulls, Visitor visitor) {
    skipPending();
    int32_t current = visitor.start();
    this->template skip<hasNulls>(current, 0, nulls);

    bool atEnd = false;
    const bool allowNulls = hasNulls && visitor.allowNulls();

    if (useBatchRead<hasNulls>(visitor)) {
      batchReadLoop<hasNulls>(
          nulls, visitor, current, atEnd, allowNulls);
      return;
    }

    int32_t toSkip;
    for (;;) {
      if (hasNulls && allowNulls && bits::isBitNull(nulls, current)) {
        toSkip = visitor.processNull(atEnd);
      } else {
        if (hasNulls && !allowNulls) {
          toSkip = visitor.checkAndSkipNulls(nulls, current, atEnd);
          if (!Visitor::dense) {
            this->template skip<false>(toSkip, current, nullptr);
          }
          if (atEnd) {
            return;
          }
        }

        // We are at a non-null value on a row to visit.
        auto value = readValue();
        toSkip = visitor.process(value, atEnd);
      }

      ++current;
      if (toSkip) {
        this->template skip<hasNulls>(toSkip, current, nulls);
        current += toSkip;
      }
      if (atEnd) {
        return;
      }
    }
  }

  /// Gate for the batched decode loop. Requires dense rows: the batch path
  /// pre-decodes a run of consecutive non-null rows assuming they map to
  /// consecutive decoder values with no gaps, which holds only for dense rows
  /// (where process() returns toSkip==0 for adjacent rows). For sparse rows
  /// process() may return toSkip>0 (gaps), which would invalidate the
  /// pre-decoded buffer, so sparse falls back to per-value.
  template <bool hasNulls, typename Visitor>
  bool useBatchRead(Visitor& visitor) {
    return Visitor::dense &&
        process::hasSimd() &&
        !std::is_same_v<typename Visitor::DataType, int128_t> &&
        Visitor::FilterType::deterministic;
  }

  /// Batched version of the per-value loop. Walks rows exactly like the
  /// fallback (same null checks, same visitor calls, same skip logic), but when
  /// it reaches a non-null row it peeks ahead in the null bitmap to find the
  /// longest run of consecutive non-null rows and decodes them with a single
  /// next* call into a scratch buffer, then processes each value. This amortizes
  /// the per-value readValue() dispatch (run-state-machine re-entry + single-
  /// value next* call) over the run length. Null handling is unchanged: null
  /// rows go through processNull / checkAndSkipNulls exactly as before, and
  /// decoder skips between non-contiguous rows use the same skip<hasNulls>.
  template <bool hasNulls, typename Visitor>
  void batchReadLoop(
      const uint64_t* nulls,
      Visitor& visitor,
      int32_t current,
      bool& atEnd,
      bool allowNulls) {
    using T = typename Visitor::DataType;
    int32_t toSkip;
    for (;;) {
      if (hasNulls && allowNulls && bits::isBitNull(nulls, current)) {
        toSkip = visitor.processNull(atEnd);
        ++current;
        if (toSkip) {
          this->template skip<hasNulls>(toSkip, current, nulls);
          current += toSkip;
        }
        if (atEnd) {
          return;
        }
        continue;
      }
      if (hasNulls && !allowNulls) {
        toSkip = visitor.checkAndSkipNulls(nulls, current, atEnd);
        if (!Visitor::dense) {
          this->template skip<false>(toSkip, current, nullptr);
        }
        if (atEnd) {
          return;
        }
      }
      // We are at a non-null row. Peek ahead to find the longest run of
      // consecutive non-null rows starting at 'current' (bounded by the
      // visitor's remaining rows). next* reads that many consecutive decoder
      // values. This is correct because decoder values are 1:1 with non-null
      // rows, and consecutive non-null rows map to consecutive decoder values.
      int32_t numValues = visitor.numRows() - visitor.rowIndex();
      if (hasNulls && numValues > 1) {
        // Count consecutive non-nulls in [current+1, current+numValues).
        int32_t contiguous = 1;
        while (contiguous < numValues &&
               !bits::isBitNull(nulls, current + contiguous)) {
          ++contiguous;
        }
        numValues = contiguous;
      }
      if (numValues > 1) {
        // Batch decode numValues values into scratch, then process each.
        if (bulkScratch_.size() < static_cast<uint64_t>(numValues)) {
          bulkScratch_.resize(static_cast<uint64_t>(numValues));
        }
        int64_t* buf = bulkScratch_.data();
        // Decode consecutive values. nextBatch advances runRead_ across run
        // boundaries as needed (doNext-style), so this works even if the
        // contiguous non-null run spans multiple RLE runs.
        nextBatch(buf, numValues);
        for (int32_t i = 0; i < numValues; ++i) {
          toSkip = visitor.process(static_cast<T>(buf[i]), atEnd);
          ++current;
          if (toSkip) {
            // For dense rows toSkip is normally 0 (adjacent rows). If non-zero
            // (e.g. filter dropped trailing rows), the remaining pre-decoded
            // values are at wrong decoder positions, so skip the gap and break
            // to re-enter single-value mode. The skipped decoder values were
            // NOT pre-decoded (nextBatch decoded exactly numValues), so the
            // gap skip is correct.
            this->template skip<hasNulls>(toSkip, current, nulls);
            current += toSkip;
          }
          if (atEnd) {
            return;
          }
          if (toSkip) {
            break; // re-enter loop; remaining buffered values are discarded
          }
        }
      } else {
        // Single non-null row (or last in run): per-value decode.
        auto value = readValue();
        toSkip = visitor.process(value, atEnd);
        ++current;
        if (toSkip) {
          this->template skip<hasNulls>(toSkip, current, nulls);
          current += toSkip;
        }
        if (atEnd) {
          return;
        }
      }
    }
  }

  /// Decodes up to 'numValues' consecutive values into 'buf', advancing the
  /// run state machine across run boundaries (like doNext but without null
  /// handling, since batchReadLoop only calls this for consecutive non-null
  /// rows). Returns the number of values decoded. The current run's header
  /// must already be parsed (runRead_ < runLength_ or caller resets).
  uint64_t nextBatch(int64_t* buf, int32_t numValues) {
    uint64_t nRead = 0;
    while (nRead < static_cast<uint64_t>(numValues)) {
      if (runRead_ == runLength_) {
        resetRun();
      }
      uint64_t offset = nRead;
      uint64_t length = static_cast<uint64_t>(numValues) - nRead;
      switch (type_) {
        case SHORT_REPEAT:
          nRead += nextShortRepeats(buf, offset, length, nullptr);
          break;
        case DIRECT:
          nRead += nextDirect(buf, offset, length, nullptr);
          break;
        case PATCHED_BASE:
          nRead += nextPatched(buf, offset, length, nullptr);
          break;
        case DELTA:
          nRead += nextDelta(buf, offset, length, nullptr);
          break;
        default:
          VELOX_FAIL("unknown encoding: {}", static_cast<int>(type_));
      }
    }
    return nRead;
  }

  unsigned char readByte() {
    if (dwio::common::IntDecoder<isSigned>::bufferStart_ ==
        dwio::common::IntDecoder<isSigned>::bufferEnd_) {
      int32_t bufferLength;
      const void* bufferPointer;
      const bool ret = dwio::common::IntDecoder<isSigned>::inputStream_->Next(
          &bufferPointer, &bufferLength);
      VELOX_CHECK(
          ret,
          "bad read in RleDecoderV2::readByte, ",
          dwio::common::IntDecoder<isSigned>::inputStream_->getName());
      dwio::common::IntDecoder<isSigned>::bufferStart_ =
          static_cast<const char*>(bufferPointer);
      dwio::common::IntDecoder<isSigned>::bufferEnd_ =
          dwio::common::IntDecoder<isSigned>::bufferStart_ + bufferLength;
    }

    unsigned char result = static_cast<unsigned char>(
        *dwio::common::IntDecoder<isSigned>::bufferStart_++);
    return result;
  }

  int64_t readLongBE(uint64_t bsz);
  uint64_t readLongs(
      int64_t* data,
      uint64_t offset,
      uint64_t len,
      uint64_t fb,
      const uint64_t* nulls = nullptr) {
    // SIMD/bulk fast path: no nulls, fb in [1,32], enough values, and the
    // remaining bytes are all in the current contiguous buffer (no refill
    // needed). This eliminates the per-byte readByte() overhead (buffer-boundary
    // check + call) by reading bytes directly from bufferStart_. The bit
    // extraction itself stays scalar (MSB-first, matching ORC RLEv2), which is
    // correct for all fb in [1,32]; the win is from removing readByte() churn.
    // The buffer-availability check is done inline here (cheap) so that small
    // runs with insufficient buffer skip readLongsSimd entirely with no
    // inlined overhead, avoiding regression on queries with small RLEv2 runs.
    if (nulls == nullptr && fb >= 1 && fb <= 32 && len >= 8) {
      const uint64_t bitsFromBuffer =
          (len * fb > bitsLeft_) ? (len * fb - bitsLeft_) : 0;
      const uint64_t neededBytes = (bitsFromBuffer + 7) / 8 + 8;
      if (static_cast<uint64_t>(this->bufferEnd_ - this->bufferStart_) >=
          neededBytes) {
        readLongsSimd(data, offset, len, fb);
        return len;
      }
    }

    uint64_t ret = 0;

    // TODO: unroll to improve performance
    for (uint64_t i = offset; i < (offset + len); i++) {
      // skip null positions
      if (nulls && bits::isBitNull(nulls, i)) {
        continue;
      }
      uint64_t result = 0;
      uint64_t bitsLeftToRead = fb;
      while (bitsLeftToRead > bitsLeft_) {
        result <<= bitsLeft_;
        result |= curByte_ & ((1 << bitsLeft_) - 1);
        bitsLeftToRead -= bitsLeft_;
        curByte_ = readByte();
        bitsLeft_ = 8;
      }

      // handle the left over bits
      if (bitsLeftToRead > 0) {
        result <<= bitsLeftToRead;
        bitsLeft_ -= static_cast<uint32_t>(bitsLeftToRead);
        result |= (curByte_ >> bitsLeft_) & ((1 << bitsLeftToRead) - 1);
      }
      data[i] = static_cast<int64_t>(result);
      ++ret;
    }

    return ret;
  }

  /// Bulk bit-unpack fast path for readLongs. Reads 'len' values of 'fb' bits
  /// each (fb in [1,32]) directly from bufferStart_, avoiding the per-byte
  /// readByte() overhead. Requires nulls==nullptr (handled by caller) and that
  /// the whole bit range fits in the current contiguous buffer window
  /// [bufferStart_, bufferEnd_) with a few bytes of padding for safe wide reads.
  ///
  /// Bit order is ORC RLEv2 MSB-first: within each byte the high bits are
  /// consumed first (curByte_ >> bitsLeft_). The implementation drains any
  /// residual sub-byte alignment (bitsLeft_ != 0) one value at a time, then
  /// reads aligned whole bytes in a tight loop.
  ///
  /// Bulk bit-unpack for readLongs. Caller guarantees: nulls==nullptr,
  /// fb in [1,32], len>=8, and the whole bit range fits in the current
  /// contiguous buffer window with padding (checked inline in readLongs).
  /// Reads bytes directly from bufferStart_ instead of per-byte readByte(),
  /// with the same MSB-first bit extraction. Updates curByte_/bitsLeft_/
  /// bufferStart_ cursor.
  void readLongsSimd(int64_t* data, uint64_t offset, uint64_t len, uint64_t fb) {
    const uint64_t mask = (1ULL << fb) - 1;

    // The byte buffer we read from. curByte_/bitsLeft_ hold residual bits from
    // *(bufferStart_ - 1); once those are drained the stream continues at
    // *bufferStart_. We process values one at a time but pull bytes straight
    // from 'p' (no readByte() boundary checks), which is the main win.
    const uint8_t* p =
        reinterpret_cast<const uint8_t*>(this->bufferStart_);
    uint32_t bitsLeft = bitsLeft_;
    uint32_t curByte = curByte_;

    for (uint64_t i = 0; i < len; ++i) {
      uint64_t result = 0;
      uint64_t bitsLeftToRead = fb;
      while (bitsLeftToRead > bitsLeft) {
        result <<= bitsLeft;
        result |= curByte & ((1u << bitsLeft) - 1);
        bitsLeftToRead -= bitsLeft;
        curByte = *p++;
        bitsLeft = 8;
      }
      if (bitsLeftToRead > 0) {
        result <<= bitsLeftToRead;
        bitsLeft -= static_cast<uint32_t>(bitsLeftToRead);
        result |= (curByte >> bitsLeft) & ((1u << bitsLeftToRead) - 1);
      }
      data[offset + i] = static_cast<int64_t>(result & mask);
    }

    // Commit cursor state. 'p' advanced past the bytes consumed from
    // bufferStart_; the last partially-consumed byte (if any) is at *(p - 1)
    // and its remaining low bits are 'bitsLeft' in 'curByte'.
    this->bufferStart_ = reinterpret_cast<const char*>(p);
    bitsLeft_ = bitsLeft;
    curByte_ = curByte;
  }

 private:
  // Used by PATCHED_BASE
  void adjustGapAndPatch() {
    curGap_ = static_cast<uint64_t>(unpackedPatch_[patchIdx_]) >> patchBitSize_;
    curPatch_ = unpackedPatch_[patchIdx_] & patchMask_;
    actualGap_ = 0;

    // special case: gap is >255 then patch value will be 0.
    // if gap is <=255 then patch value cannot be 0
    while (curGap_ == 255 && curPatch_ == 0) {
      actualGap_ += 255;
      ++patchIdx_;
      curGap_ =
          static_cast<uint64_t>(unpackedPatch_[patchIdx_]) >> patchBitSize_;
      curPatch_ = unpackedPatch_[patchIdx_] & patchMask_;
    }
    // add the left over gap
    actualGap_ += curGap_;
  }

  void resetReadLongs() {
    bitsLeft_ = 0;
    curByte_ = 0;
  }

  void resetRun() {
    resetReadLongs();
    bitSize_ = 0;
    firstByte_ = readByte();
    type_ = static_cast<EncodingType>((firstByte_ >> 6) & 0x03);
  }

  uint64_t nextShortRepeats(
      int64_t* data,
      uint64_t offset,
      uint64_t numValues,
      const uint64_t* nulls);
  uint64_t nextDirect(
      int64_t* data,
      uint64_t offset,
      uint64_t numValues,
      const uint64_t* nulls);
  uint64_t nextPatched(
      int64_t* data,
      uint64_t offset,
      uint64_t numValues,
      const uint64_t* nulls);
  uint64_t nextDelta(
      int64_t* data,
      uint64_t offset,
      uint64_t numValues,
      const uint64_t* nulls);

  int64_t readValue();

  void doNext(
      int64_t* const data,
      const uint64_t numValues,
      const uint64_t* const nulls);

  // Encoding-aware skip of `numValues` non-null values from the current run
  // position. Advances runRead_ and the underlying bit stream / unpacked
  // index exactly as doNext would, but without materializing values into an
  // output buffer. Called by skipPending; numValues is already the non-null
  // count (callers convert nulls to non-null counts before reaching here).
  void skipValues(uint64_t numValues);

  unsigned char firstByte_;
  uint64_t runLength_;
  uint64_t runRead_;
  int64_t deltaBase_; // Used by DELTA
  uint64_t byteSize_; // Used by SHORT_REPEAT and PATCHED_BASE
  int64_t firstValue_; // Used by SHORT_REPEAT and DELTA
  int64_t prevValue_; // Used by DELTA
  uint32_t bitSize_; // Used by DIRECT, PATCHED_BASE and DELTA
  uint32_t bitsLeft_; // Used by anything that uses readLongs
  uint32_t curByte_; // Used by anything that uses readLongs
  uint32_t patchBitSize_; // Used by PATCHED_BASE
  uint64_t unpackedIdx_; // Used by PATCHED_BASE
  uint64_t patchIdx_; // Used by PATCHED_BASE
  int64_t base_; // Used by PATCHED_BASE
  uint64_t curGap_; // Used by PATCHED_BASE
  int64_t curPatch_; // Used by PATCHED_BASE
  int64_t patchMask_; // Used by PATCHED_BASE
  int64_t actualGap_; // Used by PATCHED_BASE
  EncodingType type_;
  dwio::common::DataBuffer<int64_t> unpacked_; // Used by PATCHED_BASE
  dwio::common::DataBuffer<int64_t> unpackedPatch_; // Used by PATCHED_BASE
  // Scratch buffer for the batched decode loop (batchReadLoop) when the
  // visitor's value type is narrower than int64_t (int16/int32): next* writes
  // int64_t here, then values are narrowed into the visitor's buffer.
  dwio::common::DataBuffer<int64_t> bulkScratch_;
};

} // namespace facebook::velox::dwrf
