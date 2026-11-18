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

#include <fmt/format.h>
#include <chrono>
#include "velox/common/process/ProcessBase.h"

namespace facebook::velox {

// Tracks call count and elapsed CPU and wall time for a repeating operation.
struct CpuWallTiming {
  uint64_t count = 0;
  uint64_t wallNanos = 0;
  uint64_t cpuNanos = 0;

  void add(const CpuWallTiming& other) {
    count += other.count;
    cpuNanos += other.cpuNanos;
    wallNanos += other.wallNanos;
  }

  void clear() {
    count = 0;
    wallNanos = 0;
    cpuNanos = 0;
  }

  std::string toString() const {
    return fmt::format(
        "count: {}, wallNanos: {}, cpuNanos: {}", count, wallNanos, cpuNanos);
  }
};

// Adds elapsed CPU and wall time to a CpuWallTiming.
class CpuWallTimer {
 public:
  explicit CpuWallTimer(CpuWallTiming& timing);
  ~CpuWallTimer();

 private:
  uint64_t cpuTimeStart_;
#if defined(__aarch64__)
  uint64_t wallTimeStart_;
#else
  std::chrono::steady_clock::time_point wallTimeStart_;
#endif
  CpuWallTiming& timing_;
};

/// Keeps track of elapsed CPU and wall time from construction time.
class DeltaCpuWallTimeStopWatch {
 public:
  explicit DeltaCpuWallTimeStopWatch()
#if defined(__aarch64__)
      : wallTimeStart_(process::getTimeArm()),
#else
      : wallTimeStart_(std::chrono::steady_clock::now()),
#endif
        cpuTimeStart_(process::threadCpuNanos()) {}

  CpuWallTiming elapsed() const {
    // NOTE: End the cpu-time timing first, and then end the wall-time timing,
    // so as to avoid the counter-intuitive phenomenon that the final calculated
    // cpu-time is slightly larger than the wall-time.
    uint64_t cpuTimeDuration = process::threadCpuNanos() - cpuTimeStart_;
#if defined(__aarch64__)
    uint64_t currentClock;
    uint64_t currentFrq;
    asm volatile("mrs %0, cntvct_el0" : "=r" (currentClock));
    asm volatile("mrs %0, cntfrq_el0" : "=r" (currentFrq));
    uint64_t wallTimeDuration = ((currentClock * 1000000000ULL ) / currentFrq) - wallTimeStart_;
#else
     uint64_t wallTimeDuration =
         std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now() - wallTimeStart_)
             .count();
#endif
    return CpuWallTiming{1, wallTimeDuration, cpuTimeDuration};
  }

 private:
  // NOTE: Put `wallTimeStart_` before `cpuTimeStart_`, so that wall-time starts
  // counting earlier than cpu-time.
#if defined(__aarch64__)
  const uint64_t wallTimeStart_;
#else
   const std::chrono::steady_clock::time_point wallTimeStart_;
#endif
  const uint64_t cpuTimeStart_;
};

/// Composes delta CpuWallTiming upon destruction and passes it to the user
/// callback, where it can be added to the user's CpuWallTiming using
/// CpuWallTiming::add().
template <typename F>
class DeltaCpuWallTimer {
 public:
  explicit DeltaCpuWallTimer(F&& func) : func_(std::move(func)) {}

  ~DeltaCpuWallTimer() {
    func_(timer_.elapsed());
  }

 private:
  DeltaCpuWallTimeStopWatch timer_;
  F func_;
};

} // namespace facebook::velox
