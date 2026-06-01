#pragma once

#include <vector>
#include <string>
#include <random>
#include <chrono>
#include <iostream>
#include <functional>
#include <map>
#include <cstring>

#ifdef __ARM_FEATURE_SVE
#include <arm_sve.h>
#endif

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

namespace velox_benchmark {

using namespace std;

struct BenchmarkConfig {
    int data_scale;          // 数据规模（K/M/B）
    int iterations;          // 迭代次数
    int vector_width;        // 向量宽度（128/256/512）
    double selectivity;      // 选择率（0.0-1.0）
    double null_ratio;       // null比例（0.0-1.0）
    string data_type;        // 数据类型（int32/int64/float/double/string）
    string distribution;     // 数据分布（uniform/zipfian/sequential）
    bool verify_correctness; // 是否验证正确性
    bool verbose_output;     // 详细输出
    
    BenchmarkConfig() {
        data_scale = 1000000;  // 1M
        iterations = 1000;
        vector_width = 256;
        selectivity = 0.5;
        null_ratio = 0.1;
        data_type = "int32";
        distribution = "uniform";
        verify_correctness = true;
        verbose_output = false;
    }
};

struct BenchmarkResult {
    string benchmark_name;
    string implementation;
    double time_us;
    double throughput_mbps;
    int64_t items_processed;
    bool correctness_passed;
    double speedup_vs_scalar;
    map<string, double> metrics;
    
    void print() const {
        cout << "=== " << benchmark_name << " (" << implementation << ") ===" << endl;
        cout << "Time: " << time_us << " us" << endl;
        cout << "Throughput: " << throughput_mbps << " MB/s" << endl;
        cout << "Items processed: " << items_processed << endl;
        if (verify_correctness) {
            cout << "Correctness: " << (correctness_passed ? "PASS" : "FAIL") << endl;
        }
        if (speedup_vs_scalar > 0) {
            cout << "Speedup vs Scalar: " << speedup_vs_scalar << "x" << endl;
        }
        for (const auto& metric : metrics) {
            cout << metric.first << ": " << metric.second << endl;
        }
        cout << endl;
    }
};

class DataGenerator {
public:
    mt19937 rng_;
    uniform_int_distribution<int32_t> uniform_int32_dist_;
    uniform_int_distribution<int64_t> uniform_int64_dist_;
    uniform_real_distribution<double> uniform_double_dist_;
    
    DataGenerator(int seed = 42) : rng_(seed), 
        uniform_int32_dist_(0, 1000000),
        uniform_int64_dist_(0, 1000000000L),
        uniform_double_dist_(0.0, 1000000.0) {}
    
    vector<int32_t> generateInt32Data(int n, const string& distribution) {
        vector<int32_t> data(n);
        if (distribution == "uniform") {
            for (int i = 0; i < n; ++i) {
                data[i] = uniform_int32_dist_(rng_);
            }
        } else if (distribution == "sequential") {
            for (int i = 0; i < n; ++i) {
                data[i] = i;
            }
        } else if (distribution == "zipfian") {
            // 简化的zipfian分布
            for (int i = 0; i < n; ++i) {
                double zipf_val = 1.0 / (1.0 + i % 100);
                data[i] = static_cast<int32_t>(zipf_val * 1000000);
            }
        }
        return data;
    }
    
    vector<int64_t> generateInt64Data(int n, const string& distribution) {
        vector<int64_t> data(n);
        if (distribution == "uniform") {
            for (int i = 0; i < n; ++i) {
                data[i] = uniform_int64_dist_(rng_);
            }
        } else if (distribution == "sequential") {
            for (int i = 0; i < n; ++i) {
                data[i] = i;
            }
        }
        return data;
    }
    
    vector<uint64_t> generateBitmap(int n, double selectivity) {
        vector<uint64_t> bitmap((n + 63) / 64, 0);
        uniform_real_distribution<double> prob_dist(0.0, 1.0);
        
        for (int i = 0; i < n; ++i) {
            if (prob_dist(rng_) < selectivity) {
                bitmap[i / 64] |= (1ULL << (i % 64));
            }
        }
        return bitmap;
    }
    
    vector<int32_t> generateIndices(int n, int max_index, const string& distribution) {
        vector<int32_t> indices(n);
        if (distribution == "uniform") {
            uniform_int_distribution<int32_t> index_dist(0, max_index - 1);
            for (int i = 0; i < n; ++i) {
                indices[i] = index_dist(rng_);
            }
        } else if (distribution == "sequential") {
            for (int i = 0; i < n; ++i) {
                indices[i] = i % max_index;
            }
        } else if (distribution == "dense") {
            // 连续密集索引（模拟HashJoin中的密集访问）
            int start = uniform_int_dist_(rng_) % (max_index - n);
            for (int i = 0; i < n; ++i) {
                indices[i] = start + i;
            }
        }
        return indices;
    }
    
    vector<string> generateStrings(int n, int min_len, int max_len) {
        vector<string> strings(n);
        uniform_int_distribution<int> len_dist(min_len, max_len);
        uniform_int_distribution<int> char_dist('A', 'Z');
        
        for (int i = 0; i < n; ++i) {
            int len = len_dist(rng_);
            strings[i].reserve(len);
            for (int j = 0; j < len; ++j) {
                strings[i] += static_cast<char>(char_dist(rng_));
            }
        }
        return strings;
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
};

class ModuleBenchmark {
protected:
    BenchmarkConfig config_;
    DataGenerator generator_;
    vector<BenchmarkResult> results_;
    
public:
    ModuleBenchmark(const BenchmarkConfig& cfg) : config_(cfg), generator_(42) {}
    
    virtual void run() = 0;
    virtual void printResults() {
        for (const auto& result : results_) {
            result.print();
        }
    }
    
    void addResult(const BenchmarkResult& result) {
        results_.push_back(result);
    }
    
    BenchmarkResult computeResult(
        const string& name,
        const string& impl,
        double time_us,
        int64_t items,
        bool correctness,
        double scalar_time = 0.0) {
        
        BenchmarkResult result;
        result.benchmark_name = name;
        result.implementation = impl;
        result.time_us = time_us;
        result.items_processed = items;
        result.correctness_passed = correctness;
        result.throughput_mbps = (items * sizeof(int32_t) / 1024.0 / 1024.0) / (time_us / 1000000.0);
        if (scalar_time > 0) {
            result.speedup_vs_scalar = scalar_time / time_us;
        }
        return result;
    }
};

} // namespace velox_benchmark