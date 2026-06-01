#pragma once

#include <vector>
#include <string>
#include <random>
#include <chrono>
#include <iostream>
#include <functional>
#include <map>
#include <cstring>
#include <memory>

#ifdef __ARM_FEATURE_SVE
#include <arm_sve.h>
#endif

namespace velox_benchmark {

using namespace std;

struct AdvancedBenchmarkConfig {
    // 基础参数
    int data_scale;
    int iterations;
    int vector_width;
    
    // HashAgg参数
    int num_grouping_keys;
    int num_aggregates;
    double grouping_key_cardinality;  // grouping key基数比例
    double partial_agg_memory_limit_mb;
    bool enable_distinct_agg;
    bool enable_spill;
    
    // HashJoin参数
    int build_side_size;
    int probe_side_size;
    double join_selectivity;
    bool enable_prefetch;
    string hash_mode;  // "array" or "regular"
    
    // 表达式参数
    int num_expressions;
    int expression_depth;
    bool enable_peeled_encoding;
    bool enable_constant_folding;
    
    // 数据参数
    string data_type;
    string distribution;
    double null_ratio;
    double selectivity;
    
    bool verify_correctness;
    bool verbose_output;
    bool enable_memory_tracking;
    
    AdvancedBenchmarkConfig() {
        data_scale = 1000000;
        iterations = 1000;
        vector_width = 256;
        
        num_grouping_keys = 3;
        num_aggregates = 5;
        grouping_key_cardinality = 0.01;  // 1%基数
        partial_agg_memory_limit_mb = 100;
        enable_distinct_agg = false;
        enable_spill = false;
        
        build_side_size = 1000000;
        probe_side_size = 10000000;
        join_selectivity = 0.1;
        enable_prefetch = true;
        hash_mode = "regular";
        
        num_expressions = 10;
        expression_depth = 5;
        enable_peeled_encoding = true;
        enable_constant_folding = true;
        
        data_type = "int32";
        distribution = "uniform";
        null_ratio = 0.1;
        selectivity = 0.5;
        
        verify_correctness = true;
        verbose_output = false;
        enable_memory_tracking = false;
    }
};

struct AdvancedBenchmarkResult {
    string benchmark_name;
    string implementation;
    string scenario_description;
    
    double time_us;
    double throughput_mbps;
    double throughput_rows_per_sec;
    int64_t items_processed;
    int64_t bytes_processed;
    
    bool correctness_passed;
    double speedup_vs_scalar;
    
    // 业务指标
    map<string, double> business_metrics;
    
    // 内存指标
    double memory_used_mb;
    int64_t num_allocations;
    
    // Hash相关指标
    int64_t num_hash_collisions;
    int64_t num_rehashes;
    double avg_probe_depth;
    
    // 聚合相关指标
    int64_t num_groups;
    int64_t num_distinct_groups;
    double aggregation_efficiency;
    
    // 表达式相关指标
    int64_t num_peeled_layers;
    double expression_compile_time_us;
    
    void printDetailed() const {
        cout << "\n=== " << benchmark_name << " (" << implementation << ") ===" << endl;
        cout << "Scenario: " << scenario_description << endl;
        cout << "Time: " << time_us << " us" << endl;
        cout << "Throughput: " << throughput_rows_per_sec << " rows/sec" << endl;
        cout << "Items processed: " << items_processed << endl;
        cout << "Bytes processed: " << bytes_processed << " bytes" << endl;
        
        if (speedup_vs_scalar > 0) {
            cout << "Speedup vs Scalar: " << speedup_vs_scalar << "x" << endl;
        }
        
        if (verify_correctness) {
            cout << "Correctness: " << (correctness_passed ? "PASS" : "FAIL") << endl;
        }
        
        if (memory_used_mb > 0) {
            cout << "Memory used: " << memory_used_mb << " MB" << endl;
        }
        
        for (const auto& metric : business_metrics) {
            cout << "  " << metric.first << ": " << metric.second << endl;
        }
        
        if (num_hash_collisions > 0) {
            cout << "Hash collisions: " << num_hash_collisions << endl;
            cout << "Avg probe depth: " << avg_probe_depth << endl;
        }
        
        if (num_groups > 0) {
            cout << "Groups created: " << num_groups << endl;
            cout << "Aggregation efficiency: " << aggregation_efficiency << "%" << endl;
        }
        
        cout << endl;
    }
};

class AdvancedDataGenerator {
public:
    mt19937 rng_;
    
    AdvancedDataGenerator(int seed = 42) : rng_(seed) {}
    
    // 生成grouping keys（模拟HashAgg的grouping key）
    vector<vector<int32_t>> generateGroupingKeys(int num_rows, int num_keys, double cardinality_ratio) {
        vector<vector<int32_t>> keys(num_keys);
        int cardinality = max(1, (int)(num_rows * cardinality_ratio));
        
        for (int k = 0; k < num_keys; ++k) {
            keys[k].resize(num_rows);
            uniform_int_distribution<int32_t> dist(0, cardinality - 1);
            for (int i = 0; i < num_rows; ++i) {
                keys[k][i] = dist(rng_);
            }
        }
        return keys;
    }
    
    // 生成聚合输入数据
    vector<vector<int64_t>> generateAggregateInputs(int num_rows, int num_aggs) {
        vector<vector<int64_t>> inputs(num_aggs);
        uniform_int_distribution<int64_t> dist(1, 1000);
        
        for (int a = 0; a < num_aggs; ++a) {
            inputs[a].resize(num_rows);
            for (int i = 0; i < num_rows; ++i) {
                inputs[a][i] = dist(rng_);
            }
        }
        return inputs;
    }
    
    // 生成hash值（模拟VectorHasher输出）
    vector<uint64_t> generateHashValues(int num_rows, int cardinality) {
        vector<uint64_t> hashes(num_rows);
        uniform_int_distribution<uint64_t> dist(0, cardinality - 1);
        for (int i = 0; i < num_rows; ++i) {
            hashes[i] = dist(rng_);
        }
        return hashes;
    }
    
    // 生成build side数据（HashJoin build端）
    vector<int32_t> generateBuildSideKeys(int num_rows, double duplication_ratio) {
        vector<int32_t> keys(num_rows);
        int unique_keys = (int)(num_rows * duplication_ratio);
        uniform_int_distribution<int32_t> dist(0, unique_keys - 1);
        for (int i = 0; i < num_rows; ++i) {
            keys[i] = dist(rng_);
        }
        return keys;
    }
    
    // 生成probe side数据（HashJoin probe端）
    vector<int32_t> generateProbeSideKeys(int num_rows, int build_cardinality, double match_ratio) {
        vector<int32_t> keys(num_rows);
        uniform_real_distribution<double> prob_dist(0.0, 1.0);
        uniform_int_distribution<int32_t> build_dist(0, build_cardinality - 1);
        uniform_int_distribution<int32_t> random_dist(build_cardinality, build_cardinality * 10);
        
        for (int i = 0; i < num_rows; ++i) {
            if (prob_dist(rng_) < match_ratio) {
                keys[i] = build_dist(rng_);  // 可以匹配build端
            } else {
                keys[i] = random_dist(rng_);  // 不匹配
            }
        }
        return keys;
    }
    
    // 生成多层嵌套表达式数据
    vector<string> generateExpressionInputs(int num_rows) {
        vector<string> inputs(num_rows);
        uniform_int_distribution<int> len_dist(5, 50);
        uniform_int_distribution<int> char_dist('a', 'z');
        
        for (int i = 0; i < num_rows; ++i) {
            int len = len_dist(rng_);
            inputs[i].reserve(len);
            for (int j = 0; j < len; ++j) {
                inputs[i] += static_cast<char>(char_dist(rng_));
            }
        }
        return inputs;
    }
    
    // 生成编码向量（模拟dictionary/constant encoding）
    vector<int32_t> generateDictionaryIndices(int num_rows, int dictionary_size) {
        vector<int32_t> indices(num_rows);
        uniform_int_distribution<int32_t> dist(0, dictionary_size - 1);
        for (int i = 0; i < num_rows; ++i) {
            indices[i] = dist(rng_);
        }
        return indices;
    }
};

class Timer {
public:
    chrono::high_resolution_clock::time_point start_;
    
    Timer() : start_(chrono::high_resolution_clock::now()) {}
    
    double elapsedMicroseconds() const {
        auto end = chrono::high_resolution_clock::now();
        return chrono::duration_cast<chrono::microseconds>(end - start_).count();
    }
    
    double elapsedMilliseconds() const {
        return elapsedMicroseconds() / 1000.0;
    }
    
    void reset() {
        start_ = chrono::high_resolution_clock::now();
    }
};

class MemoryTracker {
private:
    size_t allocated_bytes_;
    int64_t num_allocations_;
    
public:
    MemoryTracker() : allocated_bytes_(0), num_allocations_(0) {}
    
    void recordAllocation(size_t bytes) {
        allocated_bytes_ += bytes;
        num_allocations_++;
    }
    
    double usedMB() const {
        return allocated_bytes_ / 1024.0 / 1024.0;
    }
    
    int64_t numAllocations() const {
        return num_allocations_;
    }
    
    void reset() {
        allocated_bytes_ = 0;
        num_allocations_ = 0;
    }
};

} // namespace velox_benchmark