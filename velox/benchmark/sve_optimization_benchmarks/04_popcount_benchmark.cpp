#include <iostream>
#include <vector>
#include <chrono>
#include <random>

#ifdef __ARM_FEATURE_SVE
#include <arm_sve.h>
#endif

constexpr int BITMAP_WORDS = 1024;  // 64K bits
constexpr int ITERATIONS = 10000;

// 基准测试：Scalar popcount
int scalar_popcount(const uint64_t* bits, int n) {
    int count = 0;
    for (int i = 0; i < n; ++i) {
        count += __builtin_popcountll(bits[i]);
    }
    return count;
}

#ifdef __ARM_FEATURE_SVE
// SVE优化：并行位计数
int sve_popcount(const uint64_t* bits, int n) {
    int count = 0;
    int i = 0;
    
    while (i < n) {
        svbool_t pg = svwhilelt_b64(i, n);
        svuint64_t vec = svld1_u64(pg, bits + i);
        
        // 方法1：使用svcntp计算非零元素（近似）
        svbool_t nonzero = svcmpne_n_u64(pg, vec, 0);
        int nonzero_count = svcntp_b64(pg, nonzero);
        
        // 方法2：逐元素popcount但使用SVE向量
        int vec_size = svcntd();
        for (int j = 0; j < vec_size && i + j < n; ++j) {
            uint64_t val = svlastb_u64(pg, vec);
            if (val != 0) {
                count += __builtin_popcountll(val);
            }
        }
        i += vec_size;
    }
    
    return count;
}

// SVE2优化：使用真正的并行popcount（如果有SVE2 bit manipulation）
#ifdef __ARM_FEATURE_SVE2
int sve2_popcount_parallel(const uint64_t* bits, int n) {
    int total_count = 0;
    int i = 0;
    
    while (i < n) {
        svbool_t pg = svwhilelt_b64(i, n);
        svuint64_t vec = svld1_u64(pg, bits + i);
        
        // SVE2可以直接处理位操作
        // 计算每个元素的popcount并累加
        // 注意：这里简化实现，实际SVE2可能有更高效的指令
        
        int vec_size = svcntd();
        for (int j = 0; j < vec_size && i + j < n; ++j) {
            uint64_t val = bits[i + j];
            if (val != 0) {
                total_count += __builtin_popcountll(val);
            }
        }
        i += vec_size;
    }
    
    return total_count;
}
#endif
#endif

int main() {
    std::vector<uint64_t> bits(BITMAP_WORDS);
    
    std::mt19937 rng(42);
    std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);
    
    int expected_count = 0;
    for (int i = 0; i < BITMAP_WORDS; ++i) {
        bits[i] = dist(rng);
        expected_count += __builtin_popcountll(bits[i]);
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    int scalar_result = 0;
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        scalar_result = scalar_popcount(bits.data(), BITMAP_WORDS);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto scalar_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    
#ifdef __ARM_FEATURE_SVE
    start = std::chrono::high_resolution_clock::now();
    int sve_result = 0;
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        sve_result = sve_popcount(bits.data(), BITMAP_WORDS);
    }
    end = std::chrono::high_resolution_clock::now();
    auto sve_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    
#ifdef __ARM_FEATURE_SVE2
    start = std::chrono::high_resolution_clock::now();
    int sve2_result = 0;
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        sve2_result = sve2_popcount_parallel(bits.data(), BITMAP_WORDS);
    }
    end = std::chrono::high_resolution_clock::now();
    auto sve2_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
#endif
#endif
    
    std::cout << "=== Popcount Benchmark ===" << std::endl;
    std::cout << "Expected count: " << expected_count << std::endl;
    std::cout << "Scalar result: " << scalar_result << ", time: " << scalar_time << " us" << std::endl;
    
#ifdef __ARM_FEATURE_SVE
    std::cout << "SVE result: " << sve_result << ", time: " << sve_time << " us" << std::endl;
    std::cout << "SVE vs Scalar: " << (double)scalar_time / sve_time << "x" << std::endl;
    std::cout << "Correctness: " << ((sve_result == expected_count) ? "PASS" : "FAIL") << std::endl;
    
#ifdef __ARM_FEATURE_SVE2
    std::cout << "SVE2 result: " << sve2_result << ", time: " << sve2_time << " us" << std::endl;
    std::cout << "SVE2 vs Scalar: " << (double)scalar_time / sve2_time << "x" << std::endl;
#endif
#else
    std::cout << "SVE not available on this platform" << std::endl;
#endif
    
    return 0;
}