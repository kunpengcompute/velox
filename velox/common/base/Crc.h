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

#include <folly/hash/Checksum.h>

#if defined(__aarch64__) && defined(__linux__)
#include <arm_acle.h>
#include <asm/hwcap.h>
#include <sys/auxv.h>
#include <cstring>
#endif

namespace facebook::velox::bits {

#if defined(__aarch64__) && defined(__linux__)
namespace detail {

inline bool aarch64HasCrc32() {
  static const bool kHasCrc32 = (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0;
  return kHasCrc32;
}

// Hardware CRC-32 (IEEE reflected, poly 0xEDB88320) over the running state.
// Same state semantics as folly::crc32: 'crc' is the non-inverted internal
// state and the caller inverts the final result. The armv8 CRC32 extension
// implements exactly this polynomial, so results are bit-identical to the
// folly/boost table-driven implementation.
__attribute__((target("+crc"))) inline uint32_t crc32Hw(
    const uint8_t* data,
    size_t size,
    uint32_t crc) {
  while (size >= 8) {
    uint64_t v;
    std::memcpy(&v, data, 8);
    crc = __crc32d(crc, v);
    data += 8;
    size -= 8;
  }
  if (size >= 4) {
    uint32_t v;
    std::memcpy(&v, data, 4);
    crc = __crc32w(crc, v);
    data += 4;
    size -= 4;
  }
  if (size >= 2) {
    uint16_t v;
    std::memcpy(&v, data, 2);
    crc = __crc32h(crc, v);
    data += 2;
    size -= 2;
  }
  if (size) {
    crc = __crc32b(crc, *data);
  }
  return crc;
}

} // namespace detail
#endif

// A boost compatible CRC32 calculator.
class Crc32 {
 public:
  void process_bytes(const void* data, int32_t size) {
#if defined(__aarch64__) && defined(__linux__)
    // folly::crc32 falls back to the byte-at-a-time software table on this
    // platform (visible as crc32_sw in profiles); use the armv8 CRC32
    // instructions when the CPU has them.
    if (detail::aarch64HasCrc32()) {
      checksum_ = detail::crc32Hw(
          reinterpret_cast<const uint8_t*>(data), size, checksum_);
      return;
    }
#endif
    checksum_ =
        folly::crc32(reinterpret_cast<const uint8_t*>(data), size, checksum_);
  }

  uint32_t checksum() const {
    return ~checksum_;
  }

  void reset() {
    checksum_ = ~0U;
  }

 private:
  uint32_t checksum_{~0U};
};

} // namespace facebook::velox::bits
