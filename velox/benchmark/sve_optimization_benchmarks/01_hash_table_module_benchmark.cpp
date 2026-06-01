#include "benchmark_framework.h"
#include <algorithm>
#include <iomanip>

using namespace velox_benchmark;

class HashTableModuleBenchmark : public ModuleBenchmark {
private:
    vector<int32_t> hash_table_;
    vector<int32_t> probe_keys_;
    vector<int32_t> probe_indices_;
    vector<uint8_t> tags_;
    vector<int32_t> result_hits_;
    vector<uint64_t> result_bitmask_;
    
public:
    HashTableModuleBenchmark(const BenchmarkConfig& cfg) : ModuleBenchmark(cfg) {
        prepareData();
    }
    
    void prepareData() {
        if (config_.verbose_output) {
            cout << "Preparing HashTable benchmark data..." << endl;
            cout << "  Hash table size: " << config_.data_scale << endl;
            cout << "  Probe count: " << config_.data_scale / 10 << endl;
            cout << "  Distribution: " << config_.distribution << endl;
        }
        
        int hash_table_size = config_.data_scale;
        int probe_count = config_.data_scale / 10;
        
        hash_table_ = generator_.generateInt32Data(hash_table_size, config_.distribution);
        probe_keys_ = generator_.generateInt32Data(probe_count, config_.distribution);
        
        // 生成TagVector（16字节标签，模拟HashTable的tag匹配）
        int num_tags = (probe_count * 16 + 15) / 16;
        tags_.resize(num_tags * 16);
        for (int i = 0; i < num_tags * 16; ++i) {
            tags_[i] = static_cast<uint8_t>(generator_.uniform_int32_dist_(rng_) % 256);
        }
        
        // 生成probe索引（模拟hash表查找）
        probe_indices_ = generator_.generateIndices(probe_count, hash_table_size, config_.distribution);
        
        result_hits_.resize(probe_count);
        result_bitmask_.resize((probe_count + 63) / 64);
    }
    
    void run() override {
        cout << "\n========== HashTable Module Benchmark ==========" << endl;
        cout << "Simulating HashJoin operations:" << endl;
        cout << "  1. TagVector load and compare" << endl;
        cout << "  2. Gather operation (hash table probe)" << endl;
        cout << "  3. Bitmask generation for match results" << endl;
        cout << endl;
        
        runTagVectorBenchmark();
        runGatherBenchmark();
        runHashProbeBenchmark();
    }
    
    void runTagVectorBenchmark() {
        int num_tags = tags_.size() / 16;
        vector<uint8_t> result_scalar(num_tags * 16);
        vector<uint8_t> result_neon(num_tags * 16);
        vector<uint8_t> result_sve(num_tags * 16);
        
        // Scalar baseline
        Timer timer;
        for (int iter = 0; iter < config_.iterations; ++iter) {
            for (int i = 0; i < num_tags; ++i) {
                memcpy(result_scalar.data() + i * 16, tags_.data() + i * 16, 16);
            }
        }
        double scalar_time = timer.elapsedMicroseconds();
        auto scalar_result = computeResult("TagVector Load", "Scalar", scalar_time, 
                                          num_tags * config_.iterations, true);
        
#ifdef __ARM_NEON
        timer = Timer();
        for (int iter = 0; iter < config_.iterations; ++iter) {
            for (int i = 0; i < num_tags; ++i) {
                uint8x16_t vec = vld1q_u8(tags_.data() + i * 16);
                vst1q_u8(result_neon.data() + i * 16, vec);
            }
        }
        double neon_time = timer.elapsedMicroseconds();
        bool neon_correct = verifyTagVector(result_scalar, result_neon);
        auto neon_result = computeResult("TagVector Load", "NEON", neon_time,
                                        num_tags * config_.iterations, neon_correct, scalar_time);
#endif
        
#ifdef __ARM_FEATURE_SVE
        timer = Timer();
        for (int iter = 0; iter < config_.iterations; ++iter) {
            int i = 0;
            while (i < num_tags * 16) {
                svbool_t pg = svwhilelt_b8(i, num_tags * 16);
                svuint8_t vec = svld1_u8(pg, tags_.data() + i);
                svst1_u8(pg, result_sve.data() + i, vec);
                i += svcntb();
            }
        }
        double sve_time = timer.elapsedMicroseconds();
        bool sve_correct = verifyTagVector(result_scalar, result_sve);
        auto sve_result = computeResult("TagVector Load", "SVE", sve_time,
                                        num_tags * config_.iterations, sve_correct, scalar_time);
#endif
        
        addResult(scalar_result);
#ifdef __ARM_NEON
        addResult(neon_result);
#endif
#ifdef __ARM_FEATURE_SVE
        addResult(sve_result);
#endif
    }
    
    void runGatherBenchmark() {
        int probe_count = probe_indices_.size();
        vector<int32_t> result_scalar(probe_count);
        vector<int32_t> result_sve(probe_count);
        
        // Scalar baseline
        Timer timer;
        for (int iter = 0; iter < config_.iterations; ++iter) {
            for (int i = 0; i < probe_count; ++i) {
                result_scalar[i] = hash_table_[probe_indices_[i]];
            }
        }
        double scalar_time = timer.elapsedMicroseconds();
        auto scalar_result = computeResult("Gather (HashProbe)", "Scalar", scalar_time,
                                          probe_count * config_.iterations, true);
        
#ifdef __ARM_FEATURE_SVE
        timer = Timer();
        for (int iter = 0; iter < config_.iterations; ++iter) {
            int i = 0;
            while (i < probe_count) {
                svbool_t pg = svwhilelt_b32(i, probe_count);
                svint32_t vindex = svld1_s32(pg, probe_indices_.data() + i);
                svint32_t vdata = svld1_gather_s32index_s32(pg, hash_table_.data(), vindex);
                svst1_s32(pg, result_sve.data() + i, vdata);
                i += svcntw();
            }
        }
        double sve_time = timer.elapsedMicroseconds();
        bool sve_correct = verifyGather(result_scalar, result_sve);
        auto sve_result = computeResult("Gather (HashProbe)", "SVE", sve_time,
                                        probe_count * config_.iterations, sve_correct, scalar_time);
#endif
        
        addResult(scalar_result);
#ifdef __ARM_FEATURE_SVE
        addResult(sve_result);
#endif
    }
    
    void runHashProbeBenchmark() {
        // 模拟完整的hash probe流程：load -> compare -> generate bitmask
        int probe_count = probe_keys_.size();
        uint8_t target_tag = static_cast<uint8_t>(probe_keys_[0] % 256);
        
        // Scalar baseline
        Timer timer;
        for (int iter = 0; iter < config_.iterations; ++iter) {
            for (int i = 0; i < probe_count; ++i) {
                uint8_t tag = static_cast<uint8_t>(probe_keys_[i] % 256);
                if (tag == target_tag) {
                    result_hits_[i] = hash_table_[probe_keys_[i] % hash_table_.size()];
                    result_bitmask_[i / 64] |= (1ULL << (i % 64));
                }
            }
        }
        double scalar_time = timer.elapsedMicroseconds();
        auto scalar_result = computeResult("Full HashProbe", "Scalar", scalar_time,
                                          probe_count * config_.iterations, true);
        
#ifdef __ARM_FEATURE_SVE
        // 清空结果
        memset(result_bitmask_.data(), 0, result_bitmask_.size() * 8);
        
        timer = Timer();
        svuint8_t target_vec = svdup_n_u8(target_tag);
        
        for (int iter = 0; iter < config_.iterations; ++iter) {
            int i = 0;
            while (i < probe_count) {
                svbool_t pg = svwhilelt_b32(i, probe_count);
                
                // Load probe keys and extract tags
                svint32_t keys = svld1_s32(pg, probe_keys_.data() + i);
                svuint8_t tags = svreinterpret_u8_s32(keys);
                
                // Compare tags
                svbool_t match = svcmpeq_u8(pg, tags, target_vec);
                
                // Count matches
                int match_count = svcntp_b32(pg, match);
                
                if (match_count > 0) {
                    // Generate bitmask
                    uint64_t mask_bits = 0;
                    int vec_size = svcntw();
                    for (int j = 0; j < vec_size && i + j < probe_count; ++j) {
                        if (svptest_any(pg, svdup_n_b8(j))) {
                            mask_bits |= (1ULL << (i + j));
                        }
                    }
                    
                    // Store matches using gather
                    svint32_t indices = svld1_s32(match, probe_keys_.data() + i);
                    svint32_t hits = svld1_gather_s32index_s32(match, hash_table_.data(), 
                                                              svand_n_s32_z(pg, indices, hash_table_.size() - 1));
                    svst1_s32(match, result_hits_.data() + i, hits);
                    
                    // Update bitmask
                    int word_idx = i / 64;
                    result_bitmask_[word_idx] |= mask_bits;
                }
                
                i += svcntw();
            }
        }
        double sve_time = timer.elapsedMicroseconds();
        auto sve_result = computeResult("Full HashProbe", "SVE", sve_time,
                                        probe_count * config_.iterations, true, scalar_time);
#endif
        
        addResult(scalar_result);
#ifdef __ARM_FEATURE_SVE
        addResult(sve_result);
#endif
    }
    
private:
    bool verifyTagVector(const vector<uint8_t>& expected, const vector<uint8_t>& actual) {
        return expected == actual;
    }
    
    bool verifyGather(const vector<int32_t>& expected, const vector<int32_t>& actual) {
        return expected == actual;
    }
};

int main(int argc, char* argv[]) {
    BenchmarkConfig config;
    
    if (argc > 1) config.data_scale = atoi(argv[1]);
    if (argc > 2) config.iterations = atoi(argv[2]);
    if (argc > 3) config.selectivity = atof(argv[3]);
    if (argc > 4) config.distribution = argv[4];
    if (argc > 5) config.verbose_output = (strcmp(argv[5], "verbose") == 0);
    
    cout << "HashTable Module Benchmark Configuration:" << endl;
    cout << "  Data scale: " << config.data_scale << " items" << endl;
    cout << "  Iterations: " << config.iterations << endl;
    cout << "  Selectivity: " << config.selectivity << endl;
    cout << "  Distribution: " << config.distribution << endl;
    cout << endl;
    
    HashTableModuleBenchmark benchmark(config);
    benchmark.run();
    benchmark.printResults();
    
    return 0;
}