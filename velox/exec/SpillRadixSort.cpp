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
#include "velox/exec/SpillRadixSort.h"

#include <algorithm>
#include <cstring>

#include "velox/common/base/SimdUtil.h"
#include "velox/exec/Operator.h"
#include "velox/exec/PrefixSort.h"
#include "velox/external/timsort/TimSort.hpp"

namespace facebook::velox::exec {
namespace {

// PrefixSortLayout pads the normalized buffer to 8 bytes for fast word
// comparison. The spill radix path uses the same layout, but sorts the raw
// bytes directly before PrefixSort's bitsSwapByWord transformation.
constexpr int32_t kAlignment = 8;

std::vector<CompareFlags> compareFlagsOrDefault(
    const RowContainer* rowContainer,
    const std::vector<CompareFlags>& compareFlags) {
  if (!compareFlags.empty()) {
    VELOX_CHECK_EQ(compareFlags.size(), rowContainer->keyTypes().size());
    return compareFlags;
  }
  return std::vector<CompareFlags>(rowContainer->keyTypes().size());
}

PrefixSortLayout generateLayout(
    const RowContainer* rowContainer,
    const std::vector<CompareFlags>& compareFlags,
    const SpillRadixSortConfig& config) {
  const auto& keyTypes = rowContainer->keyTypes();
  std::vector<std::optional<uint32_t>> maxStringLengths;
  maxStringLengths.reserve(keyTypes.size());
  std::vector<bool> columnHasNulls;
  columnHasNulls.reserve(keyTypes.size());

  for (int i = 0; i < keyTypes.size(); ++i) {
    std::optional<uint32_t> maxStringLength = std::nullopt;
    if (keyTypes[i]->kind() == TypeKind::VARBINARY ||
        keyTypes[i]->kind() == TypeKind::VARCHAR) {
      const auto stats = rowContainer->columnStats(i);
      if (stats.has_value()) {
        maxStringLength = stats.value().maxBytes();
      }
    }
    maxStringLengths.emplace_back(maxStringLength);
    columnHasNulls.emplace_back(rowContainer->columnHasNulls(i));
  }

  return PrefixSortLayout::generate(
      keyTypes,
      columnHasNulls,
      compareFlags,
      config.maxNormalizedKeyBytes,
      config.maxStringPrefixLength,
      maxStringLengths);
}

template <typename T>
FOLLY_ALWAYS_INLINE void encodeRowColumn(
    const PrefixSortLayout& layout,
    column_index_t index,
    const RowColumn& rowColumn,
    char* row,
    char* prefixBuffer) {
  std::optional<T> value;
  if (!layout.normalizedKeyHasNullByte[index] ||
      !RowContainer::isNullAt(
          row, rowColumn.nullByte(), rowColumn.nullMask())) {
    value = *(reinterpret_cast<T*>(row + rowColumn.offset()));
  } else {
    value = std::nullopt;
  }

  layout.encoders[index].encode(
      value,
      prefixBuffer + layout.prefixOffsets[index],
      layout.encodeSizes[index],
      layout.normalizedKeyHasNullByte[index]);
}

FOLLY_ALWAYS_INLINE void extractRowColumnToPrefix(
    TypeKind typeKind,
    const PrefixSortLayout& layout,
    uint32_t index,
    const RowColumn& rowColumn,
    char* row,
    char* prefixBuffer) {
  switch (typeKind) {
    case TypeKind::SMALLINT:
      encodeRowColumn<int16_t>(layout, index, rowColumn, row, prefixBuffer);
      return;
    case TypeKind::INTEGER:
      encodeRowColumn<int32_t>(layout, index, rowColumn, row, prefixBuffer);
      return;
    case TypeKind::BIGINT:
      encodeRowColumn<int64_t>(layout, index, rowColumn, row, prefixBuffer);
      return;
    case TypeKind::REAL:
      encodeRowColumn<float>(layout, index, rowColumn, row, prefixBuffer);
      return;
    case TypeKind::DOUBLE:
      encodeRowColumn<double>(layout, index, rowColumn, row, prefixBuffer);
      return;
    case TypeKind::TIMESTAMP:
      encodeRowColumn<Timestamp>(layout, index, rowColumn, row, prefixBuffer);
      return;
    case TypeKind::HUGEINT:
      encodeRowColumn<int128_t>(layout, index, rowColumn, row, prefixBuffer);
      return;
    case TypeKind::VARCHAR:
      [[fallthrough]];
    case TypeKind::VARBINARY:
      encodeRowColumn<StringView>(layout, index, rowColumn, row, prefixBuffer);
      return;
    default:
      VELOX_UNSUPPORTED(
          "spill radix sort does not support type kind: {}",
          mapTypeKindToName(typeKind));
  }
}

void extractRowAndEncodePrefixKeys(
    const RowContainer* rowContainer,
    const PrefixSortLayout& layout,
    char* row,
    char* prefixBuffer) {
  for (auto i = 0; i < layout.numNormalizedKeys; ++i) {
    extractRowColumnToPrefix(
        rowContainer->keyTypes()[i]->kind(),
        layout,
        i,
        rowContainer->columnAt(i),
        row,
        prefixBuffer);
  }

  if (layout.numPaddingBytes > 0) {
    simd::memset(
        prefixBuffer + layout.normalizedBufferSize - layout.numPaddingBytes,
        0,
        layout.numPaddingBytes);
  }

  *reinterpret_cast<char**>(prefixBuffer + layout.normalizedBufferSize) = row;
}

FOLLY_ALWAYS_INLINE char*& rowFromPrefix(
    char* prefixBuffer,
    const PrefixSortLayout& layout) {
  return *reinterpret_cast<char**>(prefixBuffer + layout.normalizedBufferSize);
}

int compareNonPrefixKeys(
    const RowContainer* rowContainer,
    const PrefixSortLayout& layout,
    char* leftRow,
    char* rightRow) {
  for (auto i = layout.nonPrefixSortStartIndex; i < layout.numKeys; ++i) {
    const auto result =
        rowContainer->compare(leftRow, rightRow, i, layout.compareFlags[i]);
    if (result != 0) {
      return result;
    }
  }
  return 0;
}

void countingPass(
    char** in,
    char** out,
    size_t numRows,
    uint32_t byteOffset) {
  uint32_t counts[257];
  std::memset(counts, 0, sizeof(counts));

  for (size_t i = 0; i < numRows; ++i) {
    ++counts[1 + static_cast<uint8_t>(*(in[i] + byteOffset))];
  }
  for (int32_t i = 1; i < 256; ++i) {
    counts[i] += counts[i - 1];
  }
  for (size_t i = 0; i < numRows; ++i) {
    const auto key = static_cast<uint8_t>(*(in[i] + byteOffset));
    out[counts[key]++] = in[i];
  }
}

char** radixSort(
    char** src,
    char** scratch,
    size_t numRows,
    uint32_t normalizedBytes) {
  if (numRows <= 1) {
    return src;
  }

  char** in = src;
  char** out = scratch;
  for (int32_t byte = static_cast<int32_t>(normalizedBytes) - 1; byte >= 0;
       --byte) {
    countingPass(in, out, numRows, byte);
    std::swap(in, out);
  }
  return in;
}

bool validForRadix(
    const PrefixSortLayout& layout,
    size_t numRows,
    const SpillRadixSortConfig& config) {
  if (!config.enabled || numRows < config.minRows) {
    return false;
  }
  if (!layout.hasNormalizedKeys || layout.normalizedBufferSize == 0) {
    return false;
  }
  // Should be guaranteed by PrefixSortLayout::generate. Keep this as a cheap
  // guard because radix iterates over every byte including padding.
  return layout.normalizedBufferSize % kAlignment == 0;
}

} // namespace

// static
bool SpillRadixSort::canSort(
    const RowContainer* rowContainer,
    const std::vector<CompareFlags>& compareFlags,
    size_t numRows,
    const SpillRadixSortConfig& config) {
  if (rowContainer == nullptr) {
    return false;
  }
  const auto normalizedFlags = compareFlagsOrDefault(rowContainer, compareFlags);
  const auto layout = generateLayout(rowContainer, normalizedFlags, config);
  return validForRadix(layout, numRows, config);
}

// static
void SpillRadixSort::sort(
    const RowContainer* rowContainer,
    const std::vector<CompareFlags>& compareFlags,
    const SpillRadixSortConfig& config,
    memory::MemoryPool* pool,
    SpillRows& rows) {
  const auto numRows = rows.size();
  const auto normalizedFlags = compareFlagsOrDefault(rowContainer, compareFlags);
  const auto layout = generateLayout(rowContainer, normalizedFlags, config);
  VELOX_CHECK(
      validForRadix(layout, numRows, config),
      "SpillRadixSort::sort called for unsupported input");

  const auto entrySize = layout.entrySize;
  memory::ContiguousAllocation prefixBufferAlloc;
  {
    const auto numPages =
        memory::AllocationTraits::numPages(numRows * entrySize);
    pool->allocateContiguous(numPages, prefixBufferAlloc);
  }
  char* prefixBuffer = prefixBufferAlloc.data<char>();

  for (auto i = 0; i < rows.size(); ++i) {
    extractRowAndEncodePrefixKeys(
        rowContainer, layout, rows[i], prefixBuffer + i * entrySize);
  }

  auto srcBuffer = AlignedBuffer::allocate<char*>(numRows, pool);
  auto scratchBuffer = AlignedBuffer::allocate<char*>(numRows, pool);
  char** src = srcBuffer->asMutable<char*>();
  char** scratch = scratchBuffer->asMutable<char*>();
  for (size_t i = 0; i < numRows; ++i) {
    src[i] = prefixBuffer + i * entrySize;
  }

  char** sorted = radixSort(src, scratch, numRows, layout.normalizedBufferSize);

  addThreadLocalRuntimeStat(
      SpillRadixSort::kSpillRadixSortRows,
      RuntimeCounter(numRows, RuntimeCounter::Unit::kNone));

  const bool needsResidualCompare = layout.hasNonNormalizedKey ||
      layout.nonPrefixSortStartIndex < layout.numNormalizedKeys;
  int64_t residualGroups = 0;
  if (needsResidualCompare && numRows > 1) {
    size_t begin = 0;
    while (begin < numRows) {
      size_t end = begin + 1;
      while (end < numRows &&
             std::memcmp(
                 sorted[begin], sorted[end], layout.normalizedBufferSize) ==
                 0) {
        ++end;
      }

      if (end - begin > 1) {
        ++residualGroups;
        gfx::timsort(
            sorted + begin, sorted + end, [&](char* left, char* right) {
              return compareNonPrefixKeys(
                         rowContainer,
                         layout,
                         rowFromPrefix(left, layout),
                         rowFromPrefix(right, layout)) < 0;
            });
      }
      begin = end;
    }
  }

  if (residualGroups > 0) {
    addThreadLocalRuntimeStat(
        SpillRadixSort::kSpillRadixSortResidualGroups,
        RuntimeCounter(residualGroups, RuntimeCounter::Unit::kNone));
  }

  for (size_t i = 0; i < numRows; ++i) {
    rows[i] = rowFromPrefix(sorted[i], layout);
  }
}

} // namespace facebook::velox::exec
