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

#include <string>
#include <vector>

#include "velox/exec/RowContainer.h"

namespace facebook::velox::exec {

/// Config for the spill-specific radix sort optimization.
///
/// The algorithm encodes a prefix of the spill sort keys using the same
/// normalized-key rules as PrefixSort, sorts the prefix with stable LSD radix
/// sort, and falls back to RowContainer comparison only for rows whose
/// normalized prefixes are identical.
struct SpillRadixSortConfig {
  bool enabled{true};

  /// Minimum number of spill rows for radix sort. Small runs are cheaper with
  /// the existing comparator sort due to radix counting-pass overhead.
  uint32_t minRows{1024};

  /// Maximum bytes to use for normalized key prefix per row.
  uint32_t maxNormalizedKeyBytes{64};

  /// Maximum bytes to encode from a VARCHAR/VARBINARY key. Longer strings are
  /// sorted by this prefix first, then resolved by residual RowContainer
  /// comparison within equal-prefix groups.
  uint32_t maxStringPrefixLength{16};
};

class SpillRadixSort {
 public:
  using SpillRows = std::vector<char*, memory::StlAllocator<char*>>;

  /// Returns true if 'rows' can benefit from the spill radix path.
  static bool canSort(
      const RowContainer* rowContainer,
      const std::vector<CompareFlags>& compareFlags,
      size_t numRows,
      const SpillRadixSortConfig& config = {});

  /// Sorts 'rows' using the spill radix path. The caller must call canSort()
  /// first; this function validates the same invariant and throws if violated.
  static void sort(
      const RowContainer* rowContainer,
      const std::vector<CompareFlags>& compareFlags,
      const SpillRadixSortConfig& config,
      memory::MemoryPool* pool,
      SpillRows& rows);

  /// Runtime stat: rows sorted via the spill radix path.
  static inline const std::string kSpillRadixSortRows{"spillRadixSortRows"};

  /// Runtime stat: number of equal-prefix groups that required residual
  /// RowContainer comparison.
  static inline const std::string kSpillRadixSortResidualGroups{
      "spillRadixSortResidualGroups"};
};

} // namespace facebook::velox::exec
