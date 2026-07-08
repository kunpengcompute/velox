# SVE 加速路径扩展设计方案

## 1. 背景

### 1.1 当前架构

`SumAggregateBase::updateInternal` 是 int64 sum 聚合的核心入口函数。它根据条件选择三条执行路径之一：

```
updateInternal()
├── [路径A] Lazy pushdown（延迟加载下推）
├── [路径B] SVE 加速路径 → updateGroups()（本文件内 SVE intrinsics 实现）
└── [路径C] 基类通用路径 → BaseAggregate::updateGroups()
```

### 1.2 SVE 加速路径原理

路径B 利用 ARM SVE（Scalable Vector Extension）向量指令一次处理 32 行数据，关键优化点：

- **Bitmap 向量化处理**：30KB 的 group bitmap 通过 `svld1`/`svand` 直接加载为 SVE predicate 寄存器，消除逐行 `isBitSet()` 调用
- **Gather/Scatter 访问**：通过 `svld1_gather`/`svst1_scatter` 实现对 group 累加器的向量化读写
- **密度自适应调度**：int32 路径根据 popcount 与 `kDenseThreshold`(56) 比较，自动选择 Dense（顺序扫描）或 Sparse（ctz 位扫描）内循环

## 2. 问题分析

### 2.1 当前入口条件

路径B 的进入条件是三重"与"门控（`updateInternal` L1113-1117）：

```cpp
if (exec::Aggregate::numNulls_) {              // 条件1: group层面有NULL累加器
    DecodedVector decoded(*arg, rows, ...);
    if (is_same_v<TData, int64_t> &&            // 条件2: 编译期类型匹配
        (is_same_v<TValue, int64_t> || ...) &&
        decoded.mayHaveNulls() && Overflow) {   // 条件3: 输入数据有NULL
        → 路径B (SVE)
    } else {
        → 路径C (基类)
    }
} else {
    → 路径C (基类)
}
```

三个条件缺一不可，导致大量本该走 SVE 的场景落入基类通用路径。

### 2.2 条件1：`numNulls_` — 不必要

**原理**：`numNulls_` 是聚合 group 中 NULL 累加器的计数器。当 `numNulls_ == 0` 时，所有 group 都已有有效累加值，不需要 `clearNull` 操作。

**为什么不必要**：
- `updateGroups` 接受 `tableHasNulls` 模板参数，内部已支持 `true`/`false` 两条路径
- 底层的 `clearNull()`（Aggregate.h:382）和 `clearNullSVE()`（L274）内部有独立的 `if (numNulls_)` 短路检查，`numNulls_ == 0` 时仅多一条 branch 指令（~1 cycle）
- base 路径同样用 `tableHasNulls` 模板参数区分，SVE 路径完全可以同样处理

**影响范围**：首次聚合（第一批数据到来时所有 group 为 NULL）走 SVE；后续批次 `numNulls_` 降为 0 后退化为基类路径。

### 2.3 条件3：`decoded.mayHaveNulls()` — 不必要

**原理**：`mayHaveNulls()` 表示输入列是否可能包含 NULL 值。

**为什么不必要**：
- SVE 函数内部通过 `mode1` 参数自适应处理 NULL：
  - `mode1 = 0`：无 NULL，谓词寄存器设为 `svptrue_b8()`（全选）
  - `mode1 = 1/2/3`：有 NULL，通过 bitmap 加载 + SVE `svand` 过滤
- `mode1 = 0` 的路径反而更快（跳过 bitmap 加载和 AND 操作），没有性能损失
- SVE 的 predicate 寄存器天然就是为这种可选过滤设计的

**影响范围**：非 NULL 列的 sum（如 `sum(order_count)` 通常无 NULL）无法走 SVE 加速。

### 2.4 影响量化

以典型 OLAP 场景为例——`SELECT sum(l_quantity) FROM lineitem`：

| 批次 | numNulls_ | mayHaveNulls | 当前路径 | 应走路径 |
|------|-----------|-------------|---------|---------|
| 第1批 | >0 | false | 基类 | **SVE** |
| 第2批+ | 0 | false | 基类 | **SVE** |
| 有NULL输入 | >0 | true | **SVE** | **SVE** |
| 有NULL输入 | 0 | true | 基类 | **SVE** |

**结论**：除第一批有 NULL 输入的场景外，所有其他情况均无法进入 SVE 路径。在实际生产负载中，绝大多数批次 `numNulls_ == 0`（仅在 hash table 扩容产生新 group 时短暂 >0），输入列也常常是非 NULL 的。

## 3. 方案设计

### 3.1 修改后的入口逻辑

```cpp
if constexpr (
    std::is_same_v<TData, int64_t> &&
    (std::is_same_v<TValue, int64_t> || std::is_same_v<TValue, int32_t>) &&
    Overflow) {
  // === SVE 加速路径（编译期确定） ===
  DecodedVector decoded(*arg, rows, !mayPushdown);
  if (exec::Aggregate::numNulls_) {
    updateGroups<true, TData, TValue>(groups, rows, arg, ..., decoded);
  } else {
    updateGroups<false, TData, TValue>(groups, rows, arg, ..., decoded);
  }
} else if (exec::Aggregate::numNulls_) {
  // === 非 int64 类型的基类路径 ===
  BaseAggregate::updateGroups<true, TData, TValue>(groups, rows, arg, ...);
} else {
  BaseAggregate::updateGroups<false, TData, TValue>(groups, rows, arg, ...);
}
```

### 3.2 变更点对照

| 项目 | 修改前 | 修改后 |
|------|--------|--------|
| 顶层分支条件 | `if (numNulls_)` | `if constexpr (类型匹配)` |
| `numNulls_ == 0` 时 | 必走基类路径 | **走 SVE 路径** (`updateGroups<false>`) |
| `mayHaveNulls() == false` 时 | 必走基类路径 | **走 SVE 路径** (内部 mode1=0 快速路径) |
| 非 int64 类型 | 走基类路径 | 走基类路径（不变） |
| Lazy pushdown | 优先处理 | 优先处理（不变） |

### 3.3 SVE 路径对 `numNulls_==0` 的处理

`updateGroups<false>` 调用链中，`clearNull` 的开销分析：

```cpp
// Aggregate.h:382 - clearNull 内部短路
inline bool clearNull(char* group) {
    if (numNulls_) {                    // numNulls_ == 0 → 立即返回 false
        uint8_t mask = group[nullByte_];
        if (mask & nullMask_) {
            group[nullByte_] = mask & ~nullMask_;
            --numNulls_;
            return true;
        }
    }
    return false;
}
```

`numNulls_ == 0` 时，每次 `clearNull` 仅多一次 `cmp $0, numNulls_` + `je`（已分支预测命中），开销 < 1 cycle，在批量处理 32 行的上下文中可忽略不计。

### 3.4 SVE 路径对 `mayHaveNulls()==false` 的处理

```cpp
// getBitMask L141-143: mode1 == 0 时
pg = svptrue_b8();   // 全选，O(1) SVE 指令
return pg;
```

当输入无 NULL 时，null mask 直接设为全选 predicate，跳过 bitmap 的 `ldr` 加载和 `svand` 合并，比有 NULL 的情况更快。

## 4. 风险评估

### 4.1 DecodedVector 构造开销

**风险**：原逻辑仅在 `numNulls_ > 0` 时构造 `DecodedVector`；修改后在 `if constexpr` 分支内总是构造。

**评估**：
- `DecodedVector` 构造主要做向量编码解包（dictionary/RLE/constant 展开），不触及数据内容本身
- `if constexpr` 确保非 int64 类型不走这个分支，不会产生额外开销
- 即使对比之前 `numNulls_ == 0` 时走基类路径（不构造 DecodedVector），新增的 DecodedVector 构造开销远小于 SVE 路径带来的加速收益

### 4.2 非 Overflow 场景

**风险**：`checkedPlus<int64_t>` 路径（Overflow=false）能否走 SVE？

**评估**：
- 当前仅 `Overflow=true` 走 SVE，因为 `updateSingleValue` 使用 `SumHook::add`（不检查溢出）
- `Overflow=false` 使用 `checkedPlus`，需要逐行检查溢出并抛异常
- **保持现有约束不变**，`Overflow=false` 仍走基类路径

### 4.3 其他类型扩展

| 类型 | SVE 支持 | 说明 |
|------|---------|------|
| `int64_t` | 完整 | `hashAggUpdateSVEWithCharForNormal` |
| `int32_t` | 完整 | `hashAggUpdateSVEWithCharForNormalInt32` + 密度自适应 |
| `double` | 待支持 | `svadd_f64` 可实现，后续扩展 |
| `int128_t` | 不支持 | SVE 无原生 int128，且 `kMayPushdown=false` |

### 4.4 正确性保证

修改仅改变**哪个函数处理数据**，不改变处理逻辑本身：
- `updateGroups` 内部使用的 `updateSingleValue` 与基类路径完全一致
- `clearNull` / `isNull` 逻辑与基类共享 `Aggregate.h` 中的实现
- NULL 语义（SUM 忽略 NULL）由相同的 mode/bitmap 机制保证

## 5. 测试策略

### 5.1 已有测试覆盖

- `SumAggregationTest` 系列：验证各种类型的 sum 正确性
- `MemoryCapExceededTest`：极端内存场景

### 5.2 需补充场景

| 场景 | 关键参数 | 验证点 |
|------|---------|--------|
| 全非 NULL 输入 + 第二批 | `numNulls_==0`, `mayHaveNulls()==false` | 进入 SVE 路径，结果正确 |
| 全非 NULL 输入 + 单 group | `numNulls_==0`, `mayHaveNulls()==false`, 单 group | 进入 SVE 路径 int32 快速路径 |
| 含 NULL 输入 + 第二批 | `numNulls_==0`, `mayHaveNulls()==true` | 进入 SVE 路径，NULL 正确忽略 |
| 混合 NULL + 常数编码 | `mode2==2`, `mayHaveNulls()==true` | constant 编码正确处理 |
| 混合 NULL + 字典编码 | `mode2==3`, `mayHaveNulls()==true` | dictionary 解码正确 |
| 大批次压力测试 | 100M+ 行 | 性能不退化，无 SIGABRT |

### 5.3 回归检查点

- `numNulls_` 计数是否与基类路径一致（不能多减/少减）
- `clearNull` 调用次数是否匹配
- `Overflow=false` 的 `checkedPlus` 路径是否完全不受影响

## 6. 性能预期

以 100M 行 int64 sum（无 NULL 输入，32K group）为例：

| 指标 | 修改前 | 修改后 | 提升 |
|------|--------|--------|------|
| 首批次（numNulls_>0, 走基类） | ~X ms | ~Y ms（SVE） | 预期 2-4x |
| 后续批次（numNulls_==0, 走基类） | ~X ms | ~Y ms（SVE） | 预期 2-4x |
| 有 NULL 输入批次 | ~SVE ms | ~SVE ms | 持平 |

> 注：具体数值取决于硬件（SVE 向量宽度、内存带宽）和数据分布。需在目标 ARM 平台上实测。

## 7. 后续扩展方向

1. **double/float 类型 SVE 支持**：利用 `svadd_f64`/`svadd_f32` 实现
2. **更多聚合函数复用**：将 `getBitMask`/`clearNullSVE` 等 SVE 工具函数抽入公共头文件，供 `Min`/`Max`/`Count` 等聚合函数复用
3. **`tableHasNulls` 参数利用**：当前 SVE 函数内部通过运行时 `if (numNulls_)` 判断，可进一步改为 `if constexpr (tableHasNulls)` 消除编译期可知的分支
