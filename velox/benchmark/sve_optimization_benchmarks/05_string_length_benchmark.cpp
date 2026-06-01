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

constexpr int STRING_LEN = 256;
constexpr int NUM_STRINGS = 10000;
constexpr int ITERATIONS = 1000;

std::vector<char> test_strings[NUM_STRINGS];

void init_strings() {
    for (int i = 0; i < NUM_STRINGS; ++i) {
        test_strings[i].resize(STRING_LEN);
        int actual_len = (i % 200) + 10;  // 长度在10-210之间变化
        for (int j = 0; j < actual_len; ++j) {
            test_strings[i][j] = 'A' + (j % 26);
        }
        test_strings[i][actual_len] = '\0';  // 添加终止符
    }
}

// 基准测试：Scalar strlen
size_t scalar_strlen_batch() {
    size_t total_len = 0;
    for (int i = 0; i < NUM_STRINGS; ++i) {
        total_len += strlen(test_strings[i].data());
    }
    return total_len;
}

#ifdef __ARM_NEON
// NEON优化：批量扫描（需要特殊处理）
size_t neon_strlen_batch() {
    size_t total_len = 0;
    
    for (int i = 0; i < NUM_STRINGS; ++i) {
        const char* str = test_strings[i].data();
        size_t len = 0;
        
        // NEON批量扫描寻找零终止符
        uint8x16_t zero_vec = vmovq_n_u8(0);
        
        while (len < STRING_LEN) {
            uint8x16_t chunk = vld1q_u8((const uint8_t*)(str + len));
            uint8x16_t cmp = vceqq_u8(chunk, zero_vec);
            
            // 检查是否有零
            uint64_t mask = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0) |
                           vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1);
            
            if (mask != 0) {
                // 找到零，需要定位位置
                for (int j = 0; j < 16; ++j) {
                    if (str[len + j] == '\0') {
                        total_len += len + j;
                        len = STRING_LEN;  // 结束循环
                        break;
                    }
                }
                break;
            }
            len += 16;
        }
        
        if (len >= STRING_LEN) {
            // NEON没找到零，使用传统方法
            total_len += strlen(str);
        }
    }
    
    return total_len;
}
#endif

#ifdef __ARM_FEATURE_SVE
// SVE优化：使用predicate和whilelt扫描
size_t sve_strlen_batch() {
    size_t total_len = 0;
    
    for (int i = 0; i < NUM_STRINGS; ++i) {
        const char* str = test_strings[i].data();
        size_t len = 0;
        
        svuint8_t zero_vec = svdup_n_u8(0);
        
        while (len < STRING_LEN) {
            svbool_t pg = svwhilelt_b8(len, STRING_LEN);
            svuint8_t chunk = svld1_u8(pg, (const uint8_t*)(str + len));
            
            // 比较是否等于零
            svbool_t is_zero = svcmpeq_u8(pg, chunk, zero_vec);
            
            // 检查是否有零
            if (svptest_any(pg, is_zero)) {
                // 找到第一个零的位置
                int first_zero = len;
                int vec_size = svcntb();
                
                for (int j = 0; j < vec_size && len + j < STRING_LEN; ++j) {
                    if (str[len + j] == '\0') {
                        first_zero = len + j;
                        break;
                    }
                }
                
                total_len += first_zero;
                break;
            }
            
            len += svcntb();
        }
        
        if (len >= STRING_LEN) {
            total_len += strlen(str);
        }
    }
    
    return total_len;
}

// SVE2优化：使用更高效的位操作
#ifdef __ARM_FEATURE_SVE2
size_t sve2_strlen_batch() {
    size_t total_len = 0;
    
    for (int i = 0; i < NUM_STRINGS; ++i) {
        const char* str = test_strings[i].data();
        
        // SVE2可以更高效地找到第一个零
        svuint8_t zero_vec = svdup_n_u8(0);
        svbool_t pg_all = svptrue_b8();
        
        // 加载整个字符串（predicate控制）
        int max_search = STRING_LEN;
        int found_pos = STRING_LEN;
        
        for (int offset = 0; offset < STRING_LEN; ) {
            svbool_t pg = svwhilelt_b8(offset, STRING_LEN);
            svuint8_t chunk = svld1_u8(pg, (const uint8_t*)(str + offset));
            svbool_t is_zero = svcmpeq_u8(pg, chunk, zero_vec);
            
            if (svptest_any(pg, is_zero)) {
                // 使用SVE2找到第一个匹配位置
                int vec_size = svcntb();
                for (int j = 0; j < vec_size && offset + j < STRING_LEN; ++j) {
                    if (str[offset + j] == '\0') {
                        found_pos = offset + j;
                        break;
                    }
                }
                break;
            }
            
            offset += svcntb();
        }
        
        total_len += (found_pos < STRING_LEN) ? found_pos : strlen(str);
    }
    
    return total_len;
}
#endif
#endif

int main() {
    init_strings();
    
    auto start = std::chrono::high_resolution_clock::now();
    size_t scalar_result = 0;
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        scalar_result = scalar_strlen_batch();
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto scalar_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    
#ifdef __ARM_NEON
    start = std::chrono::high_resolution_clock::now();
    size_t neon_result = 0;
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        neon_result = neon_strlen_batch();
    }
    end = std::chrono::high_resolution_clock::now();
    auto neon_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
#endif
    
#ifdef __ARM_FEATURE_SVE
    start = std::chrono::high_resolution_clock::now();
    size_t sve_result = 0;
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        sve_result = sve_strlen_batch();
    }
    end = std::chrono::high_resolution_clock::now();
    auto sve_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    
#ifdef __ARM_FEATURE_SVE2
    start = std::chrono::high_resolution_clock::now();
    size_t sve2_result = 0;
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        sve2_result = sve2_strlen_batch();
    }
    end = std::chrono::high_resolution_clock::now();
    auto sve2_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
#endif
#endif
    
    std::cout << "=== String Length Benchmark ===" << std::endl;
    std::cout << "Scalar total length: " << scalar_result << ", time: " << scalar_time << " us" << std::endl;
    
#ifdef __ARM_NEON
    std::cout << "NEON total length: " << neon_result << ", time: " << neon_time << " us" << std::endl;
    std::cout << "NEON vs Scalar: " << (double)scalar_time / neon_time << "x" << std::endl;
#endif
    
#ifdef __ARM_FEATURE_SVE
    std::cout << "SVE total length: " << sve_result << ", time: " << sve_time << " us" << std::endl;
    std::cout << "SVE vs Scalar: " << (double)scalar_time / sve_time << "x" << std::endl;
    std::cout << "Correctness: " << ((sve_result == scalar_result) ? "PASS" : "FAIL") << std::endl;
    
#ifdef __ARM_FEATURE_SVE2
    std::cout << "SVE2 total length: " << sve2_result << ", time: " << sve2_time << " us" << std::endl;
    std::cout << "SVE2 vs Scalar: " << (double)scalar_time / sve2_time << "x" << std::endl;
#endif
#else
    std::cout << "SVE not available on this platform" << std::endl;
#endif
    
    return 0;
}