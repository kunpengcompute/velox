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

#include "velox/common/time/CpuWallTimer.h"

namespace facebook::velox {

CpuWallTimer::CpuWallTimer(CpuWallTiming& timing) : timing_(timing) {
  ++timing_.count;
  cpuTimeStart_ = process::threadCpuNanos();
#if defined(__aarch64__)
  uint64_t currentClock;
  uint64_t currentFrq;
  asm volatile("mrs %0, cntvct_el0" : "=r" (currentClock));
  asm volatile("mrs %0, cntfrq_el0" : "=r" (currentFrq));
  wallTimeStart_ = ((currentClock * 1000000000ULL ) / currentFrq);
#else
  wallTimeStart_ = std::chrono::steady_clock::now();
#endif
}

CpuWallTimer::~CpuWallTimer() {
  timing_.cpuNanos += process::threadCpuNanos() - cpuTimeStart_;
#if defined(__aarch64__)
  uint64_t currentClock;
  uint64_t currentFrq;
  asm volatile("mrs %0, cntvct_el0" : "=r" (currentClock));
  asm volatile("mrs %0, cntfrq_el0" : "=r" (currentFrq));
  uint64_t duration = ((currentClock * 1000000000ULL ) / currentFrq) - wallTimeStart_;
  timing_.wallNanos += duration;
#else
  auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - wallTimeStart_);
  timing_.wallNanos += duration.count();
#endif
}

} // namespace facebook::velox
