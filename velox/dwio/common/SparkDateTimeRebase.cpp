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

#include "velox/dwio/common/SparkDateTimeRebase.h"

#include <folly/String.h>

#include "velox/vector/FlatVector.h"

namespace facebook::velox::dwio::common {
namespace {

std::optional<SparkRebaseFunctions>& rebaseFunctions() {
  static std::optional<SparkRebaseFunctions> functions;
  return functions;
}

template <
    typename T,
    typename Raw,
    typename Extract,
    typename Convert,
    typename Rebase>
void rebaseVector(
    const VectorPtr& vector,
    SparkRebaseMode mode,
    Raw cutoff,
    bool force,
    Extract extract,
    Convert convert,
    Rebase rebase) {
  // Reader output is flat except for an all-null constant.
  if (vector->isConstantEncoding() && vector->isNullAt(0)) {
    return;
  }
  auto* flat = vector->asUnchecked<FlatVector<T>>();
  const auto* raw = flat->rawValues();
  std::vector<vector_size_t> positions;
  std::vector<Raw> values;
  for (vector_size_t i = 0; i < flat->size(); ++i) {
    if (flat->isNullAt(i)) {
      continue;
    }
    const auto value = extract(raw[i]);
    if (force || value < cutoff) {
      VELOX_USER_CHECK(
          mode != SparkRebaseMode::kException,
          "Spark EXCEPTION rebase mode cannot read an ancient {} value. "
          "Specify LEGACY or CORRECTED for the Parquet file's calendar.",
          vector->type()->toString());
      positions.push_back(i);
      values.push_back(value);
    }
  }
  if (values.empty()) {
    return;
  }
  rebase(values.data(), values.size());
  auto* output = flat->mutableRawValues();
  for (size_t i = 0; i < values.size(); ++i) {
    output[positions[i]] = convert(values[i]);
  }
}

} // namespace

SparkRebaseMode sparkRebaseMode(std::string_view mode) {
  std::string normalized(mode);
  folly::toLowerAscii(normalized);
  if (normalized == "legacy") {
    return SparkRebaseMode::kLegacy;
  }
  if (normalized == "corrected") {
    return SparkRebaseMode::kCorrected;
  }
  VELOX_USER_CHECK(
      normalized == "exception",
      "Invalid Spark Parquet datetime rebase mode: {}",
      mode);
  return SparkRebaseMode::kException;
}

void registerSparkRebaseFunctions(SparkRebaseFunctions functions) {
  rebaseFunctions() = std::move(functions);
}

void unregisterSparkRebaseFunctions() {
  rebaseFunctions().reset();
}

bool hasSparkRebaseFunctions() {
  return rebaseFunctions().has_value();
}

SparkOrcTimestampOptions sparkOrcTimestampOptions(
    const std::string& writerZone) {
  const auto& functions = rebaseFunctions();
  VELOX_CHECK(
      functions.has_value(), "Spark datetime rebase functions are unavailable");
  return functions->orcTimestampOptions(writerZone);
}

void SparkDateTimeRebase::rebaseDates(const VectorPtr& vector) const {
  if (!enabled() || vector->size() == 0) {
    return;
  }
  const auto& functions = rebaseFunctions();
  VELOX_CHECK(
      functions.has_value(), "Spark datetime rebase functions are unavailable");
  rebaseVector<int32_t>(
      vector,
      mode,
      functions->lastJulianDay,
      false,
      [](int32_t day) { return day; },
      [](int32_t day) { return day; },
      [&](int32_t* days, size_t size) {
        functions->dates(orcProleptic, days, size);
      });
}

void SparkDateTimeRebase::rebaseTimestamps(const VectorPtr& vector) const {
  if (!enabled() || vector->size() == 0) {
    return;
  }
  const auto& functions = rebaseFunctions();
  VELOX_CHECK(
      functions.has_value(), "Spark datetime rebase functions are unavailable");
  rebaseVector<Timestamp>(
      vector,
      mode,
      functions->lastJulianMicros,
      adjustOrcTimezone,
      [](const Timestamp& timestamp) { return timestamp.toMicros(); },
      [](int64_t micros) { return Timestamp::fromMicros(micros); },
      [&](int64_t* values, size_t size) {
        functions->timestamps(
            timeZone, isOrc, orcProleptic, adjustOrcTimezone, values, size);
      });
}

bool isSparkRebaseType(const Type& type) {
  return type.isDate() || type.kind() == TypeKind::TIMESTAMP;
}

} // namespace facebook::velox::dwio::common
