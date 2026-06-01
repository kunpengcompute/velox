cmake_minimum_required(VERSION 3.16)
project(SVE_Module_Benchmarks)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置编译选项
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    message(STATUS "Building for ARM64 architecture")
    
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=armv8-a+sve" COMPILER_SUPPORTS_SVE)
    check_cxx_compiler_flag("-march=armv8-a+sve2" COMPILER_SUPPORTS_SVE2)
    
    if(COMPILER_SUPPORTS_SVE)
        message(STATUS "Compiler supports SVE")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a+sve -O3")
    else()
        message(STATUS "Compiler does NOT support SVE")
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

# 公共头文件
set(COMMON_HEADERS benchmark_framework.h)

# 模块级benchmark程序
add_executable(01_hash_table_module_benchmark 
    01_hash_table_module_benchmark.cpp 
    ${COMMON_HEADERS})

add_executable(02_aggregation_module_benchmark 
    02_aggregation_module_benchmark.cpp 
    ${COMMON_HEADERS})

add_executable(03_string_processing_module_benchmark 
    03_string_processing_module_benchmark.cpp 
    ${COMMON_HEADERS})

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 打印编译信息
message(STATUS "")
message(STATUS "=== Build Configuration ===")
message(STATUS "CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")
message(STATUS "Output directory: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "")
message(STATUS "Module Benchmarks:")
message(STATUS "  1. HashTable Module (HashJoin simulation)")
message(STATUS "  2. Aggregation Module (Filter/Null/Aggregate)")
message(STATUS "  3. String Processing Module")
message(STATUS "")