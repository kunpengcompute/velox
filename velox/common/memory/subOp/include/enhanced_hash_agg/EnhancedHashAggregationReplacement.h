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

// ============================================================================
// Debug Logging Control for EnhancedHashAggregationAdapter
// ============================================================================
// 
// Usage:
//   - To enable debug logs: Compile with -DENABLE_ENHANCED_HASH_AGG_ADAPTER_DEBUG=1
//   - To disable debug logs: Default (no flag needed) or -DENABLE_ENHANCED_HASH_AGG_ADAPTER_DEBUG=0
//
// Performance:
//   - When disabled (default): All debug code is completely removed at compile time
//     by the compiler optimizer. Zero runtime overhead, zero binary size increase.
//   - When enabled: Debug logs are printed using LOG(ERROR) macro.
//
// Example compilation:
//   cmake -DENABLE_ENHANCED_HASH_AGG_ADAPTER_DEBUG=1 ...
//   or
//   g++ -DENABLE_ENHANCED_HASH_AGG_ADAPTER_DEBUG=1 ...
// ============================================================================

#ifndef ENABLE_ENHANCED_HASH_AGG_ADAPTER_DEBUG
#define ENABLE_ENHANCED_HASH_AGG_ADAPTER_DEBUG 0
#endif

#if ENABLE_ENHANCED_HASH_AGG_ADAPTER_DEBUG
#define ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(level, msg) LOG(level) << "[EnhancedHashAggregationAdapter] " << msg
#else
// When debug is disabled, this macro expands to nothing, and the compiler
// will completely remove the code (including string concatenations) at compile time
#define ENHANCED_HASH_AGG_ADAPTER_DEBUG_LOG(level, msg) \
  do {                                                  \
  } while (0)
#endif

namespace facebook::velox::exec {

/// Registers the EnhancedHashAggregation operator replacement adapter.
/// This should be called once during program initialization, typically
/// in the main() function or during static initialization.
/// 
/// Thread-safe: Can be called multiple times safely (idempotent).
/// 
/// @throws std::runtime_error if registration fails
void registerEnhancedHashAggregationReplacement();

/// Unregisters the EnhancedHashAggregation operator replacement adapter.
/// Useful for testing or cleanup.
void unregisterEnhancedHashAggregationReplacement();

/// Returns true if the adapter is currently registered.
bool isEnhancedHashAggregationRegistered();

} // namespace facebook::velox::exec

