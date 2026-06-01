#include "benchmark_framework.h"

using namespace velox_benchmark;

class StringModuleBenchmark : public ModuleBenchmark {
private:
    vector<string> input_strings_;
    vector<int> string_lengths_;
    vector<bool> ascii_flags_;
    vector<string> upper_strings_;
    vector<string> lower_strings_;
    
public:
    StringModuleBenchmark(const BenchmarkConfig& cfg) : ModuleBenchmark(cfg) {
        prepareData();
    }
    
    void prepareData() {
        if (config_.verbose_output) {
            cout << "Preparing String benchmark data..." << endl;
            cout << "  String count: " << config_.data_scale << endl;
            cout << "  Avg length: 50 characters" << endl;
        }
        
        input_strings_ = generator_.generateStrings(config_.data_scale, 10, 100);
        string_lengths_.resize(config_.data_scale);
        ascii_flags_.resize(config_.data_scale);
        upper_strings_.resize(config_.data_scale);
        lower_strings_.resize(config_.data_scale);
    }
    
    void run() override {
        cout << "\n========== String Processing Module Benchmark ==========" << endl;
        cout << "Simulating String operations:" << endl;
        cout << "  1. String length calculation" << endl;
        cout << "  2. ASCII detection" << endl;
        cout << "  3. Upper/Lower case conversion" << endl;
        cout << endl;
        
        runStringLengthBenchmark();
        runASCIIDetectionBenchmark();
        runCaseConversionBenchmark();
    }
    
    void runStringLengthBenchmark() {
        // Scalar baseline
        Timer timer;
        for (int iter = 0; iter < config_.iterations; ++iter) {
            for (int i = 0; i < config_.data_scale; ++i) {
                string_lengths_[i] = input_strings_[i].length();
            }
        }
        double scalar_time = timer.elapsedMicroseconds();
        auto scalar_result = computeResult("String Length", "Scalar", scalar_time,
                                          config_.data_scale * config_.iterations, true);
        
#ifdef __ARM_NEON
        timer = Timer();
        for (int iter = 0; iter < config_.iterations; ++iter) {
            for (int i = 0; i < config_.data_scale; ++i) {
                const char* str = input_strings_[i].c_str();
                size_t len = 0;
                
                uint8x16_t zero_vec = vmovq_n_u8(0);
                while (len < input_strings_[i].length()) {
                    uint8x16_t chunk = vld1q_u8((const uint8_t*)(str + len));
                    uint8x16_t cmp = vceqq_u8(chunk, zero_vec);
                    
                    if (vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0) != 0 ||
                        vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1) != 0) {
                        for (int j = 0; j < 16; ++j) {
                            if (str[len + j] == '\0') {
                                len += j;
                                break;
                            }
                        }
                        break;
                    }
                    len += 16;
                }
                
                string_lengths_[i] = len;
            }
        }
        double neon_time = timer.elapsedMicroseconds();
        auto neon_result = computeResult("String Length", "NEON", neon_time,
                                        config_.data_scale * config_.iterations, true, scalar_time);
#endif
        
#ifdef __ARM_FEATURE_SVE
        timer = Timer();
        for (int iter = 0; iter < config_.iterations; ++iter) {
            for (int i = 0; i < config_.data_scale; ++i) {
                const char* str = input_strings_[i].c_str();
                size_t len = 0;
                int max_len = input_strings_[i].length();
                
                svuint8_t zero_vec = svdup_n_u8(0);
                
                while (len < max_len) {
                    svbool_t pg = svwhilelt_b8(len, max_len);
                    svuint8_t chunk = svld1_u8(pg, (const uint8_t*)(str + len));
                    svbool_t is_zero = svcmpeq_u8(pg, chunk, zero_vec);
                    
                    if (svptest_any(pg, is_zero)) {
                        int vec_size = svcntb();
                        for (int j = 0; j < vec_size && len + j < max_len; ++j) {
                            if (str[len + j] == '\0') {
                                len += j;
                                break;
                            }
                        }
                        break;
                    }
                    
                    len += svcntb();
                }
                
                string_lengths_[i] = len;
            }
        }
        double sve_time = timer.elapsedMicroseconds();
        auto sve_result = computeResult("String Length", "SVE", sve_time,
                                        config_.data_scale * config_.iterations, true, scalar_time);
#endif
        
        addResult(scalar_result);
#ifdef __ARM_NEON
        addResult(neon_result);
#endif
#ifdef __ARM_FEATURE_SVE
        addResult(sve_result);
#endif
    }
    
    void runASCIIDetectionBenchmark() {
        // Scalar baseline
        Timer timer;
        for (int iter = 0; iter < config_.iterations; ++iter) {
            for (int i = 0; i < config_.data_scale; ++i) {
                bool is_ascii = true;
                for (char c : input_strings_[i]) {
                    if (c & 0x80) {
                        is_ascii = false;
                        break;
                    }
                }
                ascii_flags_[i] = is_ascii;
            }
        }
        double scalar_time = timer.elapsedMicroseconds();
        auto scalar_result = computeResult("ASCII Detection", "Scalar", scalar_time,
                                          config_.data_scale * config_.iterations, true);
        
#ifdef __ARM_FEATURE_SVE
        timer = Timer();
        for (int iter = 0; iter < config_.iterations; ++iter) {
            for (int i = 0; i < config_.data_scale; ++i) {
                const char* str = input_strings_[i].c_str();
                size_t len = input_strings_[i].length();
                bool is_ascii = true;
                
                svuint8_t threshold = svdup_n_u8(0x80);
                
                int offset = 0;
                while (offset < len) {
                    svbool_t pg = svwhilelt_b8(offset, len);
                    svuint8_t chunk = svld1_u8(pg, (const uint8_t*)(str + offset));
                    
                    svbool_t is_non_ascii = svcmpge_u8(pg, chunk, threshold);
                    if (svptest_any(pg, is_non_ascii)) {
                        is_ascii = false;
                        break;
                    }
                    
                    offset += svcntb();
                }
                
                ascii_flags_[i] = is_ascii;
            }
        }
        double sve_time = timer.elapsedMicroseconds();
        auto sve_result = computeResult("ASCII Detection", "SVE", sve_time,
                                        config_.data_scale * config_.iterations, true, scalar_time);
#endif
        
        addResult(scalar_result);
#ifdef __ARM_FEATURE_SVE
        addResult(sve_result);
#endif
    }
    
    void runCaseConversionBenchmark() {
        // Scalar baseline - Upper case
        Timer timer;
        for (int iter = 0; iter < config_.iterations; ++iter) {
            for (int i = 0; i < config_.data_scale; ++i) {
                upper_strings_[i].clear();
                for (char c : input_strings_[i]) {
                    if (c >= 'a' && c <= 'z') {
                        upper_strings_[i] += c - 32;
                    } else {
                        upper_strings_[i] += c;
                    }
                }
            }
        }
        double scalar_upper_time = timer.elapsedMicroseconds();
        auto scalar_upper_result = computeResult("Upper Case", "Scalar", scalar_upper_time,
                                                config_.data_scale * config_.iterations, true);
        
#ifdef __ARM_FEATURE_SVE
        timer = Timer();
        for (int iter = 0; iter < config_.iterations; ++iter) {
            for (int i = 0; i < config_.data_scale; ++i) {
                const char* str = input_strings_[i].c_str();
                size_t len = input_strings_[i].length();
                upper_strings_[i].resize(len);
                
                svuint8_t lower_a = svdup_n_u8('a');
                svuint8_t lower_z = svdup_n_u8('z');
                svuint8_t offset = svdup_n_u8(32);
                
                int offset_idx = 0;
                while (offset_idx < len) {
                    svbool_t pg = svwhilelt_b8(offset_idx, len);
                    svuint8_t chunk = svld1_u8(pg, (const uint8_t*)(str + offset_idx));
                    
                    // 检查是否是小写字母
                    svbool_t is_lower = svand_b_z(pg, 
                        svcmpge_u8(pg, chunk, lower_a),
                        svcmple_u8(pg, chunk, lower_z));
                    
                    // 转换为大写
                    svuint8_t converted = svsub_u8_z(is_lower, chunk, offset);
                    svuint8_t result = svsel_u8(is_lower, converted, chunk);
                    
                    svst1_u8(pg, (uint8_t*)(upper_strings_[i].data() + offset_idx), result);
                    offset_idx += svcntb();
                }
            }
        }
        double sve_upper_time = timer.elapsedMicroseconds();
        auto sve_upper_result = computeResult("Upper Case", "SVE", sve_upper_time,
                                             config_.data_scale * config_.iterations, true, scalar_upper_time);
#endif
        
        addResult(scalar_upper_result);
#ifdef __ARM_FEATURE_SVE
        addResult(sve_upper_result);
#endif
    }
};

int main(int argc, char* argv[]) {
    BenchmarkConfig config;
    
    if (argc > 1) config.data_scale = atoi(argv[1]);
    if (argc > 2) config.iterations = atoi(argv[2]);
    if (argc > 3) config.verbose_output = (strcmp(argv[3], "verbose") == 0);
    
    cout << "String Processing Module Benchmark Configuration:" << endl;
    cout << "  String count: " << config.data_scale << endl;
    cout << "  Iterations: " << config.iterations << endl;
    cout << endl;
    
    StringModuleBenchmark benchmark(config);
    benchmark.run();
    benchmark.printResults();
    
    return 0;
}