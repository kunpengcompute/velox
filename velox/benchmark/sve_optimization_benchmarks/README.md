# Velox SVE/SVE2 Advanced Module Benchmarks

## 升级说明

基于Velox HashAgg、HashJoin、表达式计算逻辑深度分析，**升级到完整业务流程模拟**。

---

## Benchmark分析对比

### 原有Benchmark缺失点

| 模块 | 缺失项 | Velox实际逻辑 |
|------|--------|--------------|
| **HashAgg** | ❌ Grouping key extraction | `GroupingSet::addInput` 需提取多列grouping key |
| | ❌ Hash计算 | `VectorHasher::compute` 多列组合hash |
| | ❌ GroupProbe完整流程 | `HashTable::groupProbe` 包含probe + insert + aggregate |
| | ❌ initializeNewGroups | 新group初始化聚合状态 |
| | ❌ addInput聚合累积 | `function->addRawInput` 多aggregate并行更新 |
| | ❌ Partial aggregation内存管理 | `isPartialFull` + flush机制 |
| | ❌ Distinct aggregation | `distinctAggregations_->addInput` 特殊处理 |
| **HashJoin** | ❌ Build端逻辑 | 只有probe，缺失build hash表构建 |
| | ❌ TagVector生成 | 8-bit tag计算和存储 |
| | ❌ Collision handling | 碰撞时的线性探测策略 |
| | ❌ Prefetch机制 | `__builtin_prefetch` (HashTable.cpp:1216) |
| | ❌ Hash mode选择 | Array hash vs Regular hash模式切换 |
| | ❌ Probe depth统计 | 平均probe深度性能指标 |
| **表达式** | ❌ PeeledEncoding处理 | 字典/常量编码向量的剥离 |
| | ❌ 常量折叠 | `ExprCompiler::constantFolding` |
| | ❌ 表达式编译时间 | 编译开销统计 |
| **内存** | ❌ Memory tracking | Velox内存管理机制 |
| | ❌ Allocation统计 | 分配次数和大小跟踪 |

---

## 新增Advanced Benchmark

### **4. HashAggregation Advanced Benchmark**

**完整模拟HashAgg管道**（基于Velox `HashAggregation.cpp` + `GroupingSet.cpp`）:

1. **Grouping key extraction** (`GroupingSet::addInput`)
   - 多列grouping key提取
   - 配置：`num_grouping_keys` (默认3列)
   
2. **Hash computation** (`VectorHasher::compute`)
   - 多列组合hash计算
   - 使用SVE批量处理
   
3. **GroupProbe** (`HashTable::groupProbe`)
   - Hash表probe + 碰撞处理
   - 统计：碰撞次数、probe深度
   
4. **initializeNewGroups**
   - 新group初始化（sum=0, count=0, min/max）
   
5. **Aggregate accumulation** (`function->addRawInput`)
   - 多聚合并行更新（sum, count, min, max）
   - 配置：`num_aggregates` (默认5个)
   
6. **Partial aggregation memory**
   - 内存限制检查 (`isPartialFull`)
   - Flush机制
   - 配置：`partial_agg_memory_limit_mb`
   
7. **Distinct aggregation** (可选)
   - `distinctAggregations_->addInput`
   - 使用set去重
   - 配置：`enable_distinct_agg`

**命令行**:
```bash
04_hash_aggregation_advanced_benchmark \
    <data_scale> \
    <iterations> \
    <num_grouping_keys> \
    <num_aggregates> \
    <key_cardinality> \
    <distinct> \
    <verbose>

# 示例
./04_hash_aggregation_advanced_benchmark 1000000 1000 3 5 0.01 distinct verbose
./04_hash_aggregation_advanced_benchmark 500000 500 2 3 0.05
```

**输出指标**:
- Groups created
- Hash collisions
- Avg probe depth
- Aggregation efficiency (%)
- Memory used (MB)
- Flush count

---

### **5. HashJoin Advanced Benchmark**

**完整模拟HashJoin管道**（基于Velox `HashTable.cpp`）:

1. **Build side hash table construction**
   - Hash表构建
   - Tag generation (8-bit)
   - 碰撞插入
   
2. **Tag matching** (`loadTags`)
   - 8-bit tag批量比较
   - SVE `svcmpeq_u8` 指令
   
3. **Probe with prefetch** (`arrayGroupProbe`)
   - Prefetch机制：`__builtin_prefetch`
   - Probe distance: 10 (Velox代码)
   - Collision处理
   
4. **Hash mode comparison**
   - Array hash: 直接索引
   - Regular hash: hash % capacity
   - 配置：`hash_mode` ("array" or "regular")

**命令行**:
```bash
05_hash_join_advanced_benchmark \
    <build_side_size> \
    <probe_side_size> \
    <iterations> \
    <join_selectivity> \
    <hash_mode> \
    <prefetch> \
    <verbose>

# 示例
./05_hash_join_advanced_benchmark 1000000 10000000 1000 0.1 regular prefetch verbose
./05_hash_join_advanced_benchmark 500000 5000000 500 0.05 array
```

**输出指标**:
- Build collisions
- Probe matches
- Avg probe depth
- Prefetch count
- Match ratio (%)

---

## Velox代码映射

| Benchmark功能 | Velox代码位置 | 关键函数 |
|--------------|---------------|---------|
| Grouping key提取 | `GroupingSet.cpp:172` | `addInput()` |
| Hash计算 | `VectorHasher.h` | `compute()` |
| GroupProbe | `HashTable.cpp:560` | `groupProbe()` |
| New group初始化 | `GroupingSet.cpp:284` | `initializeNewGroups()` |
| Aggregate累积 | `GroupingSet.cpp:299` | `addRawInput()` |
| Partial内存管理 | `GroupingSet.cpp:822` | `isPartialFull()` |
| Build表构建 | `HashTable.cpp:1200` | `storeRowPointer()` |
| Prefetch | `HashTable.cpp:1216` | `__builtin_prefetch()` |
| Tag匹配 | `HashTable.h:397` | `loadTags()` |

---

## Build & Run

```bash
cd velox/velox/benchmark/sve_optimization_benchmarks
mkdir build && cd build
cmake ..
make -j
```

### 小规模快速测试
```bash
./bin/04_hash_aggregation_advanced_benchmark 10000 100 2 3 0.1
./bin/05_hash_join_advanced_benchmark 10000 100000 100 0.5 regular
```

### 大规模性能测试
```bash
./bin/04_hash_aggregation_advanced_benchmark 1000000 1000 3 5 0.01 distinct verbose
./bin/05_hash_join_advanced_benchmark 1000000 10000000 1000 0.1 regular prefetch verbose
```

---

## 新增特性总结

| 特性 | 说明 |
|------|------|
| ✅ **多列grouping key** | 支持1-10列grouping key |
| ✅ **完整hash表生命周期** | Build → Probe → Collision → Rehash |
| ✅ **Prefetch机制** | 模拟Velox的预取优化 |
| ✅ **碰撞统计** | 深度、次数、命中率 |
| ✅ **内存跟踪** | Allocation统计、Memory used |
| ✅ **业务指标** | Groups, Matches, Efficiency |
| ✅ **多种hash模式** | Array vs Regular hash对比 |
| ✅ **Distinct聚合** | Count(distinct x)模拟 |
| ✅ **Partial aggregation** | 内存限制 + Flush |

---

## 与Velox实际业务对比

| 实际场景 | Benchmark覆盖度 | 关键参数 |
|---------|-----------------|---------|
| TPC-DS Q17 (3列group by) | ✅ 完全覆盖 | `num_grouping_keys=3` |
| TPC-DS Q55 (distinct) | ✅ Distinct模式 | `enable_distinct_agg=true` |
| HashJoin大表join | ✅ Build+Probe | `build_side_size=1M` |
| Hash碰撞密集场景 | ✅ 碰撞统计 | `key_cardinality=0.01` |
| 预取优化效果 | ✅ Prefetch开关 | `enable_prefetch=true` |