cmake_minimum_required(VERSION 3.16)
project(SVE_Optimization_Benchmarks)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置编译选项
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    message(STATUS "Building for ARM64 architecture")
    
    # 检查编译器是否支持SVE
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=armv8-a+sve" COMPILER_SUPPORTS_SVE)
    check_cxx_compiler_flag("-march=armv8-a+sve2" COMPILER_SUPPORTS_SVE2)
    
    if(COMPILER_SUPPORTS_SVE)
        message(STATUS "Compiler supports SVE")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a+sve")
    else()
        message(STATUS "Compiler does NOT support SVE, building without SVE optimizations")
    endif()
    
    if(COMPILER_SUPPORTS_SVE2)
        message(STATUS "Compiler supports SVE2")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a+sve2")
    endif()
else()
    message(STATUS "Building for non-ARM architecture (SVE optimizations will be disabled)")
endif()

# 添加benchmark程序
add_executable(01_gather_benchmark 01_gather_benchmark.cpp)
add_executable(02_tag_vector_load_benchmark 02_tag_vector_load_benchmark.cpp)
add_executable(03_filter_compact_benchmark 03_filter_compact_benchmark.cpp)
add_executable(04_popcount_benchmark 04_popcount_benchmark.cpp)
add_executable(05_string_length_benchmark 05_string_length_benchmark.cpp)

# 输出编译信息
message(STATUS "CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")
message(STATUS "Benchmarks will be built:")
message(STATUS "  - 01_gather_benchmark")
message(STATUS "  - 02_tag_vector_load_benchmark")
message(STATUS "  - 03_filter_compact_benchmark")
message(STATUS "  - 04_popcount_benchmark")
message(STATUS "  - 05_string_length_benchmark")