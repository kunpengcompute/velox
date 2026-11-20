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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "velox/vector/BaseVector.h"

namespace facebook::velox::dwio::common {

inline constexpr char kSparkDateTimeRebaseEnabled[] =
    "spark-datetime-rebase-enabled";
inline constexpr char kSparkDateTimeRebaseMode[] =
    "spark.sql.parquet.datetimeRebaseModeInRead";
inline constexpr char kSparkInt96RebaseMode[] =
    "spark.sql.parquet.int96RebaseModeInRead";

enum class SparkRebaseMode { kCorrected, kLegacy, kException };

SparkRebaseMode sparkRebaseMode(std::string_view mode);

struct SparkRebaseOptions {
  SparkRebaseMode dateTime;
  SparkRebaseMode int96;
};

struct SparkOrcTimestampOptions {
  int64_t baseSeconds;
  bool adjustTimezone;
};

// The Spark embedding provides the calendar and historical time zone rules.
struct SparkRebaseFunctions {
  int32_t lastJulianDay;
  int64_t lastJulianMicros;
  std::function<void(bool, int32_t*, size_t)> dates;
  std::function<void(const std::string&, bool, bool, bool, int64_t*, size_t)>
      timestamps;
  std::function<SparkOrcTimestampOptions(const std::string&)> orcTimestampOptions;
};

void registerSparkRebaseFunctions(SparkRebaseFunctions functions);
void unregisterSparkRebaseFunctions();
bool hasSparkRebaseFunctions();
SparkOrcTimestampOptions sparkOrcTimestampOptions(const std::string& writerZone);

struct SparkDateTimeRebase {
  SparkRebaseMode mode{SparkRebaseMode::kCorrected};
  std::string timeZone;
  bool isOrc{false};
  bool orcProleptic{false};
  bool adjustOrcTimezone{false};

  bool enabled() const {
    return mode != SparkRebaseMode::kCorrected;
  }

  void rebaseDates(const VectorPtr& vector) const;
  void rebaseTimestamps(const VectorPtr& vector) const;
};

bool isSparkRebaseType(const Type& type);

} // namespace facebook::velox::dwio::common
