#include <iostream>
#include <vector>
#include <chrono>
#include <cstring>

#ifdef __ARM_FEATURE_SVE
#include <arm_sve.h>
#endif

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

constexpr int TAG_SIZE = 16;  // 128-bit向量（16个字节）
constexpr int NUM_TAGS = 1024 * 1024;  // 1M标签
constexpr int ITERATIONS = 10000;

// 基准测试：Scalar逐字节加载
void scalar_load(const uint8_t* tags, uint8_t* result, int n) {
    for (int i = 0; i < n; ++i) {
        result[i] = tags[i];
    }
}

#ifdef __ARM_NEON
// NEON实现
void neon_load(const uint8_t* tags, uint8_t* result, int n) {
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        uint8x16_t vec = vld1q_u8(tags + i);
        vst1q_u8(result + i, vec);
    }
    for (; i < n; ++i) {
        result[i] = tags[i];
    }
}
#endif

#ifdef __ARM_FEATURE_SVE
// SVE实现：使用predicate处理任意长度
void sve_load(const uint8_t* tags, uint8_t* result, int n) {
    int i = 0;
    while (i < n) {
        svbool_t pg = svwhilelt_b8(i, n);
        svuint8_t vec = svld1_u8(pg, tags + i);
        svst1_u8(pg, result + i, vec);
        i += svcntb();
    }
}
#endif

int main() {
    std::vector<uint8_t> tags(NUM_TAGS);
    std::vector<uint8_t> result_scalar(NUM_TAGS);
    std::vector<uint8_t> result_neon(NUM_TAGS);
    std::vector<uint8_t> result_sve(NUM_TAGS);
    
    for (int i = 0; i < NUM_TAGS; ++i) {
        tags[i] = (uint8_t)(i % 256);
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        scalar_load(tags.data(), result_scalar.data(), NUM_TAGS);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto scalar_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    
#ifdef __ARM_NEON
    start = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        neon_load(tags.data(), result_neon.data(), NUM_TAGS);
    }
    end = std::chrono::high_resolution_clock::now();
    auto neon_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
#endif
    
#ifdef __ARM_FEATURE_SVE
    start = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        sve_load(tags.data(), result_sve.data(), NUM_TAGS);
    }
    end = std::chrono::high_resolution_clock::now();
    auto sve_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
#endif
    
    std::cout << "=== Tag Vector Load Benchmark ===" << std::endl;
    std::cout << "Scalar time: " << scalar_time << " us" << std::endl;
    
#ifdef __ARM_NEON
    std::cout << "NEON time: " << neon_time << " us" << std::endl;
    std::cout << "NEON vs Scalar: " << (double)scalar_time / neon_time << "x" << std::endl;
#endif
    
#ifdef __ARM_FEATURE_SVE
    std::cout << "SVE time: " << sve_time << " us" << std::endl;
    std::cout << "SVE vs Scalar: " << (double)scalar_time / sve_time << "x" << std::endl;
    
    bool correct = true;
    for (int i = 0; i < NUM_TAGS; ++i) {
        if (result_scalar[i] != result_sve[i]) {
            correct = false;
            break;
        }
    }
    std::cout << "Correctness: " << (correct ? "PASS" : "FAIL") << std::endl;
#endif
    
    return 0;
}