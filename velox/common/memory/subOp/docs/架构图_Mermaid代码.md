# 架构图 Mermaid 代码

## 1. 整体架构图（第5页使用）

展示插件与 Velox 核心的关系，以及组件层次结构。

```mermaid
graph TB
    subgraph "Velox 核心"
        VeloxCore[Velox 执行引擎]
        OperatorBase[Operator 基类]
        BaseHashTable[BaseHashTable 基类]
    end
    
    subgraph "原始实现"
        HashAgg[HashAggregation<br/>继承 Operator]
        GroupSet[GroupingSet<br/>工具类]
        HashTable[HashTable<br/>继承 BaseHashTable]
        SumAgg[SumAggregateBase]
    end
    
    subgraph "插件实现"
        EnhancedHashAgg[EnhancedHashAggregation<br/>继承 Operator<br/>代码复制]
        EnhancedGroupSet[EnhancedGroupingSet<br/>代码复制<br/>工具类]
        EnhancedHashTable[EnhancedHashTable<br/>继承 BaseHashTable<br/>SVE优化]
        EnhancedSumAgg[EnhancedSumAggregateBase<br/>SVE优化]
        EnhancedDecoded[EnhancedDecodedVector<br/>辅助工具类<br/>提供 getmode1 等函数]
    end
    
    subgraph "插件注册"
        Plugin[enhanced_hash_agg_plugin.so]
        Register[registerEnhancedHashAggregationReplacement]
    end
    
    VeloxCore --> OperatorBase
    OperatorBase --> HashAgg
    OperatorBase --> EnhancedHashAgg
    
    HashAgg --> GroupSet
    EnhancedHashAgg --> EnhancedGroupSet
    
    GroupSet --> HashTable
    GroupSet --> SumAgg
    EnhancedGroupSet --> EnhancedHashTable
    EnhancedGroupSet --> EnhancedSumAgg
    
    BaseHashTable --> HashTable
    BaseHashTable --> EnhancedHashTable
    
    HashTable -.协作关系<br/>并列：HashTable负责分组映射<br/>SumAggregateBase负责聚合计算.-> SumAgg
    EnhancedHashTable -.协作关系<br/>并列：EnhancedHashTable负责分组映射<br/>EnhancedSumAggregateBase负责聚合计算.-> EnhancedSumAgg
    EnhancedSumAgg --> EnhancedDecoded
    
    Plugin --> Register
    Register -.替换.-> EnhancedHashAgg
    
    style EnhancedHashAgg fill:#90EE90
    style EnhancedGroupSet fill:#90EE90
    style EnhancedHashTable fill:#87CEEB
    style EnhancedSumAgg fill:#FFD700
    style EnhancedDecoded fill:#DDA0DD
```

---

## 2. 调用链图（第8页使用）

展示从 HashAggregation 到 HashTable 的调用链，以及 private 变量的关系。

```mermaid
graph LR
    subgraph "原始调用链"
        HA1[HashAggregation<br/>继承 Operator]
        GS1[GroupingSet<br/>工具类]
        HT1[HashTable<br/>继承 BaseHashTable]
        SA1[SumAggregateBase]
        
        HA1 -->|private: groupingSet_| GS1
        GS1 -->|private: table_| HT1
        GS1 -->|使用| SA1
        HT1 -.协作.-> SA1
    end
    
    subgraph "插件调用链（完整替换）"
        EHA[EnhancedHashAggregation<br/>继承 Operator<br/>代码复制]
        EGS[EnhancedGroupingSet<br/>代码复制]
        EHT[EnhancedHashTable<br/>继承 BaseHashTable<br/>优化点⭐]
        ESA[EnhancedSumAggregateBase<br/>SVE优化⭐]
        EDV[EnhancedDecodedVector<br/>辅助工具类<br/>提供 getmode1 等函数]
        
        EHA -->|private: groupingSet_| EGS
        EGS -->|private: table_| EHT
        EGS -->|使用| ESA
        EHT -.协作.-> ESA
        ESA -->|使用| EDV
    end
    
    HA1 -.替换.-> EHA
    
    style EHT fill:#FF6B6B
    style ESA fill:#FF6B6B
    style EDV fill:#DDA0DD
    style HA1 fill:#D3D3D3
    style GS1 fill:#D3D3D3
    style HT1 fill:#D3D3D3
    style SA1 fill:#D3D3D3
```

---

## 3. 类继承关系图（第12页使用）

展示 Enhanced 类与原始类的继承关系，以及 Velox 的设计模式。

```mermaid
classDiagram
    class Operator {
        <<abstract>>
        +MemoryPool* pool_
        +initialize()
        +addInput()
        +getOutput()
    }
    
    class HashAggregation {
        -GroupingSet* groupingSet_
        +initialize()
        +addInput()
    }
    
    class EnhancedHashAggregation {
        -EnhancedGroupingSet* groupingSet_
        +initialize()
        +addInput()
    }
    
    class GroupingSet {
        <<工具类>>
        -HashTable* table_
        +addInput()
        +getOutput()
    }
    
    class EnhancedGroupingSet {
        <<工具类>>
        -EnhancedHashTable* table_
        +addInput()
        +getOutput()
    }
    
    class BaseHashTable {
        <<abstract>>
        +hashers_
        +rows_
        +groupProbe()
        +joinProbe()
    }
    
    class HashTable {
        -HashMode hashMode_
        -char** table_
        -int64_t capacity_
        +setHashMode()
        +clear()
    }
    
    class EnhancedHashTable {
        -HashMode hashMode_
        -NormalizedKeyMode normalizedKeyMode_
        -char** table_
        -int64_t capacity_
        +setHashMode()
        +clear()
        +SVE优化
    }
    
    class SumAggregateBase {
        +add()
        +clear()
    }
    
    class EnhancedSumAggregateBase {
        +add() SVE优化
        +clear() SVE优化
    }
    
    Operator <|-- HashAggregation
    Operator <|-- EnhancedHashAggregation
    HashAggregation *-- GroupingSet : private
    EnhancedHashAggregation *-- EnhancedGroupingSet : private
    GroupingSet *-- HashTable : private
    GroupingSet *-- SumAggregateBase : 使用
    EnhancedGroupingSet *-- EnhancedHashTable : private
    EnhancedGroupingSet *-- EnhancedSumAggregateBase : 使用
    BaseHashTable <|-- HashTable
    BaseHashTable <|-- EnhancedHashTable
    HashTable ..> SumAggregateBase : 协作
    EnhancedHashTable ..> EnhancedSumAggregateBase : 协作
    EnhancedSumAggregateBase --> EnhancedDecodedVector : 使用
    
    note for Operator "Velox 设计：可继承"
    note for BaseHashTable "Velox 设计：可继承的抽象基类"
    note for HashTable "Velox 设计：具体实现，不是为继承设计"
    note for GroupingSet "Velox 设计：工具类，无继承关系"
```

---

## 4. 内存池关系图（第7页使用）

展示内存池的命名和关系，说明如何避免冲突。

```mermaid
graph TB
    subgraph TaskChildPools["Task::childPools_"]
        Task["Task<br/>childPools_: vector&lt;shared_ptr&lt;MemoryPool&gt;&gt;"]
    end
    
    subgraph NodePoolGroup["NodePool"]
        NodePool["NodePool<br/>node.4"]
    end
    
    subgraph OldPoolGroup["旧的内存池（仍存在）"]
        OldPool["MemoryPool<br/>op.4.0.0.PartialAggregation<br/>旧的 HashAggregation"]
        OldOp["HashAggregation<br/>已销毁"]
    end
    
    subgraph NewPoolGroup["新的内存池（唯一名称）"]
        NewPool["MemoryPool<br/>op.4.0.0.EnhancedPartialAggregation<br/>EnhancedHashAggregation"]
        NewOp["EnhancedHashAggregation<br/>新的 operator"]
    end
    
    Task -->|持有| NodePool
    NodePool -->|子池| OldPool
    NodePool -->|子池| NewPool
    
    OldOp -.已销毁.-> OldPool
    NewOp -->|使用| NewPool
    
    style OldPool fill:#FFCCCC
    style NewPool fill:#CCFFCC
    style OldOp fill:#FFCCCC,stroke-dasharray:5 5
    style NewOp fill:#CCFFCC
```

---

## 5. 插件替换流程时序图（第6页使用）

展示插件注册和替换的完整流程。

```mermaid
sequenceDiagram
    participant App as 应用程序
    participant Register as registerEnhancedHashAggregationReplacement
    participant DriverFactory as DriverFactory
    participant Adapter as EnhancedHashAggregationAdapter
    participant Driver as Driver
    participant OldOp as HashAggregation
    participant NewOp as EnhancedHashAggregation
    participant Task as Task
    
    Note over App,Register: 程序启动阶段
    App->>Register: 调用注册函数
    Register->>DriverFactory: 注册替换适配器<br/>(DriverAdapter)
    
    Note over DriverFactory,Task: createDriver 阶段
    DriverFactory->>Driver: 创建 Driver
    DriverFactory->>OldOp: 创建 HashAggregation operator<br/>(内存池: op.4.0.0.PartialAggregation)
    OldOp->>Task: 注册内存池到 childPools_
    
    DriverFactory->>Adapter: 调用适配器函数<br/>(EnhancedHashAggregationDriverAdapter)
    Adapter->>Adapter: 检测到 HashAggregation operator
    
    Adapter->>NewOp: 创建 EnhancedHashAggregation<br/>(内存池: op.4.0.0.EnhancedPartialAggregation)
    NewOp->>Task: 注册新内存池到 childPools_
    
    Adapter->>DriverFactory: replaceOperators()
    DriverFactory->>Driver: 替换 operator
    Driver->>OldOp: 移出旧的 HashAggregation
    Driver-->>Adapter: 返回 replaced vector
    
    Note over NewOp,OldOp: 内存池状态
    Note right of Task: 两个内存池共存：<br/>- op.4.0.0.PartialAggregation (旧)<br/>- op.4.0.0.EnhancedPartialAggregation (新)
    
    Note over Driver,NewOp: Driver 运行阶段
    Note over Driver,NewOp: 后续 Driver 运行时使用<br/>EnhancedHashAggregation<br/>→ EnhancedGroupingSet<br/>→ EnhancedHashTable<br/>→ EnhancedSumAggregateBase
```

---

## 6. 设计演进对比图（第12页使用）

展示 EnhancedHashTable 的设计演进过程。

```mermaid
graph TB
    subgraph "方案1：继承 HashTable（不可行）"
        S1[EnhancedHashTable<br/>: public HashTable]
        S1_Problem[问题：<br/>- virtual 函数需修改 HashTable private 变量<br/>- 无法访问 private 成员<br/>- HashTable 不是为继承设计的]
        S1 --> S1_Problem
        S1_Problem -->|❌| Reject1[不可行]
    end
    
    subgraph "方案2：继承 BaseHashTable（可行）"
        S2[EnhancedHashTable<br/>: public BaseHashTable]
        S2_Advantage[优势：<br/>- BaseHashTable 设计为可继承<br/>- 无 private 变量<br/>- 完全控制状态<br/>- 符合 Velox 设计意图]
        S2 --> S2_Advantage
        S2_Advantage -->|✅| Accept[最终方案]
    end
    
    subgraph "方案3：修改基类（不可行）"
        S3[修改 Velox 核心代码]
        S3_Problem[问题：<br/>- 需要修改外部库<br/>- 破坏插件独立性]
        S3 --> S3_Problem
        S3_Problem -->|❌| Reject2[不可行]
    end
    
    style S1 fill:#FFCCCC
    style S1_Problem fill:#FFCCCC
    style Reject1 fill:#FFCCCC
    style S2 fill:#CCFFCC
    style S2_Advantage fill:#CCFFCC
    style Accept fill:#CCFFCC
    style S3 fill:#FFCCCC
    style S3_Problem fill:#FFCCCC
    style Reject2 fill:#FFCCCC
```

---

## 7. 组件依赖关系图（第11页使用）

展示插件内部组件的依赖关系。

```mermaid
graph TD
    subgraph "插件组件"
        EHA[EnhancedHashAggregation<br/>Operator层]
        EGS[EnhancedGroupingSet<br/>分组层]
        EHT[EnhancedHashTable<br/>哈希表层]
        ESA[EnhancedSumAggregateBase<br/>聚合函数层]
        EDV[EnhancedDecodedVector<br/>辅助工具类<br/>提供 getmode1 等函数]
    end
    
    subgraph "Velox 核心依赖"
        Op[Operator]
        BHT[BaseHashTable]
        RC[RowContainer]
        VH[VectorHasher]
    end
    
    EHA -->|依赖| Op
    EHA -->|使用| EGS
    EGS -->|使用| EHT
    EGS -->|使用| ESA
    EHT -->|继承| BHT
    EHT -->|使用| RC
    EHT -->|使用| VH
    EHT -.协作.-> ESA
    ESA -->|使用| EDV
    
    style EHA fill:#90EE90
    style EGS fill:#90EE90
    style EHT fill:#87CEEB
    style ESA fill:#FFD700
    style EDV fill:#DDA0DD
```

---

## 8. 优化点分布图（第10页使用）

展示 SVE 优化在哪些组件中实现。

```mermaid
graph LR
    subgraph "优化组件"
        EHT[EnhancedHashTable<br/>SVE优化]
        ESA[EnhancedSumAggregateBase<br/>SVE优化]
        EDV[EnhancedDecodedVector<br/>辅助函数<br/>getmode1等]
    end
    
    subgraph "优化特性"
        SVE[SVE 向量化指令<br/>-march=armv8.2-a+crc+sve]
        NK[NormalizedKey优化<br/>NormalizedKeyMode::sve]
        VEC[向量化聚合操作]
    end
    
    EHT -->|使用| SVE
    EHT -->|支持| NK
    ESA -->|使用| SVE
    ESA -->|实现| VEC
    ESA -->|使用| EDV
    EDV -->|提供辅助函数| ESA
    
    style EHT fill:#87CEEB
    style ESA fill:#FFD700
    style EDV fill:#DDA0DD
    style SVE fill:#FF6B6B
    style NK fill:#FF6B6B
    style VEC fill:#FF6B6B
```

---

## 9. Aggregate 函数流转图 - 简化版（新增）

展示 Aggregate 函数从 PlanNode 到聚合计算的完整流转过程（简化版，通用流程，不涉及插件替换）。

```mermaid
graph LR
    subgraph PlanNode["1. PlanNode"]
        AN["AggregationNode<br/>aggregates"]
    end
    
    subgraph HashAgg["2. HashAggregation"]
        HA["initialize"]
        toAgg["toAggregateInfo"]
        AI["aggregateInfos<br/>vector&lt;AggregateInfo&gt;"]
    end
    
    subgraph GroupingSet["3. GroupingSet"]
        GS["GroupingSet<br/>aggregates_: vector&lt;AggregateInfo&gt;<br/>table_: unique_ptr&lt;HashTable&gt;"]
        Acc["accumulators<br/>转换为 Accumulator"]
    end
    
    subgraph HashTable["4. HashTable"]
        HT["HashTable<br/>rows_: unique_ptr&lt;RowContainer&gt;"]
        RC["RowContainer<br/>行布局：keys + flags + accumulators"]
    end
    
    subgraph AggregateFunc["5. AggregateFunction"]
        AF["AggregateFunction<br/>addRawInput<br/>extractValues"]
    end
    
    AN -->|aggregates| HA
    HA -->|调用| toAgg
    toAgg -->|创建 AggregateFunction| AI
    AI -->|传递| GS
    
    GS -->|accumulators| Acc
    GS -->|创建| HT
    Acc -->|传递给| HT
    HT -->|创建| RC
    
    GS -.->|调用 groupProbe<br/>操作行的分组键部分| HT
    GS -.->|使用 AggregateFunction<br/>操作行的聚合结果部分| AF
    
    style AN fill:#E6F3FF
    style AI fill:#FFE6CC
    style GS fill:#E6FFE6
    style HT fill:#FFE6E6
    style RC fill:#E6E6FF
    style AF fill:#FFD700
```

---

## 9-1. Aggregate 函数流转图 - 完整版（新增）

展示 Aggregate 函数从 PlanNode 到聚合计算的完整流转过程，以及插件中各组件的替换关系（完整版，展示所有替换）。

```mermaid
graph LR
    subgraph PlanNode["1. PlanNode"]
        AN["AggregationNode<br/>aggregates"]
    end
    
    subgraph HashAgg["2. HashAggregation"]
        HA["HashAggregation::initialize"]
        HA2["EnhancedHashAggregation::initialize<br/>代码复制"]
        toAgg["toAggregateInfo"]
        AI["aggregateInfos"]
        Replace["替换 sum 为<br/>EnhancedSumAggregateBase"]
    end
    
    subgraph GroupingSet["3. GroupingSet"]
        GS["GroupingSet<br/>代码复制"]
        GS2["EnhancedGroupingSet<br/>aggregates_<br/>table_"]
        Acc["accumulators"]
    end
    
    subgraph HashTable["4. HashTable"]
        HT["HashTable"]
        HT2["EnhancedHashTable<br/>rows_"]
        RC["RowContainer<br/>使用 Accumulator<br/>计算行布局"]
    end
    
    subgraph AggregateFunc["5. AggregateFunction"]
        AF["SumAggregateBase"]
        AF2["EnhancedSumAggregateBase<br/>SVE优化<br/>addRawInput<br/>extractValues"]
    end
    
    AN -->|aggregates| HA
    HA -.->|替换为| HA2
    HA2 -->|调用| toAgg
    toAgg -->|创建| AI
    AI -->|替换| Replace
    Replace -->|传递| GS2
    
    GS -.->|替换为| GS2
    GS2 -->|accumulators| Acc
    GS2 -->|创建| HT2
    Acc -->|传递给| HT2
    HT2 -->|创建| RC
    
    HT -.->|替换为| HT2
    AF -.->|替换为| AF2
    
    GS2 -.->|使用| HT2
    GS2 -.->|使用| AF2
    
    style AN fill:#E6F3FF
    style HA fill:#D3D3D3
    style HA2 fill:#90EE90
    style AI fill:#FFE6CC
    style GS fill:#D3D3D3
    style GS2 fill:#90EE90
    style HT fill:#D3D3D3
    style HT2 fill:#87CEEB
    style RC fill:#E6E6FF
    style AF fill:#D3D3D3
    style AF2 fill:#FFD700
```

---

## 10. HashTable 与 Aggregate 协作关系图（新增）

展示 HashTable 和 AggregateFunction 的协作关系，说明它们不是上下层关系，而是并列协作。

**协作关系（并列）的含义**：
- `HashTable` 和 `AggregateFunction` 都是被 `GroupingSet` 使用的组件
- 它们功能不同，职责分离，操作不同的对象：
  - `HashTable`：负责**分组映射**（**通过 `groupProbe` 计算哪个输入行属于哪个分组**）
    * **操作两个对象**：
      1. **哈希表本身**：通过 `firstProbe` 和 `fullProbe` 在哈希表中查找或插入
      2. **RowContainer 聚合行**：当找到新分组时，调用 `rows_->newRow()` 创建新行，这些行包含分组键（行的前半部分）
    * **返回结果**：`lookup.hits`（`char*` 数组），包含 RowContainer 中行的指针
  - `AggregateFunction`：负责**聚合计算**（**通过 `addRawInput`/`updateGroups` 更新每个分组的聚合数据**）
    * **操作对象**：RowContainer 中行的**聚合结果部分**（accumulators，行的后半部分）
    * `addRawInput` 接收 `groups`（`char**`），即 `lookup.hits.data()`（由 `groupProbe` 返回）
    * 根据 `offset` 定位到每行的 accumulator 区域并更新
- 它们不是继承关系（不是 HashTable 包含 AggregateFunction）
- 它们并列存在，协作完成聚合任务

**Aggregate 流转路径**：
1. `AggregationNode` → `HashAggregation`（通过 `toAggregateInfo` 转换）
2. `HashAggregation` → `GroupingSet`（传递 `AggregateInfo`）
3. `GroupingSet` → `HashTable` → `RowContainer`（通过 `accumulators` 传递 `Accumulator`）
   - `RowContainer` 使用 `Accumulator` 信息计算行的内存布局：
     * `accumulator.fixedWidthSize()`：计算每个 accumulator 的内存大小
     * `accumulator.alignment()`：计算对齐要求
     * `accumulator.isFixedSize()`：判断是否是固定大小
     * `accumulator.usesExternalMemory()`：判断是否需要外部内存
   - 为每个 accumulator 设置 null flag 和 initialized flag（每个 2 bits）
   - 计算每个 accumulator 在行中的偏移量（offset）
   - 处理 spill（`extractForSpill`）和清理（`destroy`）
4. `GroupingSet` 直接使用 `AggregateFunction` 进行聚合计算

```mermaid
sequenceDiagram
    participant GS as GroupingSet
    participant HT as HashTable
    participant AF as AggregateFunction
    participant Input as 输入数据
    
    Note over GS,AF: 聚合处理流程
    
    Input->>GS: addInput()
    GS->>HT: prepareForGroupProbe(lookup, input, rows)
    Note over HT: 在 HashTable 内部：<br/>1. 解码分组键<br/>2. 计算哈希值/ValueIds<br/>3. 填充 lookup.rows
    HT-->>GS: 返回（lookup 已准备好）
    
    GS->>HT: groupProbe(lookup)
    Note over HT: 在 HashTable 内部：<br/>操作两个对象：<br/>1. 哈希表本身（查找/插入）<br/>2. RowContainer 聚合行（创建新行）<br/>填充 lookup.hits (分组行指针数组)
    HT-->>GS: 返回（lookup.hits 已填充）
    
    GS->>AF: initializeNewGroups(groups, newGroups)
    GS->>AF: addRawInput(groups=lookup.hits.data(), ...)
    Note over AF: 根据映射关系更新聚合数据<br/>操作行的聚合结果部分（accumulators）<br/>使用 lookup.hits 确定每行输入对应的分组
    AF-->>GS: 聚合结果更新完成

```



## 使用说明

1. **整体架构图**：用于第5页，展示插件与 Velox 核心的关系
2. **调用链图**：用于第8页，说明为什么需要完整替换调用链
3. **类继承关系图**：用于第12页，展示设计决策和 Velox 设计模式
4. **内存池关系图**：用于第7页，说明内存池冲突解决方案
5. **插件替换流程时序图**：用于第6页，展示替换过程
6. **设计演进对比图**：用于第12页，说明设计决策的演进
7. **组件依赖关系图**：用于第11页，展示代码组织
8. **优化点分布图**：用于第10页，展示 SVE 优化特性
9. **Aggregate 函数流转图 - 简化版**：用于第9页，展示 Aggregate 从 PlanNode 到聚合计算的完整流转（简化版）
9-1. **Aggregate 函数流转图 - 完整版**：用于第9页，展示所有组件的替换关系（完整版）
10. **HashTable 与 Aggregate 协作关系图**：用于第9页，说明 HashTable 和 AggregateFunction 的协作关系

## 在 PPT 中使用

1. 复制对应的 Mermaid 代码
2. 在支持 Mermaid 的工具中渲染（如：
   - [Mermaid Live Editor](https://mermaid.live/)
   - VS Code with Mermaid extension
   - PowerPoint with Mermaid add-in
   - 或导出为图片后插入 PPT）

## 自定义样式

如果需要调整颜色或样式，可以修改：
- `fill:#颜色代码` - 填充颜色
- `stroke:#颜色代码` - 边框颜色
- `stroke-dasharray` - 虚线样式

