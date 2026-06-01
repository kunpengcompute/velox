#!/bin/bash
# Run all SVE optimization benchmarks and report results

echo "======================================"
echo "SVE/SVE2 Optimization Benchmark Suite"
echo "======================================"
echo ""

# Check architecture
if [[ $(uname -m) != "aarch64" ]]; then
    echo "WARNING: Not running on ARM64 architecture"
    echo "SVE optimizations will not be available"
    echo ""
fi

# Check if SVE is available
if grep -q "sve" /proc/cpuinfo 2>/dev/null; then
    echo "SVE detected in CPU features"
else
    echo "SVE not detected in /proc/cpuinfo (may still be supported by compiler)"
fi
echo ""

# Run benchmarks
echo "=== Running Benchmark 1: Gather ==="
./01_gather_benchmark
echo ""

echo "=== Running Benchmark 2: Tag Vector Load ==="
./02_tag_vector_load_benchmark
echo ""

echo "=== Running Benchmark 3: Filter/Compact ==="
./03_filter_compact_benchmark
echo ""

echo "=== Running Benchmark 4: Popcount ==="
./04_popcount_benchmark
echo ""

echo "=== Running Benchmark 5: String Length ==="
./05_string_length_benchmark
echo ""

echo "======================================"
echo "Benchmark Suite Completed"
echo "======================================"