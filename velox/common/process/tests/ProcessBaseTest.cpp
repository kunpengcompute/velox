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

#include "velox/common/process/ProcessBase.h"

#include <gtest/gtest.h>

namespace facebook::velox::process {
namespace {

// hasSimd() is the architecture-neutral gate for xsimd-based bulk decode fast
// paths. On aarch64 NEON is the baseline and must always be reported as
// available; on x86 it must agree with hasAvx2() so existing behavior is
// unchanged.
TEST(ProcessBaseTest, hasSimdMatchesArch) {
#if defined(__aarch64__)
  EXPECT_TRUE(hasSimd()) << "NEON baseline must be available on aarch64";
#else
  // On x86 hasSimd() delegates to hasAvx2(); the two must agree so that
  // swapping the read-path gate from hasAvx2() to hasSimd() is a no-op there.
  EXPECT_EQ(hasSimd(), hasAvx2());
#endif
}

} // namespace
} // namespace facebook::velox::process
