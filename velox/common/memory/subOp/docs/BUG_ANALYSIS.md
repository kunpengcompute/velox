# Bug 分析：内存布局不匹配导致的 offset_ 读取错误

## 问题描述

在运行 spark-sql 时，`getOffsetFromAgg()` 返回异常值 `675531264` (0x28400000)，而不是期望的 `10`。

1. new jar 包按照Aggregate.h侵入式修改之后的布局创建对象，`offset_` 存储在偏移量 32 (0x20) 的位置，值为 10。调用 `setOffsetsInternal(10, ...)`写入相应位置。
2. 插件 .so 按照原生velox Aggregate.h布局访问，调用 `getOffsetFromAgg()`，在偏移量 40 (0x28) 的位置读取 `offset_`。

## Bug 原因确认

**是的，将 jar 包替换成基于 `main/velox` 编译的版本就可以解决问题。**

### 当前问题状态

| 组件 | 当前使用的源码 | Aggregate.h 版本 | 内存布局 |
|------|--------------|-----------------|---------|
| **jar 包** (gluten-velox) | `velox/` (侵入式修改) | `nullByte_`, `nullMask_`, `numNulls_` 移到 public | 布局A |
| **插件 .so** | `v1/velox/` (原生) | `nullByte_`, `nullMask_`, `numNulls_` 在 protected | 布局B |

### 解决方案

| 组件 | 修改后使用的源码 | Aggregate.h 版本 | 内存布局 |
|------|----------------|-----------------|---------|
| **jar 包** (gluten-velox) | `main/velox/` (原生) | `nullByte_`, `nullMask_`, `numNulls_` 在 protected | 布局B |
| **插件 .so** | `v1/velox/` (原生) | `nullByte_`, `nullMask_`, `numNulls_` 在 protected | 布局B |

两个组件使用相同的原生版本，内存布局一致，问题解决。

---

## 背后的机制科普

### 1. C++ 类的内存布局（Memory Layout）

C++ 类对象在内存中按照成员声明的顺序排列：

```cpp
class Aggregate {
protected:
    TypePtr resultType_;        // 偏移量: 0
    int32_t nullByte_;         // 偏移量: 8 (假设指针8字节)
    uint8_t nullMask_;         // 偏移量: 12
    int32_t initializedByte_;  // 偏移量: 13 (对齐到16)
    uint8_t initializedMask_;  // 偏移量: 17
    int32_t offset_;           // 偏移量: 20 (对齐到24) ← 关键！
    // ...
};
```

当访问 `offset_` 时，编译器会计算偏移量：
```cpp
offset_ 的地址 = this指针 + 偏移量(24)
```

### 2. 内存布局不匹配的问题

#### 版本A（velox/侵入式修改）：
```cpp
protected:
    TypePtr resultType_;        // 偏移: 0
    // nullByte_ 被注释，不占空间
    // nullMask_ 被注释，不占空间
    int32_t initializedByte_;   // 偏移: 8
    uint8_t initializedMask_;   // 偏移: 12
    int32_t offset_;            // 偏移: 32 (0x20) ← 位置A (实际测量值)
public:
    int32_t nullByte_;          // 偏移: 20 (在public区域)
    uint64_t numNulls_;         // 偏移: 24
    uint8_t nullMask_;          // 偏移: 32
```

**注意：** 实际运行时测量显示 `offset_` 在布局A中的偏移量是 0x20（32字节），而不是之前估计的 0x10。

**为什么估算不准确？**

我之前估算时只考虑了成员变量的简单顺序，忽略了以下关键因素：

1. **虚函数表指针（vtable pointer）**：
   - `Aggregate` 类有虚函数（`virtual ~Aggregate()`, `virtual int32_t accumulatorFixedWidthSize() const = 0` 等）
   - 在64位系统上，对象开头有一个8字节的虚函数表指针
   - 实际布局：`[vtable指针(8字节)][成员变量...]`

2. **内存对齐（Alignment）**：
   - `TypePtr`（通常是 `std::shared_ptr`）需要8字节对齐
   - `int32_t` 需要4字节对齐
   - 编译器会在成员之间插入填充（padding）以满足对齐要求

3. **成员变量的实际顺序**：
   - 需要查看完整的类定义，包括所有成员变量
   - 不同访问控制符（`protected`/`public`）不会改变内存布局顺序
   - 但成员变量的声明顺序决定了它们在内存中的位置

**实际内存布局（布局B，原生版本）：**
```
0x00: vtable pointer (8 bytes)          ← 我之前忽略了！
0x08: resultType_ (TypePtr, 8 bytes)
0x10: nullByte_ (int32_t, 4 bytes)
0x14: nullMask_ (uint8_t, 1 byte) + padding (3 bytes) = 对齐到 0x18
0x18: initializedByte_ (int32_t, 4 bytes)
0x1C: initializedMask_ (uint8_t, 1 byte) + padding (3 bytes) = 对齐到 0x20
0x20: offset_ (int32_t, 4 bytes)        ← 根据代码顺序，应该在这里
0x24: rowSizeOffset_ (int32_t, 4 bytes)
0x28: numNulls_ (uint64_t, 8 bytes)     ← 但实际测量显示 offset_ 在 0x28
...
```

**注意：** 实际测量值显示 `offset_` 在 0x28，这可能是因为：
1. 编译器为了对齐 `numNulls_`（uint64_t，需要8字节对齐）而重新排列了成员
2. 或者有其他成员变量（如 `allocator_` 指针）在 `offset_` 之前
3. 或者编译器优化导致的内存布局与代码声明顺序不完全一致

**实际内存布局（布局A，侵入式修改版本）：**
```
0x00: vtable pointer (8 bytes)
0x08: resultType_ (TypePtr, 8 bytes)
0x10: initializedByte_ (int32_t, 4 bytes)  ← nullByte_ 和 nullMask_ 被移到 public
0x14: initializedMask_ (uint8_t, 1 byte) + padding (3 bytes) = 对齐到 0x18
0x18: (其他成员或 padding)
...
0x20: offset_ (int32_t, 4 bytes)        ← 实际测量值：0x20 (32字节)
```

**关键教训：**
- **理论估算不可靠**：即使知道成员变量顺序，编译器优化、对齐要求、继承关系等都会影响实际布局
- **实际测量是唯一可靠的方法**：使用 `offsetof` 或运行时指针差值计算
- **虚函数表指针很重要**：有虚函数的类，对象开头一定有 vtable 指针（8字节）

**教训：**

- 估算内存布局时必须考虑虚函数表指针
- 必须考虑内存对齐和编译器填充
- 实际测量值比理论估算更可靠
- 记住：**C++标准不保证二进制布局，只保证语义正确性。** 如果需要稳定的二进制布局（如网络协议、文件格式），应该显式序列化/反序列化，而不是直接内存拷贝。

#### 版本B（main/velox 和 v1/velox/原生）：
```cpp
protected:
    TypePtr resultType_;        // 偏移: 0
    int32_t nullByte_;          // 偏移: 8
    uint8_t nullMask_;          // 偏移: 12
    int32_t initializedByte_;   // 偏移: 16
    uint8_t initializedMask_;   // 偏移: 20
    int32_t offset_;            // 偏移: 24 ← 位置B (不同！)
    uint64_t numNulls_;         // 偏移: 32
```

**关键差异：**
- 版本A中，`offset_` 在偏移量 32 (0x20) 的位置（实际测量值）
- 版本B中，`offset_` 在偏移量 40 (0x28) 的位置（实际测量值）
- 两者相差 8 字节

### 3. 为什么会出现错误值 `675531264`？

当 jar 包（布局A）创建对象，插件 .so（布局B）访问时：

```
jar包创建的对象内存布局（布局A）:
[resultType_][initializedByte_][initializedMask_][...][offset_=10][...]
                                                          ↑
                                                          偏移量32 (0x20)的位置，存储的是 offset_=10

插件.so访问时（期望布局B）:
[resultType_][nullByte_][nullMask_][initializedByte_][initializedMask_][offset_?][...]
                                                                          ↑
                                                                          偏移量40 (0x28)的位置，读取到的是其他数据！
```

**问题分析：**
1. jar 包按照布局A创建对象，`offset_` 存储在偏移量 32 (0x20) 的位置，值为 10
2. 插件 .so 按照布局B访问，在偏移量 40 (0x28) 的位置读取 `offset_`
3. 但偏移量 40 (0x28) 在布局A中不是 `offset_` 的位置
4. 读取到的是其他数据，可能是未初始化的随机值，导致读到 `608402912` (0x24437de0) 这样的异常值

### 4. ABI（Application Binary Interface）兼容性

**ABI 兼容性规则：**
- ✅ 相同编译器、相同编译选项、相同类定义 → ABI 兼容
- ❌ 类定义不同（成员顺序/位置不同）→ ABI 不兼容

**你的情况：**
- **jar 包和 .so 使用不同的类定义（内存布局不同）**
- 运行时共享同一个对象实例
- **访问成员时按照各自编译时的偏移量计算**
- 结果：读取到错误的内存位置

### 5. 为什么统一使用 `main/velox` 可以解决？

```
jar包 (main/velox) → 布局B
插件.so (v1/velox，也是原生) → 布局B
                    ↓
            内存布局完全一致
                    ↓
        访问 offset_ 时偏移量相同
                    ↓
            读取到正确的值
```

### 6. 关键要点总结

1. **C++ 类成员的内存位置由声明顺序决定**
   - 成员变量按照声明顺序在内存中排列
   - 每个成员的位置（偏移量）在编译时确定

2. **注释掉成员变量会改变后续成员的偏移量**
   - 注释掉的成员不占用内存空间
   - 后续成员的偏移量会前移

3. **将成员移到不同访问控制区域不会改变偏移量**
   - `protected` → `public` 只是访问权限的改变
   - 但注释掉成员会改变内存布局

4. **不同编译单元必须使用相同的类定义**
   - 如果类定义不同，ABI 不兼容
   - 运行时共享对象时会出现内存布局不匹配

5. **运行时共享对象时，所有组件必须对内存布局有一致的理解**
   - **jar 包和 .so 插件在运行时共享同一个对象实例**
   - **如果它们对内存布局的理解不一致，就会读取到错误的数据**

### 7. 验证方法

可以通过以下方式验证内存布局：

```cpp
#include <cstddef>

// 打印成员变量的偏移量
std::cout << "offset_ offset: " << offsetof(Aggregate, offset_) << std::endl;
std::cout << "nullByte_ offset: " << offsetof(Aggregate, nullByte_) << std::endl;
```

如果两个版本的偏移量不同，就会出现你遇到的问题。

### 8. 实际内存布局对比（简化示例）

假设 `TypePtr` 是 8 字节指针，考虑内存对齐：

**版本A（velox/侵入式修改）：**
```
地址    大小    成员
0x00    8       resultType_
0x08    4       initializedByte_
0x0C    1       initializedMask_
...
0x20    4       offset_          ← 在这里！（实际测量值：0x20 = 32字节）
...
0x24    4       nullByte_ (public)
0x28    8       numNulls_ (public)
0x30    1       nullMask_ (public)
```

**版本B（main/velox 和 v1/velox/原生）：**
```
地址    大小    成员
0x00    8       resultType_
0x08    4       nullByte_
0x0C    1       nullMask_
0x10    4       initializedByte_
0x14    1       initializedMask_
...
0x28    4       offset_          ← 在这里！（实际测量值：0x28 = 40字节）
0x2C    4       rowSizeOffset_
0x30    8       numNulls_
```

**问题：**
- **jar 包在 0x20 处写入 `offset_ = 10`**
- **插件 .so 在 0x28 处读取，但那里存储的是其他数据**
- 结果：读取到错误的值 `608402912` (0x24437de0)

### 9. 为什么写入可以，读取会错？深入解析

这是一个非常关键的问题！让我们详细分析写入和读取的机制：

#### 关键理解：偏移量在编译时确定

**重要事实：**
- C++ 中，成员变量的偏移量是在**编译时**确定的
- 编译器根据**编译时看到的类定义**来计算偏移量
- 运行时无法改变这个偏移量

#### 场景分析

假设对象在 jar 包中创建，内存布局按照**布局A**（velox/侵入式修改版本）排列：

```
实际内存（布局A）:
地址    内容
0x00    resultType_
0x08    initializedByte_
0x0C    initializedMask_
...
0x20    offset_ = 10  ← 实际存储在这里（实际测量值：0x20 = 32字节）
...
0x24    nullByte_ (public)
0x28    numNulls_ (public)
0x2C    nullMask_ (public)
```

#### 写入过程（setOffsetsInternal）

**jar 包中的代码（布局A）：**
```cpp
// 在 Aggregate.cpp 中（基于 velox/ 编译）
void Aggregate::setOffsetsInternal(...) {
    offset_ = offset;  // 编译器计算：this + 0x20（实际测量值）
}
```

**执行过程：**
1. ==jar 包调用 `setOffsetsInternal(10, ...)`==
2. 编译器在编译 jar 包时，根据**布局A**计算 `offset_` 的偏移量 = 0x20（实际测量值）
3. 执行 `offset_ = 10` 时，实际写入地址 = `this指针 + 0x20`
4. ✅ **写入成功**：值 10 被写入到 0x20 位置（这是布局A中 `offset_` 的正确位置）

#### 读取过程（getOffsetFromAgg）

**插件 .so 中的代码（布局B）：**
```cpp
// 在 EnhancedSumAggregateBase.h 中（基于 v1/velox/ 编译）
int32_t getOffsetFromAgg() const {
    return exec::Aggregate::offset_;  // 编译器计算：this + 0x28（实际测量值）
}
```

**执行过程：**
1. ==插件 .so 调用 `getOffsetFromAgg()`==
2. 编译器在编译插件 .so 时，根据**布局B**计算 `offset_` 的偏移量 = 0x28（实际测量值）
3. 执行 `return offset_` 时，实际读取地址 = `this指针 + 0x28`
4. ❌ **读取错误**：从 0x28 位置读取，但那里存储的是其他数据，读取到 `608402912` (0x24437de0)

**对比：如果插件 .so 按照布局A读取：**
- 插件 .so 根据**布局A**计算 `offset_` 的偏移量 = 0x20
- 从 0x20 位置读取 → 读取到正确的值 10 ✅

#### 为什么写入"可以"？

**关键点：写入本身没有"对错"，只是写到了不同的位置！**

实际上：
- jar 包写入到 0x20（布局A中 `offset_` 的位置）✅ 正确
- 但对象的内存布局是布局A，所以 0x20 确实是 `offset_` 的位置 ✅ 匹配

**如果插件 .so 也按照布局A读取：**
- 插件 .so 从 0x20 读取 → 读取到正确的值 10 ✅

**但实际情况是：**
- 插件 .so 按照布局B从 0x28 读取 → 读取到错误的数据 `608402912` (0x24437de0) ❌

#### 核心机制总结

```
编译时确定偏移量：
├─ jar 包编译（布局A）→ offset_ 偏移量 = 0x20（实际测量值）
└─ 插件.so编译（布局B）→ offset_ 偏移量 = 0x28（实际测量值）

运行时访问：
├─ jar 包写入：this + 0x20 → 写入到 0x20（布局A正确位置）✅
└─ 插件.so读取：this + 0x28 → 从 0x28 读取（布局A中不是offset_）❌
```

#### 关键理解

1. **写入和读取都按照各自编译时的偏移量计算**
   - `offset_ = offset;` 使用编译时的偏移量（0x20，布局A）
   - `return offset_;` 也使用编译时的偏移量（0x28，布局B）

2. **为什么写入"看起来可以"？**
   - 因为对象是按照布局A创建的，0x20 确实是 `offset_` 的位置
   - 写入到 0x20 是正确的

3. **为什么读取会错？**
   - 因为插件 .so 按照布局B的偏移量（0x28）读取
   - 但对象是布局A，0x28 位置存储的不是 `offset_`
   - 所以读取到错误的数据 `608402912` (0x24437de0)

4. **如果对象是按照布局B创建的会怎样？**
   - jar 包写入到 0x20（但布局B中 0x20 是其他数据）→ 写入错误位置 ❌
   - 插件 .so 从 0x28 读取（布局B中 0x28 是 `offset_`）→ 读取正确 ✅

#### 完整示例

**场景：对象在 jar 包中创建（布局A）**

```cpp
// 1. jar 包创建对象（布局A）
Aggregate* agg = new EnhancedSumAggregateBase<...>(...);
// 内存布局：resultType_ | initializedByte_ | initializedMask_ | ... | offset_ | ...

// 2. jar 包调用 setOffsets（布局A的偏移量）
agg->setOffsets(10, ...);
// 编译器计算：offset_ 地址 = agg + 0x20（实际测量值）
// 执行：*(agg + 0x20) = 10
// 结果：0x20 位置存储了 10 ✅

// 3. 插件 .so 调用 getOffsetFromAgg（布局B的偏移量）
int32_t offset = agg->getOffsetFromAgg();
// 编译器计算：offset_ 地址 = agg + 0x28（实际测量值）
// 执行：return *(agg + 0x28)
// 结果：从 0x28 读取，但那里是其他数据，读取到 608402912 (0x24437de0) ❌
```

**关键点：**
- 写入和读取都使用各自编译时的偏移量
- 写入"可以"是因为对象是按照写入方的布局创建的
- 读取"错误"是因为读取方使用了不同的偏移量

---

## 解决方案

### 方案1：统一使用原生版本（推荐）

**操作：**
1. 将 jar 包（gluten-velox）改为基于 `main/velox` 编译
2. 插件 .so 继续使用 `v1/velox` 编译（两者都是原生版本）

**优点：**
- 内存布局完全一致
- 不需要修改代码
- 符合"不修改原生代码"的原则

**缺点：**
- 需要重新编译 jar 包

### 方案2：统一使用修改版本（不推荐）

**操作：**
1. 在 `v1/velox/velox/exec/Aggregate.h` 中应用与 `velox/` 相同的修改
2. jar 包和 .so 都使用修改后的版本

**优点：**
- 两个组件使用相同的修改版本

**缺点：**
- 需要修改原生代码（违背了提取插件的初衷）
- 维护成本高

---

## 结论

**推荐使用方案1：将 jar 包改为基于 `main/velox` 编译。**

因为 `main/velox` 和 `v1/velox` 都是原生版本，内存布局一致，可以安全地共享对象。这样既解决了问题，又保持了代码的整洁性。

否则，ABI 不匹配：jar 包和插件 .so 使用了不同的 Aggregate.h 定义，导致 offset_ 的内存偏移量不同。

---

## 参考资料

- [C++ ABI Compatibility](https://gcc.gnu.org/onlinedocs/libstdc++/manual/abi.html)
- [Memory Layout of C++ Objects](https://en.cppreference.com/w/cpp/language/object)
- [offsetof macro](https://en.cppreference.com/w/cpp/types/offsetof)

