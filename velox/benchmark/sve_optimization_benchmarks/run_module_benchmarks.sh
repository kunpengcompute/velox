#!/bin/bash
# Velox SVE/SVE2 Module Benchmark Suite
# 支持自定义参数运行

echo "=========================================="
echo "Velox SVE/SVE2 Module Benchmark Suite"
echo "=========================================="
echo ""

# 默认参数
DATA_SCALE=${1:-100000}
ITERATIONS=${2:-500}
SELECTIVITY=${3:-0.5}
NULL_RATIO=${4:-0.1}
VERBOSE=${5:-""}

# 显示配置
echo "Configuration:"
echo "  Data Scale: $DATA_SCALE"
echo "  Iterations: $ITERATIONS"
echo "  Selectivity: $SELECTIVITY"
echo "  Null Ratio: $NULL_RATIO"
if [ "$VERBOSE" = "verbose" ]; then
    echo "  Verbose Mode: ON"
else
    echo "  Verbose Mode: OFF"
fi
echo ""

# 检查架构
if [[ $(uname -m) != "aarch64" ]]; then
    echo "WARNING: Not running on ARM64 architecture"
    echo "SVE optimizations will not be available"
    echo ""
fi

# 运行benchmark
echo "========== Module 1: HashTable ========== "
./bin/01_hash_table_module_benchmark $DATA_SCALE $ITERATIONS $SELECTIVITY dense $VERBOSE
echo ""

echo "========== Module 2: Aggregation ========== "
./bin/02_aggregation_module_benchmark $DATA_SCALE $ITERATIONS $SELECTIVITY $NULL_RATIO uniform $VERBOSE
echo ""

echo "========== Module 3: String Processing ========== "
STRING_COUNT=$(($DATA_SCALE / 100))
./bin/03_string_processing_module_benchmark $STRING_COUNT $ITERATIONS $VERBOSE
echo ""

echo "=========================================="
echo "Benchmark Suite Completed"
echo "=========================================="
echo ""
echo "Usage: ./run_module_benchmarks.sh [data_scale] [iterations] [selectivity] [null_ratio] [verbose]"
echo "Example:"
echo "  ./run_module_benchmarks.sh 100000 500 0.5 0.1 verbose"
echo "  ./run_module_benchmarks.sh 1000000 1000 0.3 0.2"