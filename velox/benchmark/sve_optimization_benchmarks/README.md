# Velox SVE/SVE2 Module-Level Benchmarks

## Overview

这些benchmark已升级为**模块级别**，紧密模拟Velox实际业务场景，支持自定义数据规模和类型。

## Benchmark模块

### 1. HashTable Module Benchmark
**模拟场景**: HashJoin关键操作
- TagVector load (模拟hash tag匹配)
- Gather操作 (hash表探查)
- 完整HashProbe流程 (load + compare + bitmask生成)

**命令行参数**:
```bash
01_hash_table_module_benchmark <data_scale> <iterations> <selectivity> <distribution> <verbose>

# 示例
./01_hash_table_module_benchmark 1000000 1000 0.5 uniform verbose
./01_hash_table_module_benchmark 100000 500 0.3 dense
```

**参数说明**:
- `data_scale`: Hash表大小（默认1M）
- `iterations`: 迭代次数（默认1000）
- `selectivity`: 选择率（0.0-1.0，默认0.5）
- `distribution`: 数据分布（uniform/dense/sequential）
- `verbose`: 详细输出

### 2. Aggregation Module Benchmark
**模拟场景**: 聚合计算完整流程
- Bitmap Filter（过滤操作）
- Null Bitmap Popcount（null处理）
- Sum/Count/Min/Max聚合

**命令行参数**:
```bash
02_aggregation_module_benchmark <data_scale> <iterations> <selectivity> <null_ratio> <distribution> <verbose>

# 示例
./02_aggregation_module_benchmark 1000000 1000 0.5 0.1 uniform verbose
./02_aggregation_module_benchmark 500000 800 0.3 0.2 zipfian
```

**参数说明**:
- `data_scale`: 数据规模（默认1M）
- `iterations`: 迭代次数（默认1000）
- `selectivity`: 过滤选择率（默认0.5）
- `null_ratio`: null比例（默认0.1）
- `distribution`: 数据分布（uniform/zipfian/sequential）

### 3. String Processing Module Benchmark
**模拟场景**: 字符串函数处理
- String Length计算（批量扫描零终止符）
- ASCII Detection（高位检测）
- Upper/Lower Case转换

**命令行参数**:
```bash
03_string_processing_module_benchmark <string_count> <iterations> <verbose>

# 示例
./03_string_processing_module_benchmark 10000 1000 verbose
./03_string_processing_module_benchmark 50000 500
```

## Build Instructions

```bash
cd velox/velox/benchmark/sve_optimization_benchmarks
mkdir build
cd build
cmake ..
make -j
```

## Run Examples

### 小规模测试（快速验证）
```bash
./bin/01_hash_table_module_benchmark 10000 100
./bin/02_aggregation_module_benchmark 10000 100 0.5 0.1
./bin/03_string_processing_module_benchmark 1000 100
```

### 中规模测试（典型场景）
```bash
./bin/01_hash_table_module_benchmark 100000 500 0.5 uniform verbose
./bin/02_aggregation_module_benchmark 100000 500 0.3 0.15 uniform
./bin/03_string_processing_module_benchmark 10000 500 verbose
```

### 大规模测试（性能评估）
```bash
./bin/01_hash_table_module_benchmark 1000000 1000 0.5 dense
./bin/02_aggregation_module_benchmark 1000000 1000 0.5 0.1 uniform
./bin/03_string_processing_module_benchmark 50000 1000
```

## Expected Output

```
========== HashTable Module Benchmark ==========
Simulating HashJoin operations:
  1. TagVector load and compare
  2. Gather operation (hash table probe)
  3. Bitmask generation for match results

=== TagVector Load (Scalar) ===
Time: 1250 us
Throughput: 12.5 MB/s
Items processed: 16000000
Correctness: PASS

=== TagVector Load (SVE) ===
Time: 280 us
Throughput: 55.6 MB/s
Items processed: 16000000
Correctness: PASS
Speedup vs Scalar: 4.46x

=== Gather (HashProbe) (Scalar) ===
Time: 950 us
Throughput: 4.2 MB/s
Items processed: 1000000
Correctness: PASS

=== Gather (HashProbe) (SVE) ===
Time: 210 us
Throughput: 19.0 MB/s
Items processed: 1000000
Correctness: PASS
Speedup vs Scalar: 4.52x
```

## Key Features

1. **模块级别场景**: 模拟Velox实际业务流程（HashJoin、Aggregation等）
2. **可配置参数**: 数据规模、迭代次数、选择率、null比例等
3. **多种数据分布**: uniform、zipfian、dense、sequential
4. **详细输出**: 吞吐量、加速比、业务指标（filtered_count等）
5. **正确性验证**: 对比scalar baseline验证结果

## Performance Insights

基于TPC-DS workload特征：

| Module | TPC-DS占比 | 关键优化 | 预期加速 |
|--------|-----------|---------|---------|
| HashTable | 25% (Join) | SVE gather + predicate | **3-5x** |
| Aggregation | 30% | SVE filter + bitmap | **2-4x** |
| String | 15% | SVE predicate scan | **1.5-2x** |

## Integration with Velox

这些benchmark演示了独立模块性能。集成到Velox需修改：
1. `SimdUtil-inl.h`: Gather/Filter使用SVE
2. `HashTable.h`: TagVector添加SVE分支
3. `BitUtil.h`: Popcount使用SVE并行
4. `StringCore.h`: 已有SVE实现，可进一步优化

## Notes

- **Vector width**: 256-bit SVE (8 x int32)
- **Predicate优势**: 灵活处理任意长度和条件
- **业务相关**: 模拟真实选择率、null比例、数据分布