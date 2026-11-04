# Copyright (c) Facebook, Inc. and its affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

add_subdirectory(specialforms)

# Optional ISA-L Crypto for SparkMd5VectorFunction (long strings). Fusion
# (FusedMd5ConcatWs) does not depend on this library.
set(_velox_isal_crypto_md5_found FALSE)
set(_VELOX_ISAL_CRYPTO_VIA_PKGCONFIG FALSE)
if(VELOX_ENABLE_ISAL_CRYPTO_MD5)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(ISAL_CRYPTO QUIET IMPORTED_TARGET libisal_crypto)
  endif()
  if(TARGET PkgConfig::ISAL_CRYPTO)
    set(_velox_isal_crypto_md5_found TRUE)
    set(_VELOX_ISAL_CRYPTO_VIA_PKGCONFIG TRUE)
  else()
    find_path(
      ISAL_CRYPTO_INCLUDE_DIR
      NAMES isa-l_crypto/md5_mb.h
      PATHS /usr/include /usr/local/include /opt/include)
    find_library(
      ISAL_CRYPTO_LIBRARY
      NAMES isal_crypto libisal_crypto
      PATHS /usr/lib64 /usr/lib /usr/local/lib /usr/local/lib64 /opt/lib)
    if(ISAL_CRYPTO_INCLUDE_DIR AND ISAL_CRYPTO_LIBRARY)
      set(_velox_isal_crypto_md5_found TRUE)
    endif()
  endif()
endif()

if(_velox_isal_crypto_md5_found)
  message(STATUS "ISA-L Crypto detected: Spark md5 vector path enabled")
else()
  if(VELOX_ENABLE_ISAL_CRYPTO_MD5)
    message(
      STATUS "ISA-L Crypto not detected: Spark md5 stays scalar")
  else()
    message(
      STATUS
        "ISA-L Crypto MD5 detection disabled: Spark md5 stays scalar")
  endif()
endif()

set(_VELOX_FUNCTIONS_SPARK_IMPL_SRCS
    ArrayGetFunction.cpp
    ArraySort.cpp
    Comparisons.cpp
    ConcatWs.cpp
    FusedMd5ConcatWs.cpp
    DecimalArithmetic.cpp
    DecimalCompare.cpp
    Hash.cpp
    In.cpp
    LeastGreatest.cpp
    MakeTimestamp.cpp
    Map.cpp
    RegexFunctions.cpp
    Size.cpp
    String.cpp
    UnscaledValueFunction.cpp)
if(_velox_isal_crypto_md5_found)
  list(APPEND _VELOX_FUNCTIONS_SPARK_IMPL_SRCS Md5VectorFunction.cpp)
endif()

velox_add_library(velox_functions_spark_impl
                  ${_VELOX_FUNCTIONS_SPARK_IMPL_SRCS})

velox_link_libraries(
  velox_functions_spark_impl
  velox_functions_lib
  velox_functions_prestosql_impl
  velox_functions_spark_specialforms
  velox_functions_util
  Folly::folly
  simdjson::simdjson)

if(_velox_isal_crypto_md5_found)
  # Do not use directory-scoped add_compile_definitions: Gluten sets
  # VELOX_MONO_LIBRARY, and velox_add_library appends sources onto the
  # aggregated target `velox` (created in another directory). Use the
  # Velox helper so the define lands on `velox` when mono is on, and on
  # velox_functions_spark_impl otherwise.
  velox_compile_definitions(
    velox_functions_spark_impl PRIVATE VELOX_HAVE_ISAL_CRYPTO_MD5=1)
  if(_VELOX_ISAL_CRYPTO_VIA_PKGCONFIG)
    velox_link_libraries(velox_functions_spark_impl PkgConfig::ISAL_CRYPTO)
  else()
    velox_include_directories(velox_functions_spark_impl PRIVATE
                              ${ISAL_CRYPTO_INCLUDE_DIR})
    velox_link_libraries(velox_functions_spark_impl ${ISAL_CRYPTO_LIBRARY})
  endif()
  # Gluten JNI: import_library(facebook::velox libvelox.a) drops this link.
  # The shared libvelox.so must link libisal_crypto.a in Gluten
  # cpp/velox/CMakeLists.txt (Ray0915/velox_3643 md5).
endif()

if(NOT VELOX_MONO_LIBRARY)
  set_property(TARGET velox_functions_spark_impl PROPERTY JOB_POOL_COMPILE
                                                          high_memory_pool)
endif()

add_subdirectory(window)

if(${VELOX_ENABLE_AGGREGATES})
  add_subdirectory(aggregates)
endif()

if(${VELOX_BUILD_TESTING})
  add_subdirectory(tests)
  add_subdirectory(coverage)
  add_subdirectory(fuzzer)
endif()

if(${VELOX_ENABLE_BENCHMARKS})
  add_subdirectory(benchmarks)
endif()

add_subdirectory(registration)

# Non-mono: RegisterString.cpp lives on velox_functions_spark, a different
# target. Mono: this helper redirects to `velox` (same as the impl call).
if(_velox_isal_crypto_md5_found)
  velox_compile_definitions(
    velox_functions_spark PRIVATE VELOX_HAVE_ISAL_CRYPTO_MD5=1)
endif()
