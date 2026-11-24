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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>

#include <gtest/gtest.h>

#include "velox/optimizer/EGraph.h"

namespace facebook::velox::optimizer {

/// Structural fixtures explicitly assume a deterministic test dialect.
/// This witness is only for synthetic TypedExprs: production uses resolved
/// registry signatures and never inherits this test policy.
inline bool testFunctionIsDeterministic(
    const std::string& name,
    const std::vector<TypePtr>&,
    const TypePtr&) {
  return name != "rand" && name != "random" && name != "uuid" &&
      name != "shuffle" && name != "array_shuffle" &&
      name != "monotonically_increasing_id" &&
      name != "monotonic_increasing_id";
}

inline EGraphConfig testGraphConfig() {
  EGraphConfig config;
  config.functionDeterminismCheck = testFunctionIsDeterministic;
  return config;
}

// A cycle/closure regression must not hang the runner. Join completed workers
// and propagate their exceptions; on timeout exit before graph teardown.
template <typename Fn>
void withOptimizerWatchdog(Fn&& fn) {
  std::atomic<bool> done{false};
  std::exception_ptr failure;
  std::thread worker([&] {
    try {
      fn();
    } catch (...) {
      failure = std::current_exception();
    }
    done = true;
  });
  for (int i = 0; i < 300 && !done.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!done.load()) {
    ADD_FAILURE() << "CostModel/EGraph did not complete within 30 seconds";
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(1);
  }
  worker.join();
  if (failure) {
    std::rethrow_exception(failure);
  }
}

} // namespace facebook::velox::optimizer
