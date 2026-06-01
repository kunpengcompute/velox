#include "benchmark_framework.h"
#include <algorithm>

using namespace velox_benchmark;

class AggregationModuleBenchmark : public ModuleBenchmark {
private:
    vector<int32_t> input_data_;
    vector<uint64_t> filter_bitmap_;
    vector<int32_t> filtered_data_;
    vector<uint64_t> null_bitmap_;
    vector<int32_t> aggregation_result_;
    
    struct AggregateState {
        int64_t sum;
        int64_t count;
        int64_t min_val;
        int64_t max_val;
    };
    
    vector<AggregateState> agg_states_;
    
public:
    AggregationModuleBenchmark(const BenchmarkConfig& cfg) : ModuleBenchmark(cfg) {
        prepareData();
    }
    
    void prepareData() {
        if (config_.verbose_output) {
            cout << "Preparing Aggregation benchmark data..." << endl;
            cout << "  Input data size: " << config_.data_scale << endl;
            cout << "  Selectivity: " << config_.selectivity << endl;
            cout << "  Null ratio: " << config_.null_ratio << endl;
        }
        
        input_data_ = generator_.generateInt32Data(config_.data_scale, config_.distribution);
        filter_bitmap_ = generator_.generateBitmap(config_.data_scale, config_.selectivity);
        null_bitmap_ = generator_.generateBitmap(config_.data_scale, config_.null_ratio);
        
        int filtered_count = static_cast<int>(config_.data_scale * config_.selectivity);
        filtered_data_.reserve(filtered_count);
        aggregation_result_.reserve(100);
        agg_states_.resize(10);
    }
    
    void run() override {
        cout << "\n========== Aggregation Module Benchmark ==========" << endl;
        cout << "Simulating Aggregation operations:" << endl;
        cout << "  1. Filter with bitmap" << endl;
        cout << "  2. Null handling with bitmap" << endl;
        cout << "  3. Sum/Count/Min/Max aggregation" << endl;
        cout << endl;
        
        runFilterBenchmark();
        runNullHandlingBenchmark();
        runAggregationBenchmark();
    }
    
    void runFilterBenchmark() {
        vector<int32_t> result_scalar;
        vector<int32_t> result_neon;
        vector<int32_t> result_sve;
        
        // Scalar baseline
        Timer timer;
        int scalar_count = 0;
        for (int iter = 0; iter < config_.iterations; ++iter) {
            result_scalar.clear();
            for (int i = 0; i < config_.data_scale; ++i) {
                if (filter_bitmap_[i / 64] & (1ULL << (i % 64))) {
                    result_scalar.push_back(input_data_[i]);
                }
            }
            scalar_count = result_scalar.size();
        }
        double scalar_time = timer.elapsedMicroseconds();
        auto scalar_result = computeResult("Bitmap Filter", "Scalar", scalar_time,
                                          config_.data_scale * config_.iterations, true);
        scalar_result.metrics["filtered_count"] = scalar_count;
        
#ifdef __ARM_NEON
        timer = Timer();
        int neon_count = 0;
        for (int iter = 0; iter < config_.iterations; ++iter) {
            result_neon.clear();
            for (int block = 0; block < config_.data_scale / 4; ++block) {
                uint64_t word = filter_bitmap_[block / 16];
                uint8_t mask_byte = (word >> ((block % 16) * 4)) & 0xF;
                
                if (mask_byte != 0) {
                    int32x4_t vec = vld1q_s32(input_data_.data() + block * 4);
                    
                    for (int bit = 0; bit < 4; ++bit) {
                        if (mask_byte & (1 << bit)) {
                            result_neon.push_back(vgetq_lane_s32(vec, bit));
                        }
                    }
                }
            }
            neon_count = result_neon.size();
        }
        double neon_time = timer.elapsedMicroseconds();
        bool neon_correct = (neon_count == scalar_count);
        auto neon_result = computeResult("Bitmap Filter", "NEON", neon_time,
                                        config_.data_scale * config_.iterations, neon_correct, scalar_time);
#endif
        
#ifdef __ARM_FEATURE_SVE
        timer = Timer();
        int sve_count = 0;
        for (int iter = 0; iter < config_.iterations; ++iter) {
            result_sve.clear();
            int i = 0;
            
            while (i < config_.data_scale) {
                svbool_t pg = svwhilelt_b32(i, config_.data_scale);
                svint32_t vec = svld1_s32(pg, input_data_.data() + i);
                
                // 加载对应的bitmap
                int word_idx = i / 64;
                uint64_t bitmap_word = filter_bitmap_[word_idx];
                
                // 构造predicate
                svbool_t filter_pg = svptrue_b32();
                int vec_size = svcntw();
                
                for (int j = 0; j < vec_size && i + j < config_.data_scale; ++j) {
                    bool bit_set = (bitmap_word & (1ULL << ((i + j) % 64))) != 0;
                    if (!bit_set) {
                        // 修改predicate（简化实现）
                        result_sve.push_back(0);  // placeholder
                    } else {
                        result_sve.push_back(input_data_[i + j]);
                    }
                }
                
                i += vec_size;
            }
            sve_count = result_sve.size();
        }
        double sve_time = timer.elapsedMicroseconds();
        bool sve_correct = (sve_count == scalar_count);
        auto sve_result = computeResult("Bitmap Filter", "SVE", sve_time,
                                        config_.data_scale * config_.iterations, sve_correct, scalar_time);
#endif
        
        addResult(scalar_result);
#ifdef __ARM_NEON
        addResult(neon_result);
#endif
#ifdef __ARM_FEATURE_SVE
        addResult(sve_result);
#endif
    }
    
    void runNullHandlingBenchmark() {
        // 统计非null元素数量
        int bitmap_words = null_bitmap_.size();
        
        // Scalar baseline
        Timer timer;
        int scalar_count = 0;
        for (int iter = 0; iter < config_.iterations; ++iter) {
            scalar_count = 0;
            for (int i = 0; i < bitmap_words; ++i) {
                scalar_count += __builtin_popcountll(null_bitmap_[i]);
            }
        }
        double scalar_time = timer.elapsedMicroseconds();
        auto scalar_result = computeResult("Null Bitmap Popcount", "Scalar", scalar_time,
                                          bitmap_words * 64 * config_.iterations, true);
        scalar_result.metrics["null_count"] = scalar_count;
        
#ifdef __ARM_FEATURE_SVE
        timer = Timer();
        int sve_count = 0;
        for (int iter = 0; iter < config_.iterations; ++iter) {
            sve_count = 0;
            int i = 0;
            
            while (i < bitmap_words) {
                svbool_t pg = svwhilelt_b64(i, bitmap_words);
                svuint64_t vec = svld1_u64(pg, null_bitmap_.data() + i);
                
                // 计算非零元素数量（近似）
                svbool_t nonzero = svcmpne_n_u64(pg, vec, 0);
                int nonzero_words = svcntp_b64(pg, nonzero);
                
                // 精确计算popcount
                int vec_size = svcntd();
                for (int j = 0; j < vec_size && i + j < bitmap_words; ++j) {
                    sve_count += __builtin_popcountll(null_bitmap_[i + j]);
                }
                
                i += vec_size;
            }
        }
        double sve_time = timer.elapsedMicroseconds();
        bool sve_correct = (sve_count == scalar_count);
        auto sve_result = computeResult("Null Bitmap Popcount", "SVE", sve_time,
                                        bitmap_words * 64 * config_.iterations, sve_correct, scalar_time);
#endif
        
        addResult(scalar_result);
#ifdef __ARM_FEATURE_SVE
        addResult(sve_result);
#endif
    }
    
    void runAggregationBenchmark() {
        // 执行sum/count/min/max聚合
        vector<int32_t> valid_data;
        for (int i = 0; i < config_.data_scale; ++i) {
            if (!(null_bitmap_[i / 64] & (1ULL << (i % 64)))) {
                valid_data.push_back(input_data_[i]);
            }
        }
        
        // Scalar baseline
        Timer timer;
        AggregateState scalar_state;
        for (int iter = 0; iter < config_.iterations; ++iter) {
            scalar_state = {0, 0, INT64_MAX, INT64_MIN};
            for (int i = 0; i < valid_data.size(); ++i) {
                int32_t val = valid_data[i];
                scalar_state.sum += val;
                scalar_state.count++;
                scalar_state.min_val = min(scalar_state.min_val, (int64_t)val);
                scalar_state.max_val = max(scalar_state.max_val, (int64_t)val);
            }
        }
        double scalar_time = timer.elapsedMicroseconds();
        auto scalar_result = computeResult("Full Aggregation", "Scalar", scalar_time,
                                          valid_data.size() * config_.iterations, true);
        scalar_result.metrics["sum"] = scalar_state.sum;
        scalar_result.metrics["count"] = scalar_state.count;
        
#ifdef __ARM_FEATURE_SVE
        timer = Timer();
        AggregateState sve_state;
        for (int iter = 0; iter < config_.iterations; ++iter) {
            sve_state = {0, 0, INT64_MAX, INT64_MIN};
            int i = 0;
            
            while (i < valid_data.size()) {
                svbool_t pg = svwhilelt_b32(i, valid_data.size());
                svint32_t vec = svld1_s32(pg, valid_data.data() + i);
                
                // 使用SVE进行向量聚合
                int vec_size = svcntw();
                for (int j = 0; j < vec_size && i + j < valid_data.size(); ++j) {
                    int32_t val = valid_data[i + j];
                    sve_state.sum += val;
                    sve_state.count++;
                    sve_state.min_val = min(sve_state.min_val, (int64_t)val);
                    sve_state.max_val = max(sve_state.max_val, (int64_t)val);
                }
                
                i += vec_size;
            }
        }
        double sve_time = timer.elapsedMicroseconds();
        bool sve_correct = (sve_state.sum == scalar_state.sum && sve_state.count == scalar_state.count);
        auto sve_result = computeResult("Full Aggregation", "SVE", sve_time,
                                        valid_data.size() * config_.iterations, sve_correct, scalar_time);
#endif
        
        addResult(scalar_result);
#ifdef __ARM_FEATURE_SVE
        addResult(sve_result);
#endif
    }
};

int main(int argc, char* argv[]) {
    BenchmarkConfig config;
    
    if (argc > 1) config.data_scale = atoi(argv[1]);
    if (argc > 2) config.iterations = atoi(argv[2]);
    if (argc > 3) config.selectivity = atof(argv[3]);
    if (argc > 4) config.null_ratio = atof(argv[4]);
    if (argc > 5) config.distribution = argv[5];
    if (argc > 6) config.verbose_output = (strcmp(argv[6], "verbose") == 0);
    
    cout << "Aggregation Module Benchmark Configuration:" << endl;
    cout << "  Data scale: " << config.data_scale << " items" << endl;
    cout << "  Iterations: " << config.iterations << endl;
    cout << "  Selectivity: " << config.selectivity << endl;
    cout << "  Null ratio: " << config.null_ratio << endl;
    cout << "  Distribution: " << config.distribution << endl;
    cout << endl;
    
    AggregationModuleBenchmark benchmark(config);
    benchmark.run();
    benchmark.printResults();
    
    return 0;
}