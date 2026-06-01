#include <iostream>
#include <vector>
#include <chrono>
#include <cstring>
#include <random>

#ifdef __ARM_FEATURE_SVE
#include <arm_sve.h>
#endif

constexpr int VECTOR_SIZE = 8;  // 256-bit SVE处理8个int32
constexpr int DATA_SIZE = 1024 * 1024;  // 1M元素
constexpr int ITERATIONS = 1000;

// 基准测试：Generic gather（逐元素加载）
void generic_gather(const int32_t* base, const int32_t* indices, int32_t* result, int n) {
    for (int i = 0; i < n; ++i) {
        result[i] = base[indices[i]];
    }
}

#ifdef __ARM_FEATURE_SVE
// SVE优化：使用gather指令
void sve_gather(const int32_t* base, const int32_t* indices, int32_t* result, int n) {
    int i = 0;
    svbool_t pg = svptrue_b32();
    
    while (i < n) {
        svbool_t pg_current = svwhilelt_b32(i, n);
        svint32_t vindex = svld1_s32(pg_current, indices + i);
        svint32_t vdata = svld1_gather_s32index_s32(pg_current, base, vindex);
        svst1_s32(pg_current, result + i, vdata);
        i += svcntw();  // svcntw()返回当前SVE向量能处理的int32元素数量
    }
}
#endif

int main() {
    std::vector<int32_t> base_data(DATA_SIZE);
    std::vector<int32_t> indices(VECTOR_SIZE * ITERATIONS);
    std::vector<int32_t> result_generic(VECTOR_SIZE * ITERATIONS);
    std::vector<int32_t> result_sve(VECTOR_SIZE * ITERATIONS);
    
    std::mt19937 rng(42);
    std::uniform_int_distribution<int32_t> dist(0, DATA_SIZE - 1);
    
    for (int i = 0; i < DATA_SIZE; ++i) {
        base_data[i] = i * 100;
    }
    
    for (int i = 0; i < indices.size(); ++i) {
        indices[i] = dist(rng);
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        generic_gather(base_data.data(), indices.data() + iter * VECTOR_SIZE, 
                      result_generic.data() + iter * VECTOR_SIZE, VECTOR_SIZE);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto generic_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    
#ifdef __ARM_FEATURE_SVE
    start = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        sve_gather(base_data.data(), indices.data() + iter * VECTOR_SIZE, 
                  result_sve.data() + iter * VECTOR_SIZE, VECTOR_SIZE);
    }
    end = std::chrono::high_resolution_clock::now();
    auto sve_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    
    bool correct = true;
    for (int i = 0; i < result_generic.size(); ++i) {
        if (result_generic[i] != result_sve[i]) {
            correct = false;
            std::cout << "Mismatch at " << i << ": generic=" << result_generic[i] 
                     << " sve=" << result_sve[i] << std::endl;
            break;
        }
    }
    
    std::cout << "=== Gather Benchmark ===" << std::endl;
    std::cout << "Generic time: " << generic_time << " us" << std::endl;
    std::cout << "SVE time: " << sve_time << " us" << std::endl;
    std::cout << "Speedup: " << (double)generic_time / sve_time << "x" << std::endl;
    std::cout << "Correctness: " << (correct ? "PASS" : "FAIL") << std::endl;
#else
    std::cout << "=== Gather Benchmark ===" << std::endl;
    std::cout << "Generic time: " << generic_time << " us" << std::endl;
    std::cout << "SVE not available on this platform" << std::endl;
#endif
    
    return 0;
}