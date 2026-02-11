




# 源文件
add_subdirectory(src/src2)
set(SOURCES
        src/EnhancedHashAggregationReplacement.cpp
        src/EnhancedHashAggregation.cpp
        src/EnhancedGroupingSet.cpp
        src/EnhancedHashTable.cpp
        src/EnhancedDecodedVector.cpp
)

# 创建静态库（也可以改为 SHARED 创建动态库）
#add_library(enhanced_hash_agg_plugin SHARED src/EnhancedHashAggregationReplacement.cpp)
add_library(enhanced_hash_agg_plugin SHARED 
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EnhancedHashAggregationReplacement.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EnhancedHashAggregation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EnhancedGroupingSet.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EnhancedHashTable.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EnhancedDecodedVector.cpp
)

# ARM 架构特定的编译选项 - 使用 target_compile_options 确保正确应用
target_compile_options(enhanced_hash_agg_plugin PRIVATE -march=armv8.2-a+crc+sve)

## 链接 Velox 库
#target_link_libraries(enhanced_hash_agg_plugin
#    PUBLIC
#        velox::velox_exec
#        velox::velox_core
#)

# 设置 include 目录
# 1. 项目根目录，用于解析 velox/common/memory/subOp/include/ 路径
# 2. 当前目录的 include 目录，确保所有头文件都能被找到
target_include_directories(enhanced_hash_agg_plugin PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
        enhanced_hash_agg_plugin
        PRIVATE
#        enhanced_hash_agg_plugin222

#        velox_caching
#        velox_common_base
#        velox_exception
        velox_exec
#        velox_exec_test_lib
#        velox_temp_path
#        velox_test_util
#        velox_vector_fuzzer
#        fmt::fmt
#        gflags::gflags
#        glog::glog
#        GTest::gmock
#        GTest::gtest
#        GTest::gtest_main
#        re2::re23
#        Folly::folly
)

## 编译定义
#target_compile_definitions(enhanced_hash_agg_plugin
#    PRIVATE
#        VELOX_VERSION_MAJOR=${velox_VERSION_MAJOR}
#        VELOX_VERSION_MINOR=${velox_VERSION_MINOR}
#)

# Debug logging control for EnhancedHashAggregationAdapter
# This converts the CMake variable to a C++ preprocessor macro
if(ENABLE_ENHANCED_HASH_AGG_ADAPTER_DEBUG)
  target_compile_definitions(enhanced_hash_agg_plugin
      PRIVATE
          ENABLE_ENHANCED_HASH_AGG_ADAPTER_DEBUG=1
  )
  message(STATUS "EnhancedHashAggregationAdapter debug logging: ENABLED")
else()
  message(STATUS "EnhancedHashAggregationAdapter debug logging: DISABLED (default)")
endif()
#
## 安装
#install(TARGETS enhanced_hash_agg_plugin
#    EXPORT EnhancedHashAggregationPluginTargets
#    ARCHIVE DESTINATION lib
#    LIBRARY DESTINATION lib
#    RUNTIME DESTINATION bin
#    INCLUDES DESTINATION include
#)
#
#install(DIRECTORY include/
#    DESTINATION include
#    FILES_MATCHING PATTERN "*.h"
#)
#
## 导出 CMake 配置
#install(EXPORT EnhancedHashAggregationPluginTargets
#    FILE EnhancedHashAggregationPluginTargets.cmake
#    NAMESPACE EnhancedHashAggregationPlugin::
#    DESTINATION lib/cmake/EnhancedHashAggregationPlugin
#)
#
## 创建配置文件
#include(CMakePackageConfigHelpers)
#write_basic_package_version_file(
#    "${CMAKE_CURRENT_BINARY_DIR}/EnhancedHashAggregationPluginConfigVersion.cmake"
#    VERSION ${PROJECT_VERSION}
#    COMPATIBILITY AnyNewerVersion
#)
#
#configure_package_config_file(
#    "${CMAKE_CURRENT_SOURCE_DIR}/EnhancedHashAggregationPluginConfig.cmake.in"
#    "${CMAKE_CURRENT_BINARY_DIR}/EnhancedHashAggregationPluginConfig.cmake"
#    INSTALL_DESTINATION lib/cmake/EnhancedHashAggregationPlugin
#)
#
#install(FILES
#    "${CMAKE_CURRENT_BINARY_DIR}/EnhancedHashAggregationPluginConfig.cmake"
#    "${CMAKE_CURRENT_BINARY_DIR}/EnhancedHashAggregationPluginConfigVersion.cmake"
#    DESTINATION lib/cmake/EnhancedHashAggregationPlugin
#)

## 测试（可选）
#enable_testing()
#if(BUILD_TESTING)
#    add_subdirectory(tests)
#endif()

