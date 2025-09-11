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
namespace facebook::velox::bits {

#if defined(__aarch64__)
extern "C" {
  extern unsigned int crc32_x4(const unsigned char *buf, size_t len, unsigned int seed);
}
#endif

// A boost compatible CRC32 calculator.
class Crc32 {
 public:
  void process_bytes(const void* data, int32_t size) {
    checksum_ =
#if defined(__aarch64__)
        crc32_x4(reinterpret_cast<const uint8_t*>(data), size, checksum_);
#else
        folly::crc32(reinterpret_cast<const uint8_t*>(data), size, checksum_);
#endif
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
