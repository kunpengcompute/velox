cmake_minimum_required(VERSION 3.16)
project(Velox_SVE_Advanced_Benchmarks)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    message(STATUS "Building for ARM64 architecture")
    
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=armv8-a+sve" COMPILER_SUPPORTS_SVE)
    check_cxx_compiler_flag("-march=armv8-a+sve2" COMPILER_SUPPORTS_SVE2)
    
    if(COMPILER_SUPPORTS_SVE)
        message(STATUS "Compiler supports SVE - enabling optimizations")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a+sve -O3")
    else()
        message(STATUS "Compiler does NOT support SVE - using generic optimizations")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
    endif()
    
    if(COMPILER_SUPPORTS_SVE2)
        message(STATUS "Compiler supports SVE2")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a+sve2")
    endif()
else()
    message(STATUS "Building for non-ARM architecture")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
endif()

set(ADVANCED_HEADERS advanced_benchmark_framework.h)

add_executable(04_hash_aggregation_advanced_benchmark 
    04_hash_aggregation_advanced_benchmark.cpp 
    ${ADVANCED_HEADERS})

add_executable(05_hash_join_advanced_benchmark 
    05_hash_join_advanced_benchmark.cpp 
    ${ADVANCED_HEADERS})

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

message(STATUS "")
message(STATUS "=== Velox Advanced SVE Benchmarks ===")
message(STATUS "CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")
message(STATUS "Output directory: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "")
message(STATUS "Advanced Module Benchmarks:")
message(STATUS "  4. HashAggregation Advanced (Complete pipeline)")
message(STATUS "  5. HashJoin Advanced (Build + Probe + Prefetch)")
message(STATUS "")
message(STATUS "Features:")
message(STATUS "  - Grouping key processing")
message(STATUS "  - Hash computation (VectorHasher)")
message(STATUS "  - Collision handling")
message(STATUS "  - Prefetch optimization")
message(STATUS "  - Multiple hash modes")
message(STATUS "  - Memory tracking")
message(STATUS "  - Business metrics")
message(STATUS "")