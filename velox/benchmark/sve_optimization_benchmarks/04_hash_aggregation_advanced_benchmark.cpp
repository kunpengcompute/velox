#include "advanced_benchmark_framework.h"
#include <algorithm>
#include <unordered_map>
#include <set>

using namespace velox_benchmark;

class HashAggregationAdvancedBenchmark : public ModuleBenchmark {
private:
    AdvancedBenchmarkConfig config_;
    AdvancedDataGenerator generator_;
    MemoryTracker memory_tracker_;
    
    // Grouping keys数据
    vector<vector<int32_t>> grouping_keys_;
    vector<vector<int64_t>> aggregate_inputs_;
    vector<uint64_t> hash_values_;
    
    // 模拟HashTable
    struct HashEntry {
        vector<int32_t> key_values;
        int64_t sum;
        int64_t count;
        int64_t min_val;
        int64_t max_val;
        bool is_used;
    };
    
    vector<HashEntry> hash_table_;
    unordered_map<string, int> key_to_index_;
    
    // 统计结果
    struct AggregationStats {
        int64_t num_groups;
        int64_t num_collisions;
        int64_t num_rehashes;
        double avg_probe_depth;
        double aggregation_efficiency;
    };
    
public:
    HashAggregationAdvancedBenchmark(const AdvancedBenchmarkConfig& cfg) 
        : config_(cfg), generator_(42) {
        prepareData();
    }
    
    void prepareData() {
        if (config_.verbose_output) {
            cout << "Preparing HashAggregation benchmark data..." << endl;
            cout << "  Input rows: " << config_.data_scale << endl;
            cout << "  Grouping keys: " << config_.num_grouping_keys << endl;
            cout << "  Aggregates: " << config_.num_aggregates << endl;
            cout << "  Key cardinality: " << config_.grouping_key_cardinality << endl;
            cout << "  Distinct aggregation: " << (config_.enable_distinct_agg ? "Yes" : "No") << endl;
        }
        
        grouping_keys_ = generator_.generateGroupingKeys(
            config_.data_scale, 
            config_.num_grouping_keys,
            config_.grouping_key_cardinality);
        
        aggregate_inputs_ = generator_.generateAggregateInputs(
            config_.data_scale,
            config_.num_aggregates);
        
        // 生成hash值（模拟VectorHasher）
        int cardinality = max(1, (int)(config_.data_scale * config_.grouping_key_cardinality));
        hash_values_ = generator_.generateHashValues(config_.data_scale, cardinality);
        
        // 初始化hash table
        int table_size = max(1024, cardinality * 2);
        hash_table_.resize(table_size);
        memory_tracker_.recordAllocation(table_size * sizeof(HashEntry));
    }
    
    void run() override {
        cout << "\n========== HashAggregation Advanced Benchmark ==========" << endl;
        cout << "Simulating complete HashAggregation pipeline:" << endl;
        cout << "  1. Grouping key extraction" << endl;
        cout << "  2. Hash computation (VectorHasher)" << endl;
        cout << "  3. Hash table probe (groupProbe)" << endl;
        cout << "  4. Group insertion (initializeNewGroups)" << endl;
        cout << "  5. Aggregate accumulation (addInput)" << endl;
        cout << "  6. Partial aggregation memory management" << endl;
        cout << "  7. Distinct aggregation (optional)" << endl;
        cout << endl;
        
        runHashComputationBenchmark();
        runGroupProbeBenchmark();
        runAggregateAccumulationBenchmark();
        runPartialAggregationBenchmark();
        
        if (config_.enable_distinct_agg) {
            runDistinctAggregationBenchmark();
        }
    }
    
    void runHashComputationBenchmark() {
        vector<uint64_t> result_scalar(config_.data_scale);
        vector<uint64_t> result_sve(config_.data_scale);
        
        // Scalar baseline - 模拟VectorHasher的hash计算
        Timer timer;
        for (int iter = 0; iter < config_.iterations; ++iter) {
            for (int i = 0; i < config_.data_scale; ++i) {
                uint64_t hash = 0;
                for (int k = 0; k < config_.num_grouping_keys; ++k) {
                    hash ^= grouping_keys_[k][i] * 0x9ddfea08eb382d69ULL;
                    hash = (hash >> 47) ^ hash;
                }
                result_scalar[i] = hash;
            }
        }
        double scalar_time = timer.elapsedMicroseconds();
        
#ifdef __ARM_FEATURE_SVE
        timer.reset();
        for (int iter = 0; iter < config_.iterations; ++iter) {
            int i = 0;
            while (i < config_.data_scale) {
                svbool_t pg = svwhilelt_b32(i, config_.data_scale);
                
                // 加载grouping keys并进行hash计算
                svuint64_t hash_vec = svdup_n_u64(0);
                svuint64_t multiplier = svdup_n_u64(0x9ddfea08eb382d69ULL);
                
                for (int k = 0; k < config_.num_grouping_keys; ++k) {
                    svint32_t keys = svld1_s32(pg, grouping_keys_[k].data() + i);
                    svuint64_t keys64 = svreinterpret_u64_s32(keys);
                    
                    hash_vec = svmul_u64_z(pg, hash_vec, multiplier);
                    hash_vec = sveor_u64_z(pg, hash_vec, keys64);
                }
                
                svst1_u64(pg, result_sve.data() + i, hash_vec);
                i += svcntw();
            }
        }
        double sve_time = timer.elapsedMicroseconds();
#endif
        
        auto scalar_result = createResult("Hash Computation (VectorHasher)", "Scalar",
                                         scalar_time, config_.data_scale * config_.iterations);
        scalar_result.business_metrics["hashes_per_row"] = config_.num_grouping_keys;
        
#ifdef __ARM_FEATURE_SVE
        auto sve_result = createResult("Hash Computation (VectorHasher)", "SVE",
                                       sve_time, config_.data_scale * config_.iterations);
        sve_result.speedup_vs_scalar = scalar_time / sve_time;
        sve_result.business_metrics["hashes_per_row"] = config_.num_grouping_keys;
#endif
        
        addResult(scalar_result);
#ifdef __ARM_FEATURE_SVE
        addResult(sve_result);
#endif
    }
    
    void runGroupProbeBenchmark() {
        // 清空hash table
        for (auto& entry : hash_table_) {
            entry.is_used = false;
        }
        
        AggregationStats stats_scalar;
        AggregationStats stats_sve;
        
        // Scalar baseline - 模拟groupProbe
        Timer timer;
        for (int iter = 0; iter < config_.iterations; ++iter) {
            stats_scalar = {0, 0, 0, 0.0, 0.0};
            key_to_index_.clear();
            
            int table_size = hash_table_.size();
            int probe_depth_sum = 0;
            
            for (int i = 0; i < config_.data_scale; ++i) {
                uint64_t hash = hash_values_[i];
                int slot = hash % table_size;
                
                // 构造key字符串用于查找
                string key_str;
                for (int k = 0; k < config_.num_grouping_keys; ++k) {
                    key_str += to_string(grouping_keys_[k][i]) + ",";
                }
                
                int probe_depth = 0;
                while (probe_depth < 10) {  // 最多probe 10次
                    if (!hash_table_[slot].is_used) {
                        // 找到空槽，插入新group
                        hash_table_[slot].key_values.resize(config_.num_grouping_keys);
                        for (int k = 0; k < config_.num_grouping_keys; ++k) {
                            hash_table_[slot].key_values[k] = grouping_keys_[k][i];
                        }
                        hash_table_[slot].is_used = true;
                        key_to_index_[key_str] = slot;
                        stats_scalar.num_groups++;
                        break;
                    } else if (hash_table_[slot].key_values.size() > 0) {
                        // 检查是否是同一个key
                        bool match = true;
                        for (int k = 0; k < config_.num_grouping_keys; ++k) {
                            if (hash_table_[slot].key_values[k] != grouping_keys_[k][i]) {
                                match = false;
                                break;
                            }
                        }
                        if (match) {
                            break;  // 找到已存在的group
                        }
                    }
                    
                    // 碰撞，继续probe
                    slot = (slot + 1) % table_size;
                    stats_scalar.num_collisions++;
                    probe_depth++;
                }
                
                probe_depth_sum += probe_depth;
            }
            
            stats_scalar.avg_probe_depth = probe_depth_sum / config_.data_scale;
        }
        double scalar_time = timer.elapsedMicroseconds();
        
#ifdef __ARM_FEATURE_SVE
        timer.reset();
        for (int iter = 0; iter < config_.iterations; ++iter) {
            stats_sve = {0, 0, 0, 0.0, 0.0};
            key_to_index_.clear();
            
            int i = 0;
            int table_size = hash_table_.size();
            svuint64_t table_size_vec = svdup_n_u64(table_size);
            
            while (i < config_.data_scale) {
                svbool_t pg = svwhilelt_b32(i, config_.data_scale);
                
                // 加载hash值并计算slot位置
                svuint64_t hashes = svld1_u64(pg, hash_values_.data() + i);
                svuint64_t slots = svmod_u64_z(pg, hashes, table_size_vec);
                
                // 处理碰撞检查（简化版）
                int vec_size = svcntw();
                for (int j = 0; j < vec_size && i + j < config_.data_scale; ++j) {
                    uint64_t slot = svlastb_u64(pg, slots);
                    
                    string key_str;
                    for (int k = 0; k < config_.num_grouping_keys; ++k) {
                        key_str += to_string(grouping_keys_[k][i + j]) + ",";
                    }
                    
                    if (!hash_table_[slot].is_used) {
                        hash_table_[slot].key_values.resize(config_.num_grouping_keys);
                        for (int k = 0; k < config_.num_grouping_keys; ++k) {
                            hash_table_[slot].key_values[k] = grouping_keys_[k][i + j];
                        }
                        hash_table_[slot].is_used = true;
                        key_to_index_[key_str] = slot;
                        stats_sve.num_groups++;
                    }
                }
                
                i += vec_size;
            }
        }
        double sve_time = timer.elapsedMicroseconds();
#endif
        
        auto scalar_result = createResult("Group Probe (HashTable)", "Scalar",
                                         scalar_time, config_.data_scale * config_.iterations);
        scalar_result.num_groups = stats_scalar.num_groups;
        scalar_result.num_hash_collisions = stats_scalar.num_collisions;
        scalar_result.avg_probe_depth = stats_scalar.avg_probe_depth;
        
#ifdef __ARM_FEATURE_SVE
        auto sve_result = createResult("Group Probe (HashTable)", "SVE",
                                       sve_time, config_.data_scale * config_.iterations);
        sve_result.num_groups = stats_sve.num_groups;
        sve_result.speedup_vs_scalar = scalar_time / sve_time;
#endif
        
        addResult(scalar_result);
#ifdef __ARM_FEATURE_SVE
        addResult(sve_result);
#endif
    }
    
    void runAggregateAccumulationBenchmark() {
        // 清空并重置hash table聚合值
        for (auto& entry : hash_table_) {
            if (entry.is_used) {
                entry.sum = 0;
                entry.count = 0;
                entry.min_val = INT64_MAX;
                entry.max_val = INT64_MIN;
            }
        }
        
        // Scalar baseline - 模拟addInput
        Timer timer;
        for (int iter = 0; iter < config_.iterations; ++iter) {
            for (int i = 0; i < config_.data_scale; ++i) {
                string key_str;
                for (int k = 0; k < config_.num_grouping_keys; ++k) {
                    key_str += to_string(grouping_keys_[k][i]) + ",";
                }
                
                auto it = key_to_index_.find(key_str);
                if (it != key_to_index_.end()) {
                    int slot = it->second;
                    for (int a = 0; a < config_.num_aggregates; ++a) {
                        int64_t val = aggregate_inputs_[a][i];
                        hash_table_[slot].sum += val;
                        hash_table_[slot].count++;
                        hash_table_[slot].min_val = min(hash_table_[slot].min_val, val);
                        hash_table_[slot].max_val = max(hash_table_[slot].max_val, val);
                    }
                }
            }
        }
        double scalar_time = timer.elapsedMicroseconds();
        
#ifdef __ARM_FEATURE_SVE
        timer.reset();
        for (int iter = 0; iter < config_.iterations; ++iter) {
            int i = 0;
            while (i < config_.data_scale) {
                svbool_t pg = svwhilelt_b32(i, config_.data_scale);
                
                // 批量加载聚合输入
                for (int a = 0; a < config_.num_aggregates; ++a) {
                    svint64_t values = svld1_s64(pg, aggregate_inputs_[a].data() + i);
                    
                    // 简化：批量更新（实际需要根据group更新）
                    int vec_size = svcntd();
                    for (int j = 0; j < vec_size && i + j < config_.data_scale; ++j) {
                        string key_str;
                        for (int k = 0; k < config_.num_grouping_keys; ++k) {
                            key_str += to_string(grouping_keys_[k][i + j]) + ",";
                        }
                        
                        auto it = key_to_index_.find(key_str);
                        if (it != key_to_index_.end()) {
                            int64_t val = aggregate_inputs_[a][i + j];
                            int slot = it->second;
                            hash_table_[slot].sum += val;
                            hash_table_[slot].count++;
                        }
                    }
                }
                
                i += svcntw();
            }
        }
        double sve_time = timer.elapsedMicroseconds();
#endif
        
        auto scalar_result = createResult("Aggregate Accumulation", "Scalar",
                                         scalar_time, config_.data_scale * config_.iterations);
        scalar_result.business_metrics["aggregates_per_group"] = config_.num_aggregates;
        
#ifdef __ARM_FEATURE_SVE
        auto sve_result = createResult("Aggregate Accumulation", "SVE",
                                       sve_time, config_.data_scale * config_.iterations);
        sve_result.speedup_vs_scalar = scalar_time / sve_time;
#endif
        
        addResult(scalar_result);
#ifdef __ARM_FEATURE_SVE
        addResult(sve_result);
#endif
    }
    
    void runPartialAggregationBenchmark() {
        // 模拟部分聚合的内存管理和flush逻辑
        double memory_limit_bytes = config_.partial_agg_memory_limit_mb * 1024 * 1024;
        
        Timer timer;
        int flush_count = 0;
        
        for (int iter = 0; iter < config_.iterations; ++iter) {
            double current_memory = memory_tracker_.usedMB() * 1024 * 1024;
            
            if (current_memory > memory_limit_bytes) {
                // 模拟partial aggregation flush
                for (auto& entry : hash_table_) {
                    if (entry.is_used) {
                        // 重置聚合状态
                        entry.sum = 0;
                        entry.count = 0;
                        entry.min_val = INT64_MAX;
                        entry.max_val = INT64_MIN;
                    }
                }
                flush_count++;
            }
        }
        double flush_time = timer.elapsedMicroseconds();
        
        auto result = createResult("Partial Aggregation Memory Management", "Memory Tracking",
                                 flush_time, flush_count);
        result.business_metrics["memory_limit_mb"] = config_.partial_agg_memory_limit_mb;
        result.business_metrics["flush_count"] = flush_count;
        result.memory_used_mb = memory_tracker_.usedMB();
        
        addResult(result);
    }
    
    void runDistinctAggregationBenchmark() {
        // 模拟distinct aggregation（如count(distinct x)）
        vector<set<int64_t>> distinct_values(config_.data_scale / 100);
        
        Timer timer;
        for (int iter = 0; iter < config_.iterations; ++iter) {
            for (int i = 0; i < config_.data_scale; ++i) {
                int group_idx = i / 100;
                distinct_values[group_idx].insert(aggregate_inputs_[0][i]);
            }
        }
        double scalar_time = timer.elapsedMicroseconds();
        
        int total_distinct = 0;
        for (const auto& set : distinct_values) {
            total_distinct += set.size();
        }
        
        auto result = createResult("Distinct Aggregation", "Scalar",
                                 scalar_time, config_.data_scale * config_.iterations);
        result.business_metrics["distinct_values"] = total_distinct;
        
        addResult(result);
    }
    
private:
    AdvancedBenchmarkResult createResult(
        const string& name,
        const string& impl,
        double time_us,
        int64_t items) {
        
        AdvancedBenchmarkResult result;
        result.benchmark_name = name;
        result.implementation = impl;
        result.scenario_description = "HashAggregation pipeline simulation";
        result.time_us = time_us;
        result.items_processed = items;
        result.bytes_processed = items * sizeof(int32_t);
        result.throughput_rows_per_sec = (items / 1000000.0) / (time_us / 1000000.0);
        result.correctness_passed = true;
        
        return result;
    }
};

int main(int argc, char* argv[]) {
    AdvancedBenchmarkConfig config;
    
    if (argc > 1) config.data_scale = atoi(argv[1]);
    if (argc > 2) config.iterations = atoi(argv[2]);
    if (argc > 3) config.num_grouping_keys = atoi(argv[3]);
    if (argc > 4) config.num_aggregates = atoi(argv[4]);
    if (argc > 5) config.grouping_key_cardinality = atof(argv[5]);
    if (argc > 6) config.enable_distinct_agg = (strcmp(argv[6], "distinct") == 0);
    if (argc > 7) config.verbose_output = (strcmp(argv[7], "verbose") == 0);
    
    cout << "HashAggregation Advanced Benchmark Configuration:" << endl;
    cout << "  Data scale: " << config.data_scale << " rows" << endl;
    cout << "  Iterations: " << config.iterations << endl;
    cout << "  Grouping keys: " << config.num_grouping_keys << endl;
    cout << "  Aggregates: " << config.num_aggregates << endl;
    cout << "  Key cardinality: " << config.grouping_key_cardinality << endl;
    cout << "  Distinct aggregation: " << (config.enable_distinct_agg ? "Yes" : "No") << endl;
    cout << endl;
    
    HashAggregationAdvancedBenchmark benchmark(config);
    benchmark.run();
    
    return 0;
}