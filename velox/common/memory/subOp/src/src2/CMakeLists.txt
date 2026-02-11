

# 创建静态库（也可以改为 SHARED 创建动态库）
add_library(enhanced_hash_agg_plugin222 SHARED ../EnhancedHashAggregation.cpp ../EnhancedGroupingSet.cpp ../EnhancedHashTable.cpp)

# ARM 架构特定的编译选项 - 必须在目标创建后使用 target_compile_options
target_compile_options(enhanced_hash_agg_plugin222 PRIVATE -march=armv8.2-a+crc+sve)

target_include_directories(enhanced_hash_agg_plugin222 PRIVATE "include/enhanced_hash_agg/EnhancedHashAggregation.h" "include/enhanced_hash_agg/EnhancedGroupingSet.h" "include/enhanced_hash_agg/EnhancedHashTable.h" "include/enhanced_hash_agg/EnhancedHashAggregationReplacement.h")

target_link_libraries(
        enhanced_hash_agg_plugin222
        PRIVATE
        velox_exec
)