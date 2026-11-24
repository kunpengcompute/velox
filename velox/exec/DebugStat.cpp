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

#include "velox/exec/DebugStat.h" /* IWYU pragma: keep */
#ifdef DEBUG_STAT_ENABLED

namespace facebook::velox::debug {

DebugStat::DebugStatTimer::DebugStatTimer(
    DebugStat* stat,
    StateKey key,
    bool startOnCreate,
    bool stopOnDestroy)
    : stat_(stat), key_(key), stopOnDestroy_(stopOnDestroy) {
  if (startOnCreate) {
    started_ = true;
    start_ = std::chrono::steady_clock::now();
    stat_->clockNowCount_++;
  }
}

DebugStat::DebugStatTimer::~DebugStatTimer() {
  if (stopOnDestroy_) {
    stop();
  }
}

DebugStat::DebugStatExposer::DebugStatExposer(DebugStat* stat) {
  old_ = tls_pointer_;
  tls_pointer_ = stat;
}
DebugStat::DebugStatExposer::~DebugStatExposer() {
  tls_pointer_ = old_;
}

[[nodiscard]]
DebugStat::DebugStatExposer DebugStat::expose() {
  return DebugStat::DebugStatExposer{this};
}

[[nodiscard]]
DebugStat::DebugStatTimer DebugStat::timer(StateKey key) {
  return {this, key, true, true};
}

[[nodiscard]]
DebugStat::DebugStatTimer DebugStat::timerSample(StateKey key) {
  if (counts_[key] < sample_[key].nextSample) {
    counts_[key]++;
    return {this, key, false, false};
  }
  if (counts_[key] >= sample_[key].nextSquare) {
    sample_[key].currStep++;
    sample_[key].nextSquare =
        (sample_[key].currStep + 1) * (sample_[key].currStep + 1);
  }
  sample_[key].nextSample = counts_[key] + sample_[key].currStep;
  return {this, key, true, true};
}

} // namespace facebook::velox::debug

#endif
