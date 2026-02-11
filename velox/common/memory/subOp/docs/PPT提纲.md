# Velox 增强哈希聚合插件设计 - PPT 提纲

## 第1页：封面
- **标题**：Velox 增强哈希聚合插件设计
- **副标题**：插件化架构与性能优化方案
- 日期、作者

---

## 第2页：目录
1. 背景与目标
2. 架构设计
3. 核心技术方案
4. 实现细节
5. 设计决策
6. 优势与收益
7. 总结与展望

---

## 第一部分：背景与目标

### 第3页：项目背景
- Velox 执行引擎中的哈希聚合优化需求
- 需要在不修改 Velox 核心代码的前提下进行优化
- 插件化设计的必要性
- **目标**：独立维护、易于集成、性能提升

### 第4页：设计目标
- **独立性**：作为插件独立编译和部署
- **兼容性**：与 Velox 核心代码无侵入
- **可扩展性**：支持未来优化和定制
- **性能**：针对 ARM 架构的 SVE 优化

---

## 第二部分：架构设计

### 第5页：整体架构图
- 插件与 Velox 核心的关系
- 组件层次结构：
  - `EnhancedHashAggregation` (Operator层)
  - `EnhancedGroupingSet` (分组层)
  - `EnhancedHashTable` (哈希表层)
  - `EnhancedSumAggregateBase` (聚合函数层)
  - `EnhancedDecodedVector` (向量解码层)

### 第6页：插件集成方式
- **动态库形式**：`enhanced_hash_agg_plugin.so`
- **注册机制**：`registerEnhancedHashAggregationReplacement()`
- **运行时替换**：通过 Adapter 模式替换原始 Operator
- **内存池隔离**：独立的内存池命名空间

---

## 第三部分：核心技术方案

### 第7页：内存池名称冲突解决方案
- **问题**：替换 Operator 时内存池名称冲突
- **原因分析**：
  - 内存池命名格式：`op.{planNodeId}.{pipelineId}.{driverId}.{operatorType}`
  - 旧内存池生命周期问题
- **解决方案**：
  - 继承 `Operator` 而非 `HashAggregation`
  - 使用不同的 `operatorType`（`EnhancedPartialAggregation`）
  - 创建唯一的内存池名称

### 第8页：调用链分析与切入口选择 🚩
- **替换流程**：`registerUDF` → `registerAdapter` → `replaceOperator`
- **优化位置**：主要在 `EnhancedHashTable` 和 `EnhancedSumAggregateBase`
- **调用链问题**：
  ```
  HashAggregation → GroupingSet → HashTable
       ↓                ↓            ↓
  groupingSet_ (private)  table_ (private)  优化点
  ```
- **Velox 设计模式观察**：
  * `HashAggregation` 继承 `Operator`（有继承关系，可扩展）
  * `HashTable` 继承 `BaseHashTable`（有继承关系，可扩展）
  * `GroupingSet` **没有继承关系**，更像是一个**工具类/组合类**（不是为继承设计的）
- **核心挑战**：为了让调用链进入 `EnhancedHashTable`，必须让整个调用链都走自己的代码
- **解决方案**：从入口开始替换，`EnhancedHashAggregation` 和 `EnhancedGroupingSet` 都采用代码复制

### 第8-1页：设计模式选择
- **组合 vs 继承**的权衡
- **为什么选择代码复制而非继承**：
  - 基类 private 变量无法访问（`groupingSet_`, `table_`）
  - 状态一致性要求
  - 插件化的独立性需求
- **设计原则**：实用主义 > 理论优雅

---

## 第四部分：实现细节

### 第9页：核心组件实现

**一、组件层次结构**
```
EnhancedHashAggregation (Operator 层)
  └─→ EnhancedGroupingSet (分组逻辑层)
      ├─→ EnhancedHashTable (分组映射)
      │   └─→ RowContainer (行存储)
      └─→ EnhancedSumAggregateBase (聚合计算)
          └─→ EnhancedDecodedVector (辅助工具)
```

**二、各层组件详解**

**1. EnhancedHashAggregation（Operator 层）**
- **定位**：Velox Operator 接口实现，替换 `HashAggregation`
- **初始化流程**（`initialize()` 方法）：
  * 从 `AggregationNode` 获取 `aggregates`（`vector<Aggregate>`）
  * 调用 `toAggregateInfo()` 转换为 `aggregateInfos`（`vector<AggregateInfo>`，包含 `AggregateFunction*`）
  * **替换 AggregateFunction**（第84-133行）：
    - 遍历 `aggregateInfos`，检测 sum/sum_partial 函数（BIGINT 类型）
    - 根据原始 aggregate 的类型名检测 Spark/Presto SQL（判断 Overflow 参数）
    - 创建 `EnhancedSumAggregateBase` 实例替换 `aggregateInfos[i].function`
  * **创建 EnhancedGroupingSet**（第159行）：
    - `groupingSet_ = std::make_unique<EnhancedGroupingSet>(..., std::move(aggregateInfos), ...)`
    - 传入替换后的 `aggregateInfos`（包含 `EnhancedSumAggregateBase`）
- **运行时调用**：
  * `addInput()` → `groupingSet_->addInput(input, mayPushdown_)`
  * `getOutput()` → `groupingSet_->getOutput(...)`

**2. EnhancedGroupingSet（分组逻辑层）**
- **定位**：分组逻辑协调器，同时使用 HashTable 和 Aggregate
- **初始化**：
  * 构造函数接收 `vector<AggregateInfo>`，存储为成员变量 `aggregates_`
  * 延迟创建 HashTable：在首次 `addInput()` 时调用 `createHashTable()`（如果 `table_` 为空）
- **创建 EnhancedHashTable**（`createHashTable()` 方法，第380-429行）：
  * 调用 `accumulators(false)` 将 `aggregates_` 中的 `AggregateFunction` 转换为 `Accumulator`
  * 创建 `table_ = EnhancedHashTable<...>::createForAggregation(std::move(hashers_), accumulators, &pool_)`
  * 通过 `table_->rows()` 获取 `RowContainer`，调用 `initializeAggregates()` 设置 aggregate offsets
- **处理输入数据**（`addInputForActiveRows()` 方法，第240-310行）：
  * **调用 HashTable**：
    - `table_->prepareForGroupProbe(*lookup_, input, activeRows_, ...)`（第249行）
    - `table_->groupProbe(*lookup_, ...)`（第264行）
    - 获取 `lookup_->hits.data()` 作为 `groups`（行指针数组，第267行）
  * **调用 AggregateFunction**：
    - 遍历 `aggregates_`，对每个 aggregate：
      - 新分组时：`aggregates_[i].function->initializeNewGroups(groups, newGroups)`（第290行）
      - 聚合计算：`aggregates_[i].function->addRawInput(groups, rows, tempVectors_, ...)`（第305行）

**3. EnhancedHashTable（分组映射层）**
- **定位**：哈希表实现，支持 SVE 优化，继承自 `BaseHashTable`
- **创建**（`createForAggregation()` 静态方法）：
  * 接收参数：`hashers`、`accumulators`（`vector<Accumulator>`）、`pool`
  * 在构造函数中创建 `RowContainer`：`rows_ = std::make_unique<RowContainer>(keys, nullableKeys, accumulators, ...)`
  * `Accumulator` 包含 `AggregateFunction*` 和 `intermediateType`，用于 RowContainer 计算行布局
- **准备探测**（`prepareForGroupProbe()` 方法）：
  * 解码分组键：遍历 `hashers`，调用 `hasher->decode()` 和 `hasher->hash()`/`computeValueIds()`
  * 填充 `lookup.hashes` 和 `lookup.rows`
- **分组探测**（`groupProbe()` 方法）：
  * **操作哈希表**：通过 `firstProbe` 和 `fullProbe` 在哈希表中查找或插入
  * **操作 RowContainer**：当找到新分组时，调用 `rows_->newRow()` 创建新行
  * **填充结果**：将行的指针存储在 `lookup.hits[i]` 中（`char*` 数组）
    - 每行包含分组键（行的前半部分）
    - 每行对应一个输入行的分组

**4. RowContainer（行存储层）**
- **定位**：行容器，存储分组数据
- **行的内存布局**（从前往后）：
  * **分组键部分**（keys）：行的前半部分，存储分组键值
  * **标志位部分**（flags）：null flags（每个 key 一个 bit）+ accumulator flags（每个 accumulator 2 bits）
  * **聚合结果部分**（accumulators）：行的后半部分，存储聚合结果数据
- **使用 Accumulator 信息**：
  * 计算行的内存布局（row layout）：`fixedWidthSize()`、`alignment()`、`isFixedSize()`、`usesExternalMemory()`
  * 设置标志位：为每个 accumulator 设置 null flag 和 initialized flag（每个 2 bits）
  * 计算偏移量：为每个 accumulator 计算在行中的偏移量（offset）
  * 处理 spill 和清理：使用 `extractForSpill()` 和 `destroy()` 函数
- **关键点**：RowContainer 使用 Accumulator 的**元数据信息**（大小、对齐、标志位），但不直接调用 AggregateFunction 的聚合逻辑

**5. EnhancedSumAggregateBase（聚合计算层）**
- **定位**：SVE 优化的聚合函数，替换 `SumAggregateBase`
- **初始化**（`initializeNewGroups()` 方法）：
  * 当 `EnhancedGroupingSet` 检测到新分组时调用（第290行）
  * 初始化新分组行的 accumulator 区域
- **聚合计算**（`addRawInput()` 方法）：
  * **接收参数**：
    - `groups`（`char**`）：即 `lookup.hits.data()`（由 `groupProbe` 返回的行指针数组）
    - `rows`：`SelectivityVector`，指定哪些输入行需要处理
    - `args`：`vector<VectorPtr>`，聚合函数的输入数据
  * **操作对象**：RowContainer 中行的**聚合结果部分**（accumulators，行的后半部分）
    - 根据 `offset_`（在 `setOffsets()` 时设置）定位到每行的 accumulator 区域
    - 使用 SVE 指令优化聚合计算
    - 更新 accumulator 中的聚合结果数据
- **关系**：与 `EnhancedHashTable` 是**协作关系（并列）**
  * 两者都被 `EnhancedGroupingSet` 使用
  * 功能不同：
    - HashTable 负责**分组映射**（操作行的分组键部分，返回 `lookup.hits`）
    - AggregateFunction 负责**聚合计算**（操作行的聚合结果部分，使用 `lookup.hits`）
  * 不是继承关系（不是 HashTable 包含 AggregateFunction）
  * 并列存在，协作完成聚合任务

**6. EnhancedDecodedVector（辅助工具层）**
- **定位**：辅助工具类，不是优化组件
- **作用**：主要为 `EnhancedSumAggregateBase` 提供 `getmode1` 等辅助函数

**Aggregate 流转路径**：
```
AggregationNode (aggregates: vector<Aggregate>)
  ↓ toAggregateInfo()
HashAggregation (aggregateInfos: vector<AggregateInfo>)
  ↓ 替换 sum 为 EnhancedSumAggregateBase
GroupingSet (aggregates_: vector<AggregateInfo>)
  ├─→ HashTable → RowContainer (通过 accumulators 传递 Accumulator)
  │   └─ RowContainer 使用 Accumulator 元数据：
  │      - 计算行内存布局（大小、对齐、偏移量）
  │      - 设置 null/initialized flags
  │      - 处理 spill 和清理
  └─→ AggregateFunction (直接使用进行聚合计算)
      └─ 使用 RowContainer 中已分配的内存空间存储聚合结果
```

**聚合函数类继承关系**：
```
exec::Aggregate (基类)
    ↓
SimpleNumericAggregate<TInput, TAccumulator, TResult> (模板基类)
    ↓
SumAggregateBase<TInput, TAccumulator, ResultType, Overflow> (标准实现)
    └─ EnhancedSumAggregateBase<TInput, TAccumulator, ResultType, Overflow> (SVE优化版本)
```

**详细说明**：

1. **`exec::Aggregate`** (基类)
   - **位置**：`velox/exec/Aggregate.h`
   - **作用**：聚合函数的抽象基类，定义聚合函数的通用接口
   - **核心方法**：
     - `addRawInput()` - 处理原始输入数据
     - `addIntermediateResults()` - 处理中间结果
     - `extractValues()` - 提取最终结果
     - `accumulatorFixedWidthSize()` - 返回累加器固定大小
     - `accumulatorAlignmentSize()` - 返回累加器对齐大小

2. **`SimpleNumericAggregate`** (模板基类)
   - **位置**：`velox/functions/lib/aggregates/SimpleNumericAggregate.h`
   - **继承**：`public exec::Aggregate`
   - **作用**：为数值类型聚合提供通用实现框架
   - **特点**：
     - 模板参数：`<TInput, TAccumulator, TResult>`
     - 提供 `doExtractValues()` 模板方法用于提取值
     - 默认实现 `extractAccumulators()` 调用 `extractValues()`
     - 简化了数值聚合函数的实现

3. **`SumAggregateBase`** (标准 Sum 聚合基类)
   - **位置**：`velox/functions/lib/aggregates/SumAggregateBase.h`
   - **继承**：`public SimpleNumericAggregate<TInput, TAccumulator, ResultType>`
   - **作用**：Sum 聚合函数的标准实现
   - **特点**：
     - 模板参数：`<TInput, TAccumulator, ResultType, Overflow>`
     - 实现 `addRawInput()`、`addIntermediateResults()` 等核心方法
     - 处理溢出检查（`Overflow` 参数控制是否检查溢出）
     - 使用 `updateInternal()` 进行内部更新逻辑

4. **`EnhancedSumAggregateBase`** (SVE 优化版本)
   - **位置**：`velox/common/memory/subOp/include/enhanced_hash_agg/EnhancedSumAggregateBase.h`
   - **继承**：`public SimpleNumericAggregate<TInput, TAccumulator, ResultType>`
   - **作用**：使用 ARM SVE 指令集优化的 Sum 聚合实现
   - **特点**：
     - 与 `SumAggregateBase` 并行，都继承自 `SimpleNumericAggregate`
     - 包含 SVE 优化方法（如 `hashAggUpdateSVEWithChar()`）
     - 用于替换标准 `SumAggregateBase` 以提升 ARM 架构性能
     - 保持相同的接口，实现插件化替换

**设计模式**：
- **模板方法模式**：`Aggregate` 定义接口，`SimpleNumericAggregate` 提供通用实现框架
- **策略模式**：`SumAggregateBase` 和 `EnhancedSumAggregateBase` 提供不同的实现策略
- **插件化替换**：运行时通过 `EnhancedHashAggregation` 检测并替换 `SumAggregateBase` 为 `EnhancedSumAggregateBase`

### 第10页：SVE 优化特性
- ARM 架构特定优化
- 编译选项：`-march=armv8.2-a+crc+sve`
- 向量化聚合操作
- 性能提升点

### 第11页：代码组织
- 目录结构
- 编译配置（CMakeLists.txt）
- 依赖关系
- 模块划分

---

## 第五部分：设计决策

### 第12页：关键设计决策🚩
1. **继承 Operator 而非 HashAggregation**
   - 原因：内存池名称唯一性
   - 效果：避免冲突

2. **调用链完整替换（代码复制）**
   - 原因：`groupingSet_` 和 `table_` 都是 private，无法从外部替换
   - **设计模式理解**：
     * `HashAggregation` 继承 `Operator`（可继承）
     * `HashTable` 继承 `BaseHashTable`（可继承）
     * `GroupingSet` 没有继承关系，是工具类/组合类（不是为继承设计的）
   - 效果：确保调用链进入 `EnhancedHashTable`
   - 实现：`EnhancedHashAggregation` 和 `EnhancedGroupingSet` 都代码复制

3. **EnhancedHashTable 继承 BaseHashTable 而非 HashTable**
   - 最初想法：继承 `HashTable`，通过 virtual 函数重写
   - 问题：virtual 函数需要修改 `HashTable` 的 private 变量，无法实现
   - **设计意图理解**：
     * `BaseHashTable`：Velox 设计为可继承的抽象基类（无 private 变量）
     * `HashTable`：具体实现类，不是为继承设计的（有 private 变量）
   - 最终方案：继承 `BaseHashTable`，像 `HashTable` 那样维护自己的 private 变量
   - 效果：完全控制状态，避免状态分离问题，符合 Velox 的设计意图

4. **插件化架构**
   - 原因：独立维护、无侵入
   - 效果：易于集成和升级

### 第13页：设计权衡分析 🚩

#### EnhancedHashAggregation & EnhancedGroupingSet
| 方案 | 优点 | 缺点 | 选择 |
|------|------|------|------|
| 继承基类 | 代码复用 | `groupingSet_`/`table_` 是 private，无法替换；`GroupingSet` 不是为继承设计的 | ❌ |
| 组合模式 | 可替换成员 | 无法修改 private 成员，调用链无法进入优化代码 | ❌ |
| 代码复制 | 完全控制调用链，确保进入 `EnhancedHashTable` | 代码量增加 | ✅ |

**设计模式理解**：
- `HashAggregation` 继承 `Operator`（有继承关系，可扩展）
- `HashTable` 继承 `BaseHashTable`（有继承关系，可扩展）
- `GroupingSet` **没有继承关系**，是**工具类/组合类**（不是为继承设计的）

#### EnhancedHashTable
| 方案 | 优点 | 缺点 | 选择 |
|------|------|------|------|
| 继承 HashTable | 代码复用 | HashTable 不是为继承设计的（有 private 变量），无法扩展 | ❌ |
| 继承 BaseHashTable | 符合 Velox 设计意图，完全控制状态 | 需要维护自己的 private 变量 | ✅ |
| 修改基类 | 理论最优 | 需要修改外部库 | ❌ |

**设计意图理解**：
- `BaseHashTable`：Velox 设计为可继承的抽象基类（无 private 变量）
- `HashTable`：具体实现类，不是为继承设计的（有 private 变量）

---

## 第六部分：优势与收益

### 第14页：技术优势
- **独立性**：插件独立维护，不影响 Velox 核心
- **可扩展性**：易于添加新优化
- **兼容性**：与 Velox 版本解耦
- **清晰性**：代码边界明确

### 第15页：性能收益（如果有数据）🚩
- SVE 优化带来的性能提升
- 哈希表优化效果
- 聚合函数优化效果
- 整体性能对比

### 第16页：工程收益
- **维护成本**：独立维护，降低耦合
- **集成成本**：插件化，易于集成
- **升级成本**：与 Velox 升级解耦
- **测试成本**：独立测试，边界清晰

---

## 第七部分：总结与展望

### 第17页：总结
- ✅ 成功实现插件化架构
- ✅ 解决了内存池冲突问题
- ✅ 采用实用的设计决策
- ✅ 为未来优化奠定基础

### 第18页：未来规划
- 性能优化方向
- 功能扩展计划
- 兼容性维护
- 社区贡献计划

### 第19页：Q&A
- 预留问答页

---

## PPT 制作建议

### 1. 视觉设计
- 使用架构图、流程图、类图
- 关键代码片段用代码块展示
- 对比表格突出设计决策

### 2. 重点突出
- **内存池冲突解决方案**（核心问题）
- **设计决策的权衡过程**（体现思考）
- **插件化架构的优势**（价值）

### 3. 技术细节
- 第9-11页可深入，其他页保持概述
- 准备代码示例（关键函数签名）
- 准备架构图（组件关系）

### 4. 数据支撑
- 如有性能数据，在第15页展示
- 代码统计（行数、模块数）
- 测试覆盖情况

---

## 关键代码示例（可选）

### 内存池命名对比
```cpp
// 旧的 HashAggregation
MemoryPool: "op.4.0.0.PartialAggregation"

// EnhancedHashAggregation
MemoryPool: "op.4.0.0.EnhancedPartialAggregation"
```

### 插件注册
```cpp
void registerEnhancedHashAggregationReplacement();
```

### 架构层次与调用链
```
替换流程：
registerUDF → registerAdapter → replaceOperator
    ↓
替换整个 HashAggregation operator

调用链（必须完整替换）：
HashAggregation (继承 Operator, private: groupingSet_)
    ↓
GroupingSet (工具类/组合类, 无继承关系)
    ├── private: table_ → HashTable (继承 BaseHashTable) → 优化点：EnhancedHashTable
    │   └── groupProbe: 计算输入数据到分组的映射关系
    └── 使用 → SumAggregateBase → 优化点：EnhancedSumAggregateBase (SVE)
        └── updateGroups: 更新每行输入数据对应分组的聚合数据
        └── 使用 → EnhancedDecodedVector (提供 getmode1 等辅助函数)

插件实现：
EnhancedHashAggregation (继承 Operator，代码复制)
    └── EnhancedGroupingSet (代码复制，工具类)
            ├── EnhancedHashTable (继承 BaseHashTable) - 协作关系
            └── EnhancedSumAggregateBase (SVE优化) - 协作关系
                └── EnhancedDecodedVector (辅助函数)

Velox 设计模式：
- HashAggregation : Operator (可继承)
- HashTable : BaseHashTable (可继承)
- GroupingSet (工具类，无继承关系)
```

### 设计演进过程
```
EnhancedHashTable 设计演进：

1. 最初想法：
   EnhancedHashTable : public HashTable
   - 通过 virtual 函数重写部分逻辑
   - 复用 HashTable 的已有函数
   
2. 发现问题：
   - virtual 函数需要修改 HashTable 的 private 变量
   - 无法访问 HashTable 的 private 成员
   - **设计意图理解**：Velox 没有打算让 HashTable 成为可扩展继承的类
     * BaseHashTable：设计为可继承的抽象基类（无 private 变量）
     * HashTable：具体实现类，不是为继承设计的（有 private 变量）
   
3. 最终方案：
   EnhancedHashTable : public BaseHashTable
   - 继承 BaseHashTable（Velox 设计为可继承的基类）
   - 像 HashTable 那样维护自己的 private 变量
   - 完全控制状态，避免状态分离问题
   - **符合 Velox 的设计意图**：通过 BaseHashTable 扩展，而非继承 HashTable
```

