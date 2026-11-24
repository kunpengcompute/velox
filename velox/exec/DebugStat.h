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
#include <folly/Synchronized.h>

// #define DEBUG_STAT_ENABLED

#ifdef DEBUG_STAT_ENABLED
#include <glog/logging.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <memory>
#include <ostream>
#include <sstream>
#include <utility>
#include <vector>
namespace facebook {
namespace velox {
namespace debug {

struct DebugStat {
 public:
#define LK_DEBUG_STAT_GENERATOR(K)    \
  K(LifeTime)                         \
  K(OperatorAddInputCalls)            \
  K(OperatorGetOutputCalls)           \
  K(InitKeysCalls)                    \
  K(InitKeysCounts)                   \
  K(ExtractKeysCalls)                 \
  K(ExtractKeysCounts)                \
  K(ExtractStringKeysCalls)           \
  K(ExtendMemoryCalls)                \
  K(ExtendMemoryUsed)                 \
  K(ExtendMemoryInit)                 \
  K(ExtendMemoryGrow)                 \
  K(EmplaceBatchCalls)                \
  K(EmplaceCalls)                     \
  K(EmplaceCounts)                    \
  K(ExpandDirectlyCalls)              \
  K(ExpandIterativelyCalls)           \
  K(HashCallsFromEmplaceImpl)         \
  K(SpeculativeHashCalls)             \
  K(SpeculativeEmplaceBatchCalls)     \
  K(SpeculativeStoreKeysCalls)        \
  K(SpeculativeCompareKeyCalls)       \
  K(SpeculativeCompareFailedNum)      \
  K(ResetEmplaceContextCalls)         \
  K(ResetRehashContextCalls)          \
  K(HashCallsFromResetEmplaceContext) \
  K(HashCallsFromResetRehashContext)  \
  K(NumericHashCalls)                 \
  K(NumericEqualsCalls)               \
  K(FixedBufHashCalls)                \
  K(FixedBufEqualsCalls)              \
  K(CompactKeyHashCalls)              \
  K(CompactKeyEqualsCalls)            \
  K(BuildAddInputCalls)               \
  K(BuildFinishCalls)                 \
  K(ProbeAddInputCalls)               \
  K(ProbeGetOutputCalls)              \
  K(LookupValueIdsCalls)              \
  K(ProbeJoinProbeCalls)              \
  K(ProbeJoinProbeArrayCalls)         \
  K(ProbeJoinProbeTaperCalls)         \
  K(ProbeJoinProbeNormalizedCalls)    \
  K(ProbeJoinProbeHashCalls)          \
  K(BuildDropDuplicatesCalls)         \
  K(BuildComputeValueIdsCalls)        \
  K(BuildStoreCalls)                  \
  K(ProbeListJoinResultsCalls)        \
  K(ProbeEvalFilterCalls)             \
  K(ProbeFillOutputCalls)             \
  K(NewRow)                           \
  K(InsertJoin)                       \
  K(hashRows)

  enum StateKey : size_t {
#define LK_DEBUG_STAT_KEY(name) name,
    LK_DEBUG_STAT_GENERATOR(LK_DEBUG_STAT_KEY) kStateLength,
#undef LK_DEBUG_STAT_KEY
  };
  using Count = int64_t;
  using Duration = std::chrono::steady_clock::duration;
  using TimePoint = std::chrono::steady_clock::time_point;

  struct DebugStatTimer {
   public:
    DebugStatTimer(
        DebugStat* stat,
        StateKey key,
        bool startOnCreate = false,
        bool stopOnDestroy = false);

    ~DebugStatTimer();

    DebugStatTimer(const DebugStatTimer&) = delete;
    DebugStatTimer& operator=(const DebugStatTimer&) = delete;
    DebugStatTimer(DebugStatTimer&&) = default;
    DebugStatTimer& operator=(DebugStatTimer&&) = default;

    void setStopOnDestroy(bool stopOnDestroy = true) {
      stopOnDestroy_ = stopOnDestroy;
    }
    void stop() {
      if (not started_) {
        return;
      }
      auto stop = std::chrono::steady_clock::now();
      stat_->clockNowCount_++;
      stat_->record(key_, start_, stop);
      started_ = false;
    }

   private:
    DebugStat* stat_;
    StateKey key_;
    bool stopOnDestroy_;
    bool started_{false};
    TimePoint start_;
  };

  struct DebugStatExposer {
   public:
    explicit DebugStatExposer(DebugStat* stat);
    ~DebugStatExposer();
    DebugStatExposer(const DebugStatExposer&) = delete;
    DebugStatExposer& operator=(const DebugStatExposer&) = delete;
    DebugStatExposer(DebugStatExposer&&) = delete;
    DebugStatExposer& operator=(DebugStatExposer&&) = delete;

   private:
    DebugStat* old_;
  };

  DebugStat() : DebugStat("") {}

  explicit DebugStat(std::string title)
      : title_(std::move(title)),
        counts_(static_cast<size_t>(kStateLength)),
        durations_(static_cast<size_t>(kStateLength)),
        durationCounts_(static_cast<size_t>(kStateLength)),
        sample_(static_cast<size_t>(kStateLength)) {}

  void setTitle(std::string title) {
    title_ = std::move(title);
  }

  [[nodiscard]] DebugStatTimer timer(StateKey key);
  [[nodiscard]] DebugStatTimer timerSample(StateKey key);

  void start() {
    lifetimeTimer_ =
        std::make_unique<DebugStatTimer>(this, LifeTime, true, false);
  }
  void stop() {
    if (!lifetimeTimer_) {
      return;
    }
    lifetimeTimer_->stop();
    lifetimeTimer_ = nullptr;
  }
  FOLLY_ALWAYS_INLINE
  void count(StateKey key, Count value) {
    counts_[key] += value;
  }
  FOLLY_ALWAYS_INLINE
  void count(StateKey key) {
    counts_[key]++;
  }
  FOLLY_ALWAYS_INLINE
  void record(StateKey key, Duration duration) {
    counts_[key]++;
    durations_[key] += duration;
    durationCounts_[key]++;
  }
  FOLLY_ALWAYS_INLINE
  void record(StateKey key, TimePoint start, TimePoint stop) {
    record(key, std::chrono::duration_cast<Duration>(stop - start));
  }

  static constexpr float kMsNs = 1000000;
  static std::string
  formatDuration(Duration duration, Count totalCount, Count sampleCount) {
    auto ns = duration.count();
    std::stringstream ss;
    if (ns != 0) {
      ss << " avg: " << std::fixed << std::setprecision(6)
         << (static_cast<double>(ns) / kMsNs / sampleCount) << " ms";
      if (totalCount == sampleCount) {
        ss << ", sum: " << std::fixed << std::setprecision(6)
           << (static_cast<double>(ns) / kMsNs) << " ms";
      }
    } else {
      ss << "--";
    }
    return ss.str();
  }

  void print() {
    std::stringstream ss;
    ss << "[DebugStat:" << title_ << "] print stats:";

#define LK_DEBUG_STAT_OUTPUT(key)                                              \
  if (counts_[key] > 0) {                                                      \
    ss << "\n    " << title_ << "::" #key " = " << counts_[key] << " times, "  \
       << formatDuration(durations_[key], counts_[key], durationCounts_[key]); \
  }

    LK_DEBUG_STAT_GENERATOR(LK_DEBUG_STAT_OUTPUT)
#undef LK_DEBUG_STAT_OUTPUT

    ss << "\n    " << title_ << "::CountSteadyClockNow = " << clockNowCount_
       << " times";
    LOG(WARNING) << ss.str();
  }

  [[nodiscard]] DebugStatExposer expose();

  FOLLY_ALWAYS_INLINE
  static DebugStat* current() {
    // if (UNLIKELY(tls_pointer_ == nullptr)) {
    //   return fallback_.get();
    // }
    return tls_pointer_;
  }

 private:
  std::string title_;
  std::vector<Count> counts_;
  std::vector<Duration> durations_;
  std::vector<Count> durationCounts_;
  struct SampleStat {
    Count nextSample{0};
    Count currStep{1};
    Count nextSquare{4};
  };

  std::vector<SampleStat> sample_;
  static inline std::atomic_size_t current_missing_count_ = 0;
  static inline auto fallback_ = std::make_unique<DebugStat>("<fallback>");
  static inline thread_local DebugStat* tls_pointer_ = fallback_.get();
  Count clockNowCount_ = 0;
  std::unique_ptr<DebugStatTimer> lifetimeTimer_{nullptr};

#undef __LK_DEBUG_STAT_GENERATOR
};

} // namespace debug
} // namespace velox
} // namespace facebook

#define DEBUG_STAT_DEFINE_MEMBER(member, name) \
  facebook::velox::debug::DebugStat member{#name};

// set thread_local current() to ptr, and
// reset current() to default(<fallback>) at the end of this scope
#define STRING_CONCAT_2_INNER(name, line) name##line
#define STRING_CONCAT_2(name, line) STRING_CONCAT_2_INNER(name, line)
#define DEBUG_STAT_TLS_EXPOSE_SCOPE(ptr) \
  auto STRING_CONCAT_2(debugStatExposing, __LINE__) = (ptr)->expose();

// 1. increment key by 1
// 2. increment duration to the end of this scope
#define DEBUG_STAT_TLS_RECORD_SCOPE(key)            \
  auto STRING_CONCAT_2(debugStatTiming, __LINE__) = \
      facebook::velox::debug::DebugStat::current()->timer(key);

// 1. increment key by 1
// 2. increment duration if decide to sample (by an increasing step)
#define DEBUG_STAT_TLS_SAMPLE_SCOPE(key)            \
  auto STRING_CONCAT_2(debugStatTiming, __LINE__) = \
      facebook::velox::debug::DebugStat::current()->timerSample(key);

// 1. increment key by 1
// 2. not touch duration
#define DEBUG_STAT_TLS_COUNT_INCR(key) \
  facebook::velox::debug::DebugStat::current()->count(key);

// 1. increment key by n
// 2. not touch duration
#define DEBUG_STAT_TLS_COUNT(key, n) \
  facebook::velox::debug::DebugStat::current()->count(key, n);

#else

namespace facebook {
namespace velox {
namespace debug {
struct DebugStat {};
} // namespace debug
} // namespace velox
} // namespace facebook

#define DEBUG_STAT_DEFINE_MEMBER(member, name)
#define DEBUG_STAT_TLS_EXPOSE_SCOPE(ptr)
#define DEBUG_STAT_TLS_RECORD_SCOPE(key)
#define DEBUG_STAT_TLS_SAMPLE_SCOPE(key)
#define DEBUG_STAT_TLS_COUNT_INCR(key)
#define DEBUG_STAT_TLS_COUNT(key, n)
#endif

namespace facebook::velox::exec {
namespace debug {
using facebook::velox::debug::DebugStat;
} // namespace debug
} // namespace facebook::velox::exec
