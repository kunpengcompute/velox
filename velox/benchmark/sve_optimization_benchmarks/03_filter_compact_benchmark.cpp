#include <iostream>
#include <vector>
#include <chrono>
#include <random>

#ifdef __ARM_FEATURE_SVE
#include <arm_sve.h>
#endif

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

constexpr int VECTOR_SIZE = 256;  // 256-bit SVE处理的数据量
constexpr int ITERATIONS = 10000;

// 基准测试：Scalar filter实现
int scalar_filter(const int32_t* data, int mask, int32_t* result) {
    int count = 0;
    for (int i = 0; i < VECTOR_SIZE; ++i) {
        if (mask & (1 << i)) {
            result[count++] = data[i];
        }
    }
    return count;
}

#ifdef __ARM_NEON
// NEON filter实现（需要多次调用处理128-bit块）
int neon_filter_block(const int32_t* data, uint8_t mask_byte, int32_t* result) {
    int count = 0;
    int32x4_t vec = vld1q_s32(data);
    
    for (int bit = 0; bit < 8; ++bit) {
        if (mask_byte & (1 << bit)) {
            result[count++] = vgetq_lane_s32(vec, bit);
        }
    }
    return count;
}

int neon_filter(const int32_t* data, int mask, int32_t* result) {
    int count = 0;
    for (int block = 0; block < VECTOR_SIZE / 4; ++block) {
        uint8_t mask_byte = (mask >> (block * 4)) & 0xF;
        count += neon_filter_block(data + block * 4, mask_byte, result + count);
    }
    return count;
}
#endif

#ifdef __ARM_FEATURE_SVE
// SVE优化：使用svcompact指令（需要SVE2）
#ifdef __ARM_FEATURE_SVE2
int sve2_filter(const int32_t* data, int mask, int32_t* result) {
    svbool_t pg = svptrue_b32();
    svint32_t vec_data = svld1_s32(pg, data);
    
    // 从mask构造predicate
    svbool_t pred = svptrue_b32();
    int active_count = 0;
    
    // 使用compact指令压缩active元素
    svint32_t compacted = svcompact_s32(pred, vec_data);
    
    // 只存储需要的数量
    int count = svcntp_b32(pg, pred);
    svst1_s32(svwhilelt_b32(0, count), result, compacted);
    
    return count;
}
#else
// SVE（非SVE2）：逐元素处理但使用predicate
int sve_filter(const int32_t* data, int mask, int32_t* result) {
    int count = 0;
    int i = 0;
    
    while (i < VECTOR_SIZE) {
        svbool_t pg = svwhilelt_b32(i, VECTOR_SIZE);
        svint32_t vec = svld1_s32(pg, data + i);
        
        int vec_size = svcntw();
        for (int j = 0; j < vec_size && i + j < VECTOR_SIZE; ++j) {
            if (mask & (1 << (i + j))) {
                result[count++] = svlastb_s32(pg, vec);
            }
        }
        i += vec_size;
    }
    return count;
}
#endif
#endif

int main() {
    std::vector<int32_t> data(VECTOR_SIZE);
    std::vector<int32_t> result_scalar(VECTOR_SIZE);
    std::vector<int32_t> result_neon(VECTOR_SIZE);
    std::vector<int32_t> result_sve(VECTOR_SIZE);
    
    std::mt19937 rng(42);
    std::uniform_int_distribution<int32_t> dist(1, 1000);
    std::uniform_int_distribution<int> mask_dist(0, (1 << VECTOR_SIZE) - 1);
    
    for (int i = 0; i < VECTOR_SIZE; ++i) {
        data[i] = dist(rng);
    }
    
    int test_mask = 0xAAAAAAAA;  // 约50%元素被选中
    
    auto start = std::chrono::high_resolution_clock::now();
    int scalar_count = 0;
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        scalar_count = scalar_filter(data.data(), test_mask, result_scalar.data());
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto scalar_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    
#ifdef __ARM_NEON
    start = std::chrono::high_resolution_clock::now();
    int neon_count = 0;
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        neon_count = neon_filter(data.data(), test_mask, result_neon.data());
    }
    end = std::chrono::high_resolution_clock::now();
    auto neon_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
#endif
    
#ifdef __ARM_FEATURE_SVE
    start = std::chrono::high_resolution_clock::now();
    int sve_count = 0;
    for (int iter = 0; iter < ITERATIONS; ++iter) {
#ifdef __ARM_FEATURE_SVE2
        sve_count = sve2_filter(data.data(), test_mask, result_sve.data());
#else
        sve_count = sve_filter(data.data(), test_mask, result_sve.data());
#endif
    }
    end = std::chrono::high_resolution_clock::now();
    auto sve_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
#endif
    
    std::cout << "=== Filter/Compact Benchmark ===" << std::endl;
    std::cout << "Test mask: " << test_mask << " (selected " << __builtin_popcount(test_mask) << " elements)" << std::endl;
    std::cout << "Scalar time: " << scalar_time << " us, count=" << scalar_count << std::endl;
    
#ifdef __ARM_NEON
    std::cout << "NEON time: " << neon_time << " us, count=" << neon_count << std::endl;
    std::cout << "NEON vs Scalar: " << (double)scalar_time / neon_time << "x" << std::endl;
#endif
    
#ifdef __ARM_FEATURE_SVE
    std::cout << "SVE time: " << sve_time << " us, count=" << sve_count << std::endl;
    std::cout << "SVE vs Scalar: " << (double)scalar_time / sve_time << "x" << std::endl;
    
    if (scalar_count == sve_count) {
        bool correct = true;
        for (int i = 0; i < scalar_count; ++i) {
            if (result_scalar[i] != result_sve[i]) {
                correct = false;
                std::cout << "Mismatch at " << i << std::endl;
                break;
            }
        }
        std::cout << "Correctness: " << (correct ? "PASS" : "FAIL") << std::endl;
    } else {
        std::cout << "Correctness: FAIL (count mismatch)" << std::endl;
    }
#endif
    
    return 0;
}