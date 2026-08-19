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

#include "velox/dwio/dwrf/common/RLEv2.h"
#include "velox/dwio/common/SeekableInputStream.h"
#include "velox/dwio/dwrf/common/Common.h"

#include <arm_sve.h>

namespace facebook::velox::dwrf {

using memory::MemoryPool;

// Build an SVE predicate where lane i is active iff nulls bit `pos+i` is set
// (i.e. the position is NOT null / valid). The bitmap is bit-packed MSB-first
// within each uint64_t word. pgTail gates which lanes participate (use
// svptrue_b64 for a full vector, or svwhilelt for a partial tail vector).
// Requires nulls to cover at least floor((pos+cnt)/64)+1 words.
static inline svbool_t nullBitsToPredicate(const uint64_t* nulls,
                                           uint64_t pos,
                                           svbool_t pgTail) {
  const uint64_t cnt = svcntd();
  const uint64_t wordIdx = pos >> 6;
  const uint64_t bitOff = pos & 63;
  uint64_t bits = nulls[wordIdx] >> bitOff;
  if (bitOff + cnt > 64) {
    bits |= nulls[wordIdx + 1] << (64 - bitOff);
  }
  svuint64_t vbits = svdup_u64(bits);
  svuint64_t vidx = svindex_u64(0, 1);
  svuint64_t vbit = svand_n_u64_x(
      pgTail, svlsr_u64_x(pgTail, vbits, vidx), 1);
  return svcmpne_n_u64(pgTail, vbit, 0);
}

struct FixedBitSizes {
  enum FBS {
    ONE = 0,
    TWO,
    THREE,
    FOUR,
    FIVE,
    SIX,
    SEVEN,
    EIGHT,
    NINE,
    TEN,
    ELEVEN,
    TWELVE,
    THIRTEEN,
    FOURTEEN,
    FIFTEEN,
    SIXTEEN,
    SEVENTEEN,
    EIGHTEEN,
    NINETEEN,
    TWENTY,
    TWENTYONE,
    TWENTYTWO,
    TWENTYTHREE,
    TWENTYFOUR,
    TWENTYSIX,
    TWENTYEIGHT,
    THIRTY,
    THIRTYTWO,
    FORTY,
    FORTYEIGHT,
    FIFTYSIX,
    SIXTYFOUR
  };
};

inline uint32_t decodeBitWidth(uint32_t n) {
  if (n <= FixedBitSizes::TWENTYFOUR) {
    return n + 1;
  } else if (n == FixedBitSizes::TWENTYSIX) {
    return 26;
  } else if (n == FixedBitSizes::TWENTYEIGHT) {
    return 28;
  } else if (n == FixedBitSizes::THIRTY) {
    return 30;
  } else if (n == FixedBitSizes::THIRTYTWO) {
    return 32;
  } else if (n == FixedBitSizes::FORTY) {
    return 40;
  } else if (n == FixedBitSizes::FORTYEIGHT) {
    return 48;
  } else if (n == FixedBitSizes::FIFTYSIX) {
    return 56;
  } else {
    return 64;
  }
}

inline uint32_t getClosestFixedBits(uint32_t n) {
  if (n == 0) {
    return 1;
  }

  if (n >= 1 && n <= 24) {
    return n;
  } else if (n > 24 && n <= 26) {
    return 26;
  } else if (n > 26 && n <= 28) {
    return 28;
  } else if (n > 28 && n <= 30) {
    return 30;
  } else if (n > 30 && n <= 32) {
    return 32;
  } else if (n > 32 && n <= 40) {
    return 40;
  } else if (n > 40 && n <= 48) {
    return 48;
  } else if (n > 48 && n <= 56) {
    return 56;
  } else {
    return 64;
  }
}

template <bool isSigned>
int64_t RleDecoderV2<isSigned>::readLongBE(uint64_t bsz) {
  int64_t ret = 0, val;
  uint64_t n = bsz;
  while (n > 0) {
    --n;
    val = readByte();
    ret |= (val << (n * 8));
  }
  return ret;
}

template <bool isSigned>
RleDecoderV2<isSigned>::RleDecoderV2(
    std::unique_ptr<dwio::common::SeekableInputStream> input,
    MemoryPool& pool)
    : dwio::common::IntDecoder<isSigned>{std::move(input), false, 0},
      firstByte_(0),
      runLength_(0),
      runRead_(0),
      deltaBase_(0),
      byteSize_(0),
      firstValue_(0),
      prevValue_(0),
      bitSize_(0),
      bitsLeft_(0),
      curByte_(0),
      patchBitSize_(0),
      unpackedIdx_(0),
      patchIdx_(0),
      base_(0),
      curGap_(0),
      curPatch_(0),
      patchMask_(0),
      actualGap_(0),
      unpacked_(pool, 0),
      unpackedPatch_(pool, 0),
      bulkScratch_(pool, 0) {}

template RleDecoderV2<true>::RleDecoderV2(
    std::unique_ptr<dwio::common::SeekableInputStream> input,
    MemoryPool& pool);
template RleDecoderV2<false>::RleDecoderV2(
    std::unique_ptr<dwio::common::SeekableInputStream> input,
    MemoryPool& pool);

template <bool isSigned>
void RleDecoderV2<isSigned>::seekToRowGroup(
    dwio::common::PositionProvider& location) {
  // move the input stream
  dwio::common::IntDecoder<isSigned>::inputStream_->seekToPosition(location);
  // clear state
  dwio::common::IntDecoder<isSigned>::bufferStart_ = nullptr;
  dwio::common::IntDecoder<isSigned>::bufferEnd_ = nullptr;
  runRead_ = 0;
  runLength_ = 0;
  // skip ahead the given number of records
  this->pendingSkip_ = location.next();
}

template void RleDecoderV2<true>::seekToRowGroup(
    dwio::common::PositionProvider& location);
template void RleDecoderV2<false>::seekToRowGroup(
    dwio::common::PositionProvider& location);

template <bool isSigned>
void RleDecoderV2<isSigned>::skipPending() {
  auto numValues = this->pendingSkip_;
  this->pendingSkip_ = 0;
  if (numValues == 0) {
    return;
  }
  skipValues(numValues);
}

template void RleDecoderV2<true>::skipPending();
template void RleDecoderV2<false>::skipPending();

template <bool isSigned>
void RleDecoderV2<isSigned>::skipValues(uint64_t numValues) {
  // Encoding-aware skip: advance runRead_ and the bit stream / unpacked index
  // exactly as doNext would, without materializing values. Mirrors the control
  // flow of doNext (resetRun at run boundaries, dispatch by type_) but each
  // branch advances state arithmetically instead of decoding into a buffer.
  while (numValues > 0) {
    if (runRead_ == runLength_) {
      resetRun();
    }

    switch (type_) {
      case SHORT_REPEAT: {
        // Header decode (same guard as nextShortRepeats).
        if (runRead_ == runLength_) {
          byteSize_ = (firstByte_ >> 3) & 0x07;
          byteSize_ += 1;
          runLength_ = firstByte_ & 0x07;
          runLength_ += RLE_MINIMUM_REPEAT;
          runRead_ = 0;
          firstValue_ = readLongBE(byteSize_);
          if (isSigned) {
            firstValue_ =
                ZigZag::decode<uint64_t>(static_cast<uint64_t>(firstValue_));
          }
        }
        uint64_t nSkip = std::min(runLength_ - runRead_, numValues);
        // Repeated value: no bit stream to advance, just count.
        runRead_ += nSkip;
        numValues -= nSkip;
        break;
      }
      case DIRECT: {
        // Header decode (same guard as nextDirect).
        if (runRead_ == runLength_) {
          unsigned char fbo = (firstByte_ >> 1) & 0x1f;
          bitSize_ = decodeBitWidth(fbo);
          runLength_ = static_cast<uint64_t>(firstByte_ & 0x01) << 8;
          runLength_ |= readByte();
          runLength_ += 1;
          runRead_ = 0;
        }
        uint64_t nSkip = std::min(runLength_ - runRead_, numValues);
        // Advance the MSB-first bit stream by nSkip * bitSize_ bits. This must
        // leave curByte_/bitsLeft_ in the same state as if readLongs had
        // decoded nSkip values one-by-one.
        //   bitsLeft_ = remaining unconsumed bits in curByte_ (counted from LSB)
        //   Consuming fb bits: if bitsLeft_ >= fb, bitsLeft_ -= fb (curByte_
        //   unchanged); else read fresh bytes for the remainder.
        {
          uint64_t totalBits = nSkip * bitSize_;
          if (totalBits <= bitsLeft_) {
            bitsLeft_ -= static_cast<uint32_t>(totalBits);
          } else {
            uint64_t need = totalBits - bitsLeft_; // bits still needed
            uint64_t extraBytes = (need + 7) / 8; // bytes to read
            for (uint64_t b = 0; b < extraBytes; ++b) {
              curByte_ = readByte();
            }
            bitsLeft_ = static_cast<uint32_t>(extraBytes * 8 - need);
            // If bitsLeft_ == 0, curByte_ is fully consumed; readLongs will
            // read a fresh byte on its next value (its inner while loop
            // triggers on bitsLeftToRead > bitsLeft_ == 0). This matches the
            // scalar reader, which leaves bitsLeft_=0 with a stale curByte_.
          }
        }
        runRead_ += nSkip;
        numValues -= nSkip;
        break;
      }
      case PATCHED_BASE: {
        // Header decode + full pre-decode (same as nextPatched).
        if (runRead_ == runLength_) {
          unsigned char fbo = (firstByte_ >> 1) & 0x1f;
          bitSize_ = decodeBitWidth(fbo);
          runLength_ = static_cast<uint64_t>(firstByte_ & 0x01) << 8;
          runLength_ |= readByte();
          runLength_ += 1;
          runRead_ = 0;

          uint64_t thirdByte = readByte();
          byteSize_ = (thirdByte >> 5) & 0x07;
          byteSize_ += 1;

          uint32_t pwo = thirdByte & 0x1f;
          patchBitSize_ = decodeBitWidth(pwo);

          uint64_t fourthByte = readByte();
          uint32_t pgw = (fourthByte >> 5) & 0x07;
          pgw += 1;

          size_t pl = fourthByte & 0x1f;
          VELOX_CHECK_NE(
              pl,
              0,
              "Corrupt PATCHED_BASE encoded data (pl==0)! ",
              dwio::common::IntDecoder<isSigned>::inputStream_->getName());

          base_ = readLongBE(byteSize_);
          int64_t mask = (static_cast<int64_t>(1) << ((byteSize_ * 8) - 1));
          if ((base_ & mask) != 0) {
            base_ = base_ & ~mask;
            base_ = -base_;
          }

          unpacked_.resize(runLength_);
          unpackedIdx_ = 0;
          readLongs(unpacked_.data(), 0, runLength_, bitSize_);
          resetReadLongs();

          unpackedPatch_.resize(pl);
          patchIdx_ = 0;
          VELOX_CHECK_LE(
              (patchBitSize_ + pgw),
              64,
              "Corrupt PATCHED_BASE encoded data (patchBitSize + pgw > 64)! ",
              dwio::common::IntDecoder<isSigned>::inputStream_->getName());
          uint32_t cfb = getClosestFixedBits(patchBitSize_ + pgw);
          readLongs(unpackedPatch_.data(), 0, pl, cfb);
          resetReadLongs();

          patchMask_ = ((static_cast<int64_t>(1) << patchBitSize_) - 1);
          adjustGapAndPatch();
        }
        uint64_t nSkip = std::min(runLength_ - runRead_, numValues);
        // Advance through the pre-decoded run, advancing patch state at each
        // patch position (mirrors nextPatched's per-value loop body).
        for (uint64_t i = 0; i < nSkip; ++i) {
          if (static_cast<int64_t>(unpackedIdx_) != actualGap_) {
            // no patching required
          } else {
            ++patchIdx_;
            if (patchIdx_ < unpackedPatch_.size()) {
              adjustGapAndPatch();
              actualGap_ += unpackedIdx_;
            }
          }
          ++runRead_;
          ++unpackedIdx_;
        }
        numValues -= nSkip;
        break;
      }
      case DELTA: {
        // Header decode (same guard as nextDelta).
        if (runRead_ == runLength_) {
          unsigned char fbo = (firstByte_ >> 1) & 0x1f;
          if (fbo != 0) {
            bitSize_ = decodeBitWidth(fbo);
          } else {
            bitSize_ = 0;
          }
          runLength_ = static_cast<uint64_t>(firstByte_ & 0x01) << 8;
          runLength_ |= readByte();
          ++runLength_;
          runRead_ = deltaBase_ = 0;

          if constexpr (isSigned) {
            firstValue_ = dwio::common::IntDecoder<isSigned>::readVsLong();
          } else {
            firstValue_ = static_cast<int64_t>(
                dwio::common::IntDecoder<isSigned>::readVuLong());
          }
          prevValue_ = firstValue_;
          deltaBase_ = dwio::common::IntDecoder<isSigned>::readVsLong();
        }
        uint64_t nSkip = std::min(runLength_ - runRead_, numValues);

        if (bitSize_ == 0) {
          // Fixed delta: pure arithmetic series. The first value (firstValue_)
          // occupies runRead_==0 with no delta added; each subsequent position
          // adds deltaBase_. Skipping nSkip values from the current runRead_
          // advances prevValue_ accordingly.
          if (runRead_ == 0) {
            if (nSkip == 1) {
              prevValue_ = firstValue_;
            } else {
              prevValue_ =
                  firstValue_ + static_cast<int64_t>(nSkip - 1) * deltaBase_;
            }
          } else {
            prevValue_ += static_cast<int64_t>(nSkip) * deltaBase_;
          }
          runRead_ += nSkip;
        } else {
          // Variable delta: must read and accumulate deltas from the bit
          // stream. Mirror nextDelta's control flow exactly:
          //   - runRead_==0: position 0 is firstValue_ (no delta)
          //   - runRead_<2:  position 1 is firstValue_ + deltaBase_
          //   - runRead_>=2: subsequent positions add/subtract deltas
          //                  unpacked via readLongs
          uint64_t remaining = nSkip;
          if (runRead_ == 0 && remaining > 0) {
            // position 0: firstValue_, prevValue_ already == firstValue_
            ++runRead_;
            --remaining;
          }
          if (runRead_ < 2 && remaining > 0) {
            // position 1: firstValue_ + deltaBase_
            prevValue_ = firstValue_ + deltaBase_;
            ++runRead_;
            --remaining;
          }
          if (remaining > 0) {
            constexpr uint64_t SCRATCH = 256;
            int64_t local[SCRATCH];
            int64_t* scratch = local;
            if (remaining > SCRATCH) {
              if (unpacked_.size() < remaining) {
                unpacked_.resize(remaining);
              }
              scratch = unpacked_.data();
            }
            readLongs(scratch, 0, remaining, bitSize_, nullptr);
            if (deltaBase_ < 0) {
              for (uint64_t i = 0; i < remaining; ++i) {
                prevValue_ -= scratch[i];
              }
            } else {
              for (uint64_t i = 0; i < remaining; ++i) {
                prevValue_ += scratch[i];
              }
            }
            runRead_ += remaining;
          }
        }
        numValues -= nSkip;
        break;
      }
      default:
        VELOX_FAIL("unknown encoding: {}", static_cast<int>(type_));
    }
  }
}

template void RleDecoderV2<true>::skipValues(uint64_t numValues);
template void RleDecoderV2<false>::skipValues(uint64_t numValues);

template <bool isSigned>
void RleDecoderV2<isSigned>::next(
    int64_t* const data,
    const uint64_t numValues,
    const uint64_t* const nulls) {
  skipPending();
  doNext(data, numValues, nulls);
}

template <bool isSigned>
void RleDecoderV2<isSigned>::doNext(
    int64_t* const data,
    const uint64_t numValues,
    const uint64_t* const nulls) {
  uint64_t nRead = 0;

  while (nRead < numValues) {
    // Skip any nulls before attempting to read first byte.
    while (nulls && bits::isBitNull(nulls, nRead)) {
      if (++nRead == numValues) {
        return; // ended with null values
      }
    }

    if (runRead_ == runLength_) {
      resetRun();
    }

    uint64_t offset = nRead, length = numValues - nRead;

    switch (type_) {
      case SHORT_REPEAT:
        nRead += nextShortRepeats(data, offset, length, nulls);
        break;
      case DIRECT:
        nRead += nextDirect(data, offset, length, nulls);
        break;
      case PATCHED_BASE:
        nRead += nextPatched(data, offset, length, nulls);
        break;
      case DELTA:
        nRead += nextDelta(data, offset, length, nulls);
        break;
      default:
        VELOX_FAIL("unknown encoding: {}", static_cast<int>(type_));
    }
  }
}

template void RleDecoderV2<true>::doNext(
    int64_t* const data,
    const uint64_t numValues,
    const uint64_t* const nulls);
template void RleDecoderV2<false>::doNext(
    int64_t* const data,
    const uint64_t numValues,
    const uint64_t* const nulls);

template <bool isSigned>
uint64_t RleDecoderV2<isSigned>::nextShortRepeats(
    int64_t* data,
    uint64_t offset,
    uint64_t numValues,
    const uint64_t* nulls) {
  if (runRead_ == runLength_) {
    // extract the number of fixed bytes
    byteSize_ = (firstByte_ >> 3) & 0x07;
    byteSize_ += 1;

    runLength_ = firstByte_ & 0x07;
    // run lengths values are stored only after MIN_REPEAT value is met
    runLength_ += RLE_MINIMUM_REPEAT;
    runRead_ = 0;

    // read the repeated value which is store using fixed bytes
    firstValue_ = readLongBE(byteSize_);

    if (isSigned) {
      firstValue_ =
          ZigZag::decode<uint64_t>(static_cast<uint64_t>(firstValue_));
    }
  }

  uint64_t nRead = std::min(runLength_ - runRead_, numValues);

  if (nulls) {
    // SVE: broadcast firstValue_ and store only to non-null lanes. runRead_
    // advances by the count of non-null positions (matches the scalar loop,
    // which only ++runRead_ for valid lanes).
    const uint64_t end = offset + nRead;
    uint64_t pos = offset;
    const uint64_t cnt = svcntd();
    svint64_t vval = svdup_n_s64(firstValue_);
    for (; pos + cnt <= end; pos += cnt) {
      // bit i==1 in the null bitmap means valid; build a predicate from it.
      svbool_t pgValid = nullBitsToPredicate(nulls, pos, svptrue_b64());
      // Predicated store: null lanes keep their original value.
      svst1_s64(pgValid, reinterpret_cast<int64_t*>(data + pos), vval);
      // runRead_ += number of active (valid) lanes in this vector.
      runRead_ += svcntp_b64(svptrue_b64(), pgValid);
    }
    // Tail: remaining elements that don't fill a full vector.
    for (; pos < end; ++pos) {
      if (!bits::isBitNull(nulls, pos)) {
        data[pos] = firstValue_;
        ++runRead_;
      }
    }
  } else {
    // No nulls: unconditionally fill [offset, offset+nRead) with firstValue_.
    // runRead_ advances by nRead (every position is valid).
    uint64_t pos = offset;
    const uint64_t end = offset + nRead;
    svint64_t vval = svdup_n_s64(firstValue_);
    svbool_t pg = svwhilelt_b64_u64(pos, end);
    do {
      svst1_s64(pg, reinterpret_cast<int64_t*>(data + pos), vval);
      pos += svcntd();
      pg = svwhilelt_b64_u64(pos, end);
    } while (svptest_first(svptrue_b64(), pg));
    runRead_ += nRead;
  }

  return nRead;
}

template uint64_t RleDecoderV2<true>::nextShortRepeats(
    int64_t* data,
    uint64_t offset,
    uint64_t numValues,
    const uint64_t* nulls);
template uint64_t RleDecoderV2<false>::nextShortRepeats(
    int64_t* data,
    uint64_t offset,
    uint64_t numValues,
    const uint64_t* nulls);

static inline void zigzagDecodeVectorizedWithNull(int64_t* data,
                                                  const uint64_t* nulls,
                                                  uint64_t offset,
                                                  uint64_t nRead) {
  const uint64_t end = offset + nRead;
  uint64_t pos = offset;
  const uint64_t cnt = svcntd();

  for (; pos + cnt <= end; pos += cnt) {
    const uint64_t wordIdx = pos >> 6;
    const uint64_t bitOff  = pos & 63;
    uint64_t bits = nulls[wordIdx] >> bitOff;
    if (bitOff + cnt > 64) {
      bits |= nulls[wordIdx + 1] << (64 - bitOff);
    }

    svuint64_t vbits = svdup_u64(bits);
    svuint64_t vidx  = svindex_u64(0, 1);
    svuint64_t vbit  = svand_n_u64_x(svptrue_b64(),
                          svlsr_u64_x(svptrue_b64(), vbits, vidx),
                          1);
    svbool_t pgValid = svcmpne_n_u64(svptrue_b64(), vbit, 0);

    svint64_t  vdata    = svld1_s64(svptrue_b64(),
                                    reinterpret_cast<const int64_t*>(data + pos));
    svuint64_t vu       = svreinterpret_u64_s64(vdata);
    svuint64_t vshifted = svlsr_n_u64_x(svptrue_b64(), vu, 1);
    svuint64_t vlow     = svand_n_u64_x(svptrue_b64(), vu, 1);
    svuint64_t vneg     = svsub_u64_x(svptrue_b64(), svdup_n_u64(0), vlow);
    svuint64_t vres     = sveor_u64_x(svptrue_b64(), vshifted, vneg);
    svint64_t  vout     = svreinterpret_s64_u64(vres);

    svst1_s64(pgValid, reinterpret_cast<int64_t*>(data + pos), vout);
  }

  for (; pos < end; ++pos) {
    if (!bits::isBitNull(nulls, pos)) {
      data[pos] = ZigZag::decode<uint64_t>(static_cast<uint64_t>(data[pos]));
    }
  }
}

// SVE1 inclusive prefix-sum (scan) of a signed 64-bit vector: result[i] =
// x[0]+x[1]+...+x[i]. SVE has no native scan instruction, and svext requires
// a *compile-time constant* index, so we dispatch over the common vector
// lengths (2/4/8/16) and expand the Hillis-Steele scan with constant strides.
//
// svext_s64(zero, v, imm) concatenates zero:v and takes VL lanes starting at
// element imm of zero, i.e. it shifts v left by (VL - imm) lanes with zero
// fill. To shift left by s lanes, pass imm = VL - s. Hillis-Steele needs the
// power-of-two shifts s = 1, 2, ..., VL/2 (order irrelevant: each lane i then
// accumulates x[i-t] exactly once for every t in [0, i], since every t is a
// unique subset sum of the shifts).
//
// The active predicate pgAll gates which lanes participate; inactive lanes
// keep x's value (callers mask the result).
static inline svint64_t inclusiveScanAddS64(svint64_t x, svbool_t pgAll) {
  svint64_t sum = x;
  switch (svcntd()) {
    case 16:
      // shifts 1, 2, 4, 8 -> imm 15, 14, 12, 8
      sum = svadd_s64_x(pgAll, sum, svext_s64(svdup_n_s64(0), sum, 15));
      sum = svadd_s64_x(pgAll, sum, svext_s64(svdup_n_s64(0), sum, 14));
      sum = svadd_s64_x(pgAll, sum, svext_s64(svdup_n_s64(0), sum, 12));
      sum = svadd_s64_x(pgAll, sum, svext_s64(svdup_n_s64(0), sum, 8));
      break;
    case 8:
      // shifts 1, 2, 4 -> imm 7, 6, 4
      sum = svadd_s64_x(pgAll, sum, svext_s64(svdup_n_s64(0), sum, 7));
      sum = svadd_s64_x(pgAll, sum, svext_s64(svdup_n_s64(0), sum, 6));
      sum = svadd_s64_x(pgAll, sum, svext_s64(svdup_n_s64(0), sum, 4));
      break;
    case 4:
      // shifts 1, 2 -> imm 3, 2
      sum = svadd_s64_x(pgAll, sum, svext_s64(svdup_n_s64(0), sum, 3));
      sum = svadd_s64_x(pgAll, sum, svext_s64(svdup_n_s64(0), sum, 2));
      break;
    case 2:
      // shift 1 -> imm 1
      sum = svadd_s64_x(pgAll, sum, svext_s64(svdup_n_s64(0), sum, 1));
      break;
    default: {
      // Fallback for unusual lengths: scalar per-lane accumulation. Correct
      // for any VLA length, just not vectorized. 32 covers the largest SVE
      // register width in practice (2048-bit = 32 x int64).
      const uint64_t cnt = svcntd();
      int64_t carry = 0;
      int64_t tmp[32];
      svst1_s64(svptrue_b64(), tmp, x);
      for (uint64_t i = 0; i < cnt; ++i) {
        carry += tmp[i];
        tmp[i] = carry;
      }
      sum = svld1_s64(svptrue_b64(), tmp);
      break;
    }
  }
  return sum;
}

template <bool isSigned>
uint64_t RleDecoderV2<isSigned>::nextDirect(
    int64_t* data,
    uint64_t offset,
    uint64_t numValues,
    const uint64_t* nulls) {
  if (runRead_ == runLength_) {
    // extract the number of fixed bits
    unsigned char fbo = (firstByte_ >> 1) & 0x1f;
    bitSize_ = decodeBitWidth(fbo);

    // extract the run length
    runLength_ = static_cast<uint64_t>(firstByte_ & 0x01) << 8;
    runLength_ |= readByte();
    // runs are one off
    runLength_ += 1;
    runRead_ = 0;
  }

  uint64_t nRead = std::min(runLength_ - runRead_, numValues);

  runRead_ += readLongs(data, offset, nRead, bitSize_, nulls);

  if (isSigned) {
    if (nulls) {
      zigzagDecodeVectorizedWithNull(data, nulls, offset, nRead);
    } else {
      uint64_t pos = offset;
      const uint64_t end = offset + nRead;
      svbool_t pg = svwhilelt_b64_u64(pos, end);
      do {
        svuint64_t v   = svld1_u64(pg, reinterpret_cast<const uint64_t*>(data + pos));
        svuint64_t hi  = svlsr_n_u64_x(pg, v, 1);
        svuint64_t lo  = svand_n_u64_x(pg, v, 1);
        svuint64_t neg = svsub_u64_x(pg, svdup_n_u64(0), lo);
        svuint64_t res = sveor_u64_x(pg, hi, neg);
        svst1_u64(pg, reinterpret_cast<uint64_t*>(data + pos), res);
        pos += svcntd();
        pg = svwhilelt_b64_u64(pos, end);
      } while (svptest_first(svptrue_b64(), pg));
    }
  }

  return nRead;
}

template uint64_t RleDecoderV2<true>::nextDirect(
    int64_t* const data,
    uint64_t offset,
    uint64_t numValues,
    const uint64_t* const nulls);
template uint64_t RleDecoderV2<false>::nextDirect(
    int64_t* const data,
    uint64_t offset,
    uint64_t numValues,
    const uint64_t* const nulls);

template <bool isSigned>
uint64_t RleDecoderV2<isSigned>::nextPatched(
    int64_t* const data,
    uint64_t offset,
    uint64_t numValues,
    const uint64_t* const nulls) {
  if (runRead_ == runLength_) {
    // extract the number of fixed bits
    unsigned char fbo = (firstByte_ >> 1) & 0x1f;
    bitSize_ = decodeBitWidth(fbo);

    // extract the run length
    runLength_ = static_cast<uint64_t>(firstByte_ & 0x01) << 8;
    runLength_ |= readByte();
    // runs are one off
    runLength_ += 1;
    runRead_ = 0;

    // extract the number of bytes occupied by base
    uint64_t thirdByte = readByte();
    byteSize_ = (thirdByte >> 5) & 0x07;
    // base width is one off
    byteSize_ += 1;

    // extract patch width
    uint32_t pwo = thirdByte & 0x1f;
    patchBitSize_ = decodeBitWidth(pwo);

    // read fourth byte and extract patch gap width
    uint64_t fourthByte = readByte();
    uint32_t pgw = (fourthByte >> 5) & 0x07;
    // patch gap width is one off
    pgw += 1;

    // extract the length of the patch list
    size_t pl = fourthByte & 0x1f;
    VELOX_CHECK_NE(
        pl,
        0,
        "Corrupt PATCHED_BASE encoded data (pl==0)! ",
        dwio::common::IntDecoder<isSigned>::inputStream_->getName());

    // read the next base width number of bytes to extract base value
    base_ = readLongBE(byteSize_);
    int64_t mask = (static_cast<int64_t>(1) << ((byteSize_ * 8) - 1));
    // if mask of base value is 1 then base is negative value else positive
    if ((base_ & mask) != 0) {
      base_ = base_ & ~mask;
      base_ = -base_;
    }

    // TODO: something more efficient than resize
    unpacked_.resize(runLength_);
    unpackedIdx_ = 0;
    readLongs(unpacked_.data(), 0, runLength_, bitSize_);
    // any remaining bits are thrown out
    resetReadLongs();

    // TODO: something more efficient than resize
    unpackedPatch_.resize(pl);
    patchIdx_ = 0;
    // TODO: Skip corrupt?
    //    if ((patchBitSize + pgw) > 64 && !skipCorrupt) {
    VELOX_CHECK_LE(
        (patchBitSize_ + pgw),
        64,
        "Corrupt PATCHED_BASE encoded data (patchBitSize + pgw > 64)! ",
        dwio::common::IntDecoder<isSigned>::inputStream_->getName());
    uint32_t cfb = getClosestFixedBits(patchBitSize_ + pgw);
    readLongs(unpackedPatch_.data(), 0, pl, cfb);
    // any remaining bits are thrown out
    resetReadLongs();

    // apply the patch directly when decoding the packed data
    patchMask_ = ((static_cast<int64_t>(1) << patchBitSize_) - 1);

    adjustGapAndPatch();
  }

  uint64_t nRead = std::min(runLength_ - runRead_, numValues);

  for (uint64_t pos = offset; pos < offset + nRead; ++pos) {
    // skip null positions
    if (nulls && bits::isBitNull(nulls, pos)) {
      continue;
    }
    if (static_cast<int64_t>(unpackedIdx_) != actualGap_) {
      // no patching required. add base to unpacked value to get final value
      data[pos] = base_ + unpacked_[unpackedIdx_];
    } else {
      // extract the patch value
      int64_t patchedVal = unpacked_[unpackedIdx_] | (curPatch_ << bitSize_);

      // add base to patched value
      data[pos] = base_ + patchedVal;

      // increment the patch to point to next entry in patch list
      ++patchIdx_;

      if (patchIdx_ < unpackedPatch_.size()) {
        adjustGapAndPatch();

        // next gap is relative to the current gap
        actualGap_ += unpackedIdx_;
      }
    }

    ++runRead_;
    ++unpackedIdx_;
  }

  return nRead;
}

template uint64_t RleDecoderV2<true>::nextPatched(
    int64_t* const data,
    uint64_t offset,
    uint64_t numValues,
    const uint64_t* const nulls);

template uint64_t RleDecoderV2<false>::nextPatched(
    int64_t* const data,
    uint64_t offset,
    uint64_t numValues,
    const uint64_t* const nulls);

template <bool isSigned>
uint64_t RleDecoderV2<isSigned>::nextDelta(
    int64_t* const data,
    uint64_t offset,
    uint64_t numValues,
    const uint64_t* const nulls) {
  if (runRead_ == runLength_) {
    // extract the number of fixed bits
    unsigned char fbo = (firstByte_ >> 1) & 0x1f;
    if (fbo != 0) {
      bitSize_ = decodeBitWidth(fbo);
    } else {
      bitSize_ = 0;
    }

    // extract the run length
    runLength_ = static_cast<uint64_t>(firstByte_ & 0x01) << 8;
    runLength_ |= readByte();
    ++runLength_; // account for first value
    runRead_ = deltaBase_ = 0;

    // read the first value stored as vint
    if constexpr (isSigned) {
      firstValue_ = dwio::common::IntDecoder<isSigned>::readVsLong();
    } else {
      firstValue_ = static_cast<int64_t>(
          dwio::common::IntDecoder<isSigned>::readVuLong());
    }

    prevValue_ = firstValue_;

    // read the fixed delta value stored as vint (deltas can be negative even
    // if all number are positive)
    deltaBase_ = dwio::common::IntDecoder<isSigned>::readVsLong();
  }

  uint64_t nRead = std::min(runLength_ - runRead_, numValues);

  uint64_t pos = offset;
  for (; pos < offset + nRead; ++pos) {
    // skip null positions
    if (!nulls || !bits::isBitNull(nulls, pos)) {
      break;
    }
  }
  if (runRead_ == 0 && pos < offset + nRead) {
    data[pos++] = firstValue_;
    ++runRead_;
  }

  if (bitSize_ == 0) {
    // add fixed deltas to adjacent values
    if (nulls == nullptr) {
      // No nulls: pure arithmetic progression. data[pos+k] =
      // prevValue_ + (k+1)*deltaBase_. No cross-lane dependency, so each
      // vector is built from prevValue_ and a (1..cnt)*delta ramp.
      const uint64_t end = offset + nRead;
      const uint64_t cnt = svcntd();
      // vk = {1, 2, 3, ..., cnt} as signed, so vk*deltaBase_ is the per-lane
      // step relative to prevValue_.
      svint64_t vk = svreinterpret_s64_u64(svindex_u64(1, 1));
      svint64_t vdelta = svdup_n_s64(deltaBase_);
      for (; pos + cnt <= end; pos += cnt) {
        svint64_t vstep = svmul_s64_x(svptrue_b64(), vk, vdelta);
        svint64_t vval = svadd_n_s64_x(svptrue_b64(), vstep, prevValue_);
        svst1_s64(svptrue_b64(), reinterpret_cast<int64_t*>(data + pos), vval);
        // All cnt lanes written: newest value = prevValue_ + cnt*deltaBase_.
        prevValue_ += static_cast<int64_t>(cnt) * deltaBase_;
        runRead_ += cnt;
      }
      // Tail: remaining < cnt elements, scalar.
      for (; pos < end; ++pos) {
        prevValue_ = data[pos] = prevValue_ + deltaBase_;
        ++runRead_;
      }
    } else {
      // Nulls present: prevValue_ only advances on non-null positions. The
      // k-th valid lane gets prevValue_ + k*deltaBase_, where k is its rank
      // among valid lanes (1-based, inclusive). Compute that rank via an
      // inclusive scan of the per-lane validity mask.
      const uint64_t end = offset + nRead;
      const uint64_t cnt = svcntd();
      for (; pos + cnt <= end; pos += cnt) {
        svbool_t pgValid = nullBitsToPredicate(nulls, pos, svptrue_b64());
        // vone[i] = 1 if lane i is valid, else 0.
        svint64_t vone =
            svsel_s64(pgValid, svdup_n_s64(1), svdup_n_s64(0));
        // vrank[i] = count of valid lanes in [0..i] (inclusive). Null lanes
        // contribute 0, so they don't advance the rank.
        svint64_t vrank = inclusiveScanAddS64(vone, svptrue_b64());
        // value = prevValue_ + vrank * deltaBase_.
        svint64_t vstep = svmul_n_s64_x(svptrue_b64(), vrank, deltaBase_);
        svint64_t vval = svadd_n_s64_x(svptrue_b64(), vstep, prevValue_);
        // Store only valid lanes; null lanes keep their original value.
        svst1_s64(pgValid, reinterpret_cast<int64_t*>(data + pos), vval);
        // Advance carry by the number of valid lanes in this vector.
        const uint64_t nValid = svcntp_b64(svptrue_b64(), pgValid);
        prevValue_ += static_cast<int64_t>(nValid) * deltaBase_;
        runRead_ += nValid;
      }
      // Tail: remaining elements, scalar.
      for (; pos < end; ++pos) {
        if (bits::isBitNull(nulls, pos)) {
          continue;
        }
        prevValue_ = data[pos] = prevValue_ + deltaBase_;
        ++runRead_;
      }
    }
  } else {
    for (; pos < offset + nRead; ++pos) {
      // skip null positions
      if (!nulls || !bits::isBitNull(nulls, pos)) {
        break;
      }
    }
    if (runRead_ < 2 && pos < offset + nRead) {
      // add delta base and first value
      prevValue_ = data[pos++] = firstValue_ + deltaBase_;
      ++runRead_;
    }

    // write the unpacked values, add it to previous value and store final
    // value to result buffer. if the delta base value is negative then it
    // is a decreasing sequence else an increasing sequence
    uint64_t remaining = (offset + nRead) - pos;
    runRead_ += readLongs(data, pos, remaining, bitSize_, nulls);

    if (deltaBase_ < 0) {
      if (nulls == nullptr) {
        // No nulls: data[pos+k] = prevValue_ - (d[0]+d[1]+...+d[k]). Compute
        // the inclusive sum of deltas once, then subtract from prevValue_.
        const uint64_t end = offset + nRead;
        const uint64_t cnt = svcntd();
        for (; pos + cnt <= end; pos += cnt) {
          svint64_t vd = svld1_s64(
              svptrue_b64(), reinterpret_cast<const int64_t*>(data + pos));
          svint64_t vsum = inclusiveScanAddS64(vd, svptrue_b64());
          svint64_t vval = svsub_s64_x(
              svptrue_b64(), svdup_n_s64(prevValue_), vsum);
          svst1_s64(
              svptrue_b64(), reinterpret_cast<int64_t*>(data + pos), vval);
          // New prevValue_ = prevValue_ - sum(all deltas). Sum the ORIGINAL
          // deltas (vd), not vsum (which is an inclusive scan whose horizontal
          // sum is a weighted sum, not what we want).
          prevValue_ -= svaddv_s64(svptrue_b64(), vd);
        }
        for (; pos < end; ++pos) {
          prevValue_ = data[pos] = prevValue_ - data[pos];
        }
      } else {
        // Nulls present: scan only over valid lanes. Null deltas are forced
        // to 0 so they don't perturb the running sum; only valid lanes are
        // stored back (null lanes keep their original value).
        const uint64_t end = offset + nRead;
        const uint64_t cnt = svcntd();
        for (; pos + cnt <= end; pos += cnt) {
          svbool_t pgValid = nullBitsToPredicate(nulls, pos, svptrue_b64());
          svint64_t vd = svld1_s64(
              svptrue_b64(), reinterpret_cast<const int64_t*>(data + pos));
          // Zero out null-lane deltas so the inclusive scan skips them.
          svint64_t vdClean = svsel_s64(pgValid, vd, svdup_n_s64(0));
          svint64_t vsum = inclusiveScanAddS64(vdClean, svptrue_b64());
          svint64_t vval = svsub_s64_x(
              svptrue_b64(), svdup_n_s64(prevValue_), vsum);
          // Store only valid lanes; null lanes keep their original value.
          svst1_s64(pgValid, reinterpret_cast<int64_t*>(data + pos), vval);
          // New prevValue_ = prevValue_ - sum(valid deltas).
          prevValue_ -= svaddv_s64(pgValid, vd);
        }
        for (; pos < end; ++pos) {
          if (bits::isBitNull(nulls, pos)) {
            continue;
          }
          prevValue_ = data[pos] = prevValue_ - data[pos];
        }
      }
    } else {
      if (nulls == nullptr) {
        // No nulls: data[pos+k] = prevValue_ + (d[0]+d[1]+...+d[k]).
        const uint64_t end = offset + nRead;
        const uint64_t cnt = svcntd();
        for (; pos + cnt <= end; pos += cnt) {
          svint64_t vd = svld1_s64(
              svptrue_b64(), reinterpret_cast<const int64_t*>(data + pos));
          svint64_t vsum = inclusiveScanAddS64(vd, svptrue_b64());
          svint64_t vval = svadd_n_s64_x(svptrue_b64(), vsum, prevValue_);
          svst1_s64(
              svptrue_b64(), reinterpret_cast<int64_t*>(data + pos), vval);
          // New prevValue_ = prevValue_ + sum(all deltas). Sum ORIGINAL deltas.
          prevValue_ += svaddv_s64(svptrue_b64(), vd);
        }
        for (; pos < end; ++pos) {
          prevValue_ = data[pos] = prevValue_ + data[pos];
        }
      } else {
        // Nulls present: same scan-over-valid-lanes scheme as the subtract
        // branch, but adding.
        const uint64_t end = offset + nRead;
        const uint64_t cnt = svcntd();
        for (; pos + cnt <= end; pos += cnt) {
          svbool_t pgValid = nullBitsToPredicate(nulls, pos, svptrue_b64());
          svint64_t vd = svld1_s64(
              svptrue_b64(), reinterpret_cast<const int64_t*>(data + pos));
          svint64_t vdClean = svsel_s64(pgValid, vd, svdup_n_s64(0));
          svint64_t vsum = inclusiveScanAddS64(vdClean, svptrue_b64());
          svint64_t vval = svadd_n_s64_x(svptrue_b64(), vsum, prevValue_);
          svst1_s64(pgValid, reinterpret_cast<int64_t*>(data + pos), vval);
          // New prevValue_ = prevValue_ + sum(valid deltas).
          prevValue_ += svaddv_s64(pgValid, vd);
        }
        for (; pos < end; ++pos) {
          if (bits::isBitNull(nulls, pos)) {
            continue;
          }
          prevValue_ = data[pos] = prevValue_ + data[pos];
        }
      }
    }
  }
  return nRead;
}

template uint64_t RleDecoderV2<true>::nextDelta(
    int64_t* const data,
    uint64_t offset,
    uint64_t numValues,
    const uint64_t* const nulls);

template uint64_t RleDecoderV2<false>::nextDelta(
    int64_t* const data,
    uint64_t offset,
    uint64_t numValues,
    const uint64_t* const nulls);

template <bool isSigned>
int64_t RleDecoderV2<isSigned>::readValue() {
  if (runRead_ == runLength_) {
    resetRun();
  }

  uint64_t nRead = 0;
  int64_t value = 0;
  switch (type_) {
    case SHORT_REPEAT:
      nRead = nextShortRepeats(&value, 0, 1, nullptr);
      break;
    case DIRECT:
      nRead = nextDirect(&value, 0, 1, nullptr);
      break;
    case PATCHED_BASE:
      nRead = nextPatched(&value, 0, 1, nullptr);
      break;
    case DELTA:
      nRead = nextDelta(&value, 0, 1, nullptr);
      break;
    default:
      VELOX_FAIL("unknown encoding: {}", static_cast<int>(type_));
  }
  VELOX_CHECK_EQ(nRead, (uint64_t)1);
  return value;
}

template int64_t RleDecoderV2<true>::readValue();

template int64_t RleDecoderV2<false>::readValue();

} // namespace facebook::velox::dwrf
