# Velox x86 / ARM 数据不一致分析与验证方案：Excel 第 34–65 行

分析日期：2026-10-16。范围为 `sql筛选_95条.xlsx` 中工作表 **`sql筛选_95条 (2)` 的物理第 34–65 行**，共 32 条，行号列取值为 94–206 范围内的指定记录，并非该数值区间内的所有整数。

**测试口径（用户确认）：baseline、baseline_again 为两次 Velox x86 运行，new 为一次 Velox ARM 运行；本批次没有原生 Spark 运行数据。** 文中 baseline/new 均按此含义使用，不能将结果解释为原生 Spark 与 Velox 的实测差异。

## 1. 先读结论

这 32 条不能统一解释为“浮点误差”或“集合无序”。文件支持的主要排查方向是：数值计算尾差、数值边界放大成离散输出、窗口/排名改变记录选择、源数据/样本配对，以及外部 Python 运行路径。**本报告已完成文件交叉核对和静态 SQL 分析；未连接生产 Spark/Velox 环境执行 SQL。逐条原因是有证据支持的候选，最终确认条件写在各条验证方案中。**

- 32 条的 `baseline` 与 `baseline_again` 的 size 和全部字段 checksum 一致；这能说明两次基线观测相同，不能证明 SQL 在所有执行方式下确定，也不能证明源快照与 new 相同。
- 29 条 size 相同；115 为 new 少 1 条，119 为 new 多 25 条，166 为 new 多 1 条。size 差是净值，不代表差异记录总数。
- 31 条 new/baseline 的完整 SQL 文本相同。159 只差 ADD FILE 的临时 session 路径，业务日期和查询正文相同；两个 baseline 之间也只差该路径。
- 20 条含非空的具体差异值样本；其余 12 条只有字段 checksum/SQL 等证据。报告没有将缺失的差异值补造为已知结果。
- 103、121、172 的样本是字符串内数值不同；138 是 `1122.38/1122.37`；206 是候选成员增减。这些不能按集合排序差异直接关闭。
- 122 的样本变化是联系人 token，204 的样本坐标已经不同，均应先查来源和完整记录配对。
- 175 需要先确认 ROW_NUMBER→RANK 改写。完整 ROW_NUMBER 在固定 N 行下始终产生 1..N，单纯同分行交换不能改变无序逐列排名 checksum。

### 1.1 已确认的运行平台与待核对的执行细节

已确认 **baseline/baseline_again = Velox x86，new = Velox ARM**。文件中两边 build 均包含 `gluten.backend_type=velox`，且 32 条均记录相同的 Gluten revision `5c17852c70bdf448e0274f3d6dc68c98e1f9b906`、Velox revision `901e2e0f0c0cb4f24676f287744e6e902d7e7244`，Spark 集成版本为内部 `3.5.1-mt-1.0.0-rc7-SNAPSHOT`。ARM new 的编译器为 GCC 10.3.1，x86 baseline 为 GCC 10.2.1。配置中两边均指定 mjdk-17.0.19。

本报告重点排查同一 Velox 代码版本在 x86/ARM 上的数学库、编译产物、表达式执行、聚合合并和定制规则差异，同时排除输入及比较器问题。相同源码 revision 不等于相同数学库或编译行为；两次 x86 基线一致只证明当前两次观测稳定，单次 ARM 运行尚不足以判断 ARM 侧是否稳定。应先补充 ARM 重复运行，再取得 executed plan、回退信息、有效配置、CPU 型号、libm/编译参数及 UDF 实现，确认各阶段的实际路径。

后文引用 Spark 的 SQL 语义和公开源码，是因为本任务使用 Spark SQL 方言及 Gluten 集成，并非声称存在原生 Spark 测试结果。任何 JVM/原生 Spark 回退实验都只是可选的新增定位对照；即使新增结果与某一侧一致，也不能单凭多数一致就判定另一侧错误。

### 1.2 内部确定化配置会改变分析前提

三次运行的 conf 中均存在以下设置，不能仅依据未优化的 SQL 文本判断实际行为：

```text
spark.sql.convert.rowNumber.to.rank=true
spark.sql.deterministic.collect.aggregate.function=true
spark.sql.deterministic.first.function=true
spark.sql.deterministic.unix_timestamp.function=true
spark.sql.convert.percentileApprox.to.percentile=true
spark.sql.convert.limit.to.nolimit=true
spark.sql.deterministic.rand.function=true
```

这些内部设置的具体实现没有随包提供。验证时保存 analyzed/optimized/executed plan，确认规则是否匹配、是否两边一致、是否被其他配置覆盖。不能假定它们一定生效，也不能忽略它们而直接套用普通 Spark 的随机性解释。

## 2. 文件证据与关联规则

源分片 `part00000`–`part00025` 按编号拼接后成功解压，共四个文件。原始分片及工作簿未修改，解压内容保存在 `extracted/`。

| 资料 | 本次使用方式 |
| --- | --- |
| [sql筛选_95条.xlsx](extracted/sql筛选_95条.xlsx) | 第一工作表 A34:N65 确定 32 条范围；另一工作表结构不同，不按其物理行数取数 |
| [1791959046900_324条_分析.csv](extracted/1791959046900_324条_分析.csv) | 用 `序号=行号`、plan_name、etl_name 关联，复核差异字段；自动提取的表达式和原因只作线索 |
| [1791959046900_原始数据.csv](extracted/1791959046900_原始数据.csv) | 主证据。逻辑记录序号含表头，从 2 起，与 Excel 行号相同；解析 detail 获得三次运行 SQL、DDL、conf、build、size、checksum、advance_check |
| [1791959046900_原始行号+detail列.json](extracted/1791959046900_原始行号+detail列.json) | 按对象 `row` 精确定位，作为交叉证据；detail 是嵌套的 JSON 字符串，需要二次解析 |

**JSON 质量问题。** 本范围中 18 条 detail 可严格解析且与原始 CSV 内容完全相同；13 条存在控制字符/引号/反斜杠转义损坏，不能直接二次解析；138 可解析但三次运行的 DDL 文本与 CSV 不同，size/checksum 一致。13 条为 98、103、122、124、148、154、155、169、175、199、201、202、204。因此统一使用原始 CSV 中可严格解析的 detail，不对损坏 JSON 猜测性修复后执行 SQL。

**追溯路径。** 每条的字段与差异样本分别来自 `detail.new/baseline/baseline_again.checksum`、`detail.advance_check.<字段>.diff` 和 `.equals`；SQL、目标类型来自 `.new.sql/.new.ddl`。CSV 含带换行的字段，文中的“行号”指逻辑记录号，不能用文本编辑器物理行号直接截取。advance_check 没有给 diff 数组两元素明确命名，本报告写“两值/两侧”，不擅自赋予 new/baseline 方向。`.equals` 仅是样本中的相同字段，不保证构成唯一业务键。

**证据边界。** 包内没有完整结果集、源表快照/源表完整 DDL、实际执行计划、checksum/advance_check 实现、UDF JAR 和 159 的 Python 脚本。原表中“已确认跨平台精度”“acceptable_diff”“unstable_sql”等是历史标签，不等于本次已验证的根因。checksum 是指纹，其数值差不能用来估算业务值差、差异行数；逐列指纹一致也不能保证列之间的对应关系一致。

## 3. 逐条总览

下表按 Excel 原顺序列出。size 直接引用运行记录；字段名与 new/baseline checksum 的实际差异集合逐条核对一致。

| Excel行 | 行号 | new / baseline size | 不一致列 | 首要排查方向 |
| --- | --- | ---: | --- | --- |
| 34 | [94](#case-94) | 110 / 110 | pdp_gtv | 周报 GTV：平均值跨越取整边界 |
| 35 | [96](#case-96) | 1,231 / 1,231 | reviewdis | 评论距离：近距离 ACOS 敏感性及 FLOAT 写入 |
| 36 | [98](#case-98) | 202 / 202 | total_roi_pay_cnt | 召回策略金额：MAX 后 SUM 的累计误差 |
| 37 | [99](#case-99) | 3,333,232 / 3,333,232 | geo_type, order_id, travel_distance_type, user_id | 支付旅行类型：距离分档、去重与样本配对 |
| 38 | [103](#case-103) | 343,627 / 343,627 | extension_info, index_list, item_personattr | 商品索引：EXP 得分尾数被嵌入三个字段 |
| 39 | [104](#case-104) | 19,977,229 / 19,977,229 | travel_distance_type | 酒店距离类型：离散分档被数值边界放大 |
| 40 | [105](#case-105) | 2 / 2 | psi | PBOC 模型 PSI：分桶比例、LN、SUM、STRING |
| 41 | [115](#case-115) | 455 / 456 | 全部 20 列 | 客服转接：不完全窗口排序改变过滤结果 |
| 42 | [119](#case-119) | 15,562,819 / 15,562,794 | bu_name, datekey, device_id, travel_distance_type | 意向旅行类型：分类变化导致去重组合增加 |
| 43 | [120](#case-120) | 4,513,404 / 4,513,404 | second_cat4_avg_weight | 二级业态均价：真实 AVG 列被按位置写入 |
| 44 | [121](#case-121) | 620,217 / 620,217 | recall_json | 类目召回 JSON：LOG10 权重差异，而非单纯排序 |
| 45 | [122](#case-122) | 706 / 706 | risk_params_json | 风险参数 JSON：联系电话 token 已不同 |
| 46 | [124](#case-124) | 396,846 / 396,846 | in_3year, out_3year | 长账龄余额：LAG、累计窗口与跨年桶分配 |
| 47 | [130](#case-130) | 3 / 3 | psi | CreditPay 模型 PSI：历史分桶到文本的同类链路 |
| 48 | [135](#case-135) | 196,407 / 196,407 | reviewdis | 评论距离：三角误差经三位小数舍入再写 FLOAT |
| 49 | [138](#case-138) | 3,383,591 / 3,383,591 | avg_delivery_distance | 配送距离字符串：加权平均的两位舍入差异 |
| 50 | [146](#case-146) | 440 / 440 | reviewdis | 风险团购评论：距离微差及独立的选单风险 |
| 51 | [147](#case-147) | 1 / 1 | psi | Apollo 模型 PSI：小尾差及告警门槛 |
| 52 | [148](#case-148) | 68,310,558 / 68,310,558 | avg_duration_hash | 配送时长：基础 AVG 不同，关联辅助均值相同 |
| 53 | [154](#case-154) | 769,319 / 769,319 | order_amt | 搜索订单金额：GROUPING SETS 下的 SUM |
| 54 | [155](#case-155) | 1,521,872 / 1,521,872 | increase_rate | 热词增长率：比率、聚合与混合数值类型 |
| 55 | [159](#case-159) | 42,903,036 / 42,903,036 | number | 外部 Python 得分：SQL 路径差异不等于业务 SQL 不同 |
| 56 | [166](#case-166) | 2,620,278 / 2,620,277 | 全部 11 列 | 商品价格变更：相同时间戳下 LAG 改变去重 |
| 57 | [169](#case-169) | 1,982,922 / 1,982,922 | cpc_arpu_level | 广告 ARPU 等级：0.5 边界的离散翻转 |
| 58 | [172](#case-172) | 3,438,737 / 3,438,737 | recall_json | 意向召回 JSON：LOG10 尾差与多层排名 |
| 59 | [175](#case-175) | 34,956,933 / 34,956,933 | score_rank_no | 商品分数排名：先确认 RANK 改写与 checksum 算法 |
| 60 | [180](#case-180) | 166 / 166 | roi_cnt | 广告转化量：DOUBLE SUM 与派生 CVR |
| 61 | [199](#case-199) | 137,406,282 / 137,406,282 | loc_uuid_count | 百米内去重人数：边界筛选优先于 COUNT DISTINCT |
| 62 | [201](#case-201) | 35,279 / 35,279 | avg_jvm_heap_used_ratio | JVM 内存比例：除法、AVG、ROUND、STRING 四层 |
| 63 | [202](#case-202) | 793,697 / 793,697 | page_stay_time | 页面停留时间：AVG 跨整数边界后截断 |
| 64 | [204](#case-204) | 7,981 / 7,981 | risk_params_json | 关联客户风险 JSON：坐标行不同，先核对匹配与快照 |
| 65 | [206](#case-206) | 13,791,516 / 13,791,516 | extend_info, rel_product_ids | 商品相似推荐：PMI 排名边界改变候选集合 |

## 4. 共用验证流程

### V0：固定输入和实际执行路径

1. 固定所有读取分区和非分区维表为不可变快照，保存 schema、文件清单、文件校验及 UDF/脚本哈希。涉及中间 drop/create 的 SQL 为两边分配不同 staging 名称，并串行测试，防止相互覆盖。
2. 首先原样复现 current conf，不立即删除内部确定化规则。在同一冻结输入上让 Velox x86、Velox ARM 各至少重复 3 次，优先补充目前只有一次的 ARM 观测；保存执行计划、聚合/窗口实际执行路径、数据读取路径、有效 session 配置、shuffle/AQE、ANSI、时区和字符串/DECIMAL 类型转换。
3. 主对照始终为 Velox x86 与 Velox ARM：固定源码、输入、有效配置和逻辑计划，记录并尽量对齐编译器版本/选项、依赖库及执行并发。先用固定分区数隔离数值合并顺序，再恢复生产 AQE/并发；对标量函数使用相同输入位模式跨架构重放。一次只改变一个因素。只有定位仍不清楚时，才可新增同架构 JVM/原生 Spark 回退实验；这不属于现有三次测试，且须由执行计划确认实际回退。
4. `unstable_sql` 只是框架标签；COUNT/SUM、数学函数、输入漂移和窗口选择分别取证，不按标签批量判定。

### V1：正确比较结果与中间结果

先比较 schema、记录数、NULL/NaN/Inf 分布和业务键重复度。唯一键存在时用 NULL-safe 连接对齐；重复键用完整记录多重集，不可只 DISTINCT，也不可按 collect 输出顺序逐行 zip。保留未展示的源行标识，防止 99、122、204 这类误配。

下列 SQL 是**验证模板**，`baseline_stage/new_stage` 指同一逻辑阶段的只读快照，列顺序及类型需显式对齐，模板未在本地运行：

```sql
-- 两个方向都做；EXCEPT ALL 保留重复行次数。
SELECT * FROM baseline_stage
EXCEPT ALL
SELECT * FROM new_stage;

SELECT * FROM new_stage
EXCEPT ALL
SELECT * FROM baseline_stage;

-- k1/k2 替换为实际业务键，先确定是否能一对一配对。
SELECT k1, k2, COUNT(*) AS n
FROM baseline_stage
GROUP BY k1, k2
HAVING COUNT(*) > 1;
```

若任务存在 CUBE/GROUPING SETS，诊断输出保留 `grouping_id()`，不要先 COALESCE 为“整体/全国”后再构造比较键。比较逻辑本身也应在固定路径执行，避免两边使用不同 checksum 实现。

### V2：数值链逐阶段断点

对 SUM/AVG 保存原始贡献行、输入类型、非 NULL 数、sum、avg；对数学函数保存每一项输入和输出；对 ROUND/CAST/JSON 保存转换前值、转换后值和目标 schema。高精度参照必须使用与两边相同的实际输入；DECIMAL 的精度、scale、溢出和除法缩位需明确，它不是无条件可替换的修复。

DOUBLE 使用 17 位有效数字、原始 64 位值和 ULP 记录，FLOAT 使用原始 32 位值。只检查十进制展示不足以区分计算差异与格式化差异。若在 Python 中读取位值，应从保留类型的 Parquet/Arrow/Row 中取得数值，避免先通过低精度文本再重建。

```python
# 仅示意如何记录一个已经正确读入的 DOUBLE；并非完整比较器。
import struct
def describe_double(x):
    return {'decimal17': format(x, '.17g'),
            'bits': struct.pack('>d', x).hex()}
```

以 `abs(a-b) <= atol + rtol*max(abs(a),abs(b))` 记录有限值误差，atol/rtol 由字段含义确定，不预设万能阈值。NULL、NaN、Inf、正负零单列比较。金额到分、计数、标签、候选集合、ROUND/CAST 后离散结果需要各自验收，不能只因中间浮点误差小就判通过。

### V3：距离、NaN 与阈值

将角度换算、三角项、ACOS/ASIN 输入、距离和谓词逐层物化。对 `d=R*acos(u)`，`|dd/du|=R/sqrt(1-u²)`，u 接近 ±1 时误差放大；这说明机制合理，不证明本条输入已经满足条件。

Spark 文档规定 NaN 在比较/排序中大于其他数值，不能假定 `NaN>1000` 必为 false。因此距离 CASE 必须观测 isnan、isnull 和各比较条件；普通 C++ 比较若未做 Spark 语义适配也应检查。参见 [Spark 3.5.6 NaN 语义](https://spark.apache.org/docs/3.5.6/sql-ref-datatypes.html#nan-semantics)。该文档为同系列公开参考，内部 3.5.1 分支以实测为准。

```sql
-- 两边均运行的特殊值语义探针；要求确认表达式实际执行路径。
SELECT x, isnan(x), x IS NULL AS is_null,
       x < 100 AS lt_100, x <= 50 AS le_50, x > 1000 AS gt_1000
FROM VALUES (CAST('NaN' AS DOUBLE)),
            (CAST(NULL AS DOUBLE)),
            (CAST('Infinity' AS DOUBLE)),
            (CAST(100 AS DOUBLE)) AS t(x);
```

同位置/近距离/对跖点、100 米及 50/300/1000 公里两侧都应覆盖。Haversine、高精度计算和对 u 的夹紧仅作定位对照；未经业务定义不能随意将非法坐标、NULL、NaN 转成合法距离。

### V4：窗口、集合和字符串

窗口先物化其完整输入，检查分区键和排序键的重复度。LEAD/LAG/LAST_VALUE 需要可解释的业务顺序；RANK 与 ROW_NUMBER 在并列时语义不同。测试 2–3 行同时间戳不同价格/状态、整组同分、Top K 边界同分等最小样本，并确认内部改写后的函数。

对集合输出分开比较元素成员、重复次数、元素值、顺序和文本格式。普通 Spark 的 collect_list/collect_set 顺序受输入/shuffle 影响，但本批次有内部确定化配置，实际行为需看计划。参见 [Spark 集合聚合说明](https://spark.apache.org/docs/3.5.6/api/sql/index.html#collect_set)。仅在成员和值一致后，才把排序归一作为差异验证步骤。JSON 字符串按 schema 展开；手工键值字符串先按其真实语法解析并保留重复项。

## 5. 32 条详细分析

每条均以 V0/V1 为前置条件。下文“目标类型”来自落表 DDL，不代表整条表达式链均采用该类型；“样本相同”只描述已保存样本，不扩大为全量逐行一致。


<a id="case-94"></a>

### 94 周报 GTV：平均值跨越取整边界

**任务：** `hmart_generalshop.mv_traffic_general_pdp_wbr_info`。Excel 第 34 行；原始记录行号 94。

**运行观测：** size 为 new=110、baseline=110、baseline_again=110；差异列：`pdp_gtv`。原框架 reason：`unstable_sql`。

**证据与表达式。** `pdp_gtv` 目标类型为 DOUBLE，来自 `round(avg(pdp_gtv))`，按派生的 `WEEK` 聚合，源表为 `mart_generalshop.mv_traffic_general_pdp_daily_info`。没有保存具体差异值。其他 57 列 checksum 相同，说明应先聚焦该聚合链，而不是看到同一 SQL 的 `collect_set` 就归因于集合顺序。

**可能原因。** 首选假设是 DOUBLE 的分区内累加、分区间合并顺序不同，使平均值落在 `k+0.5` 两侧，最终 ROUND 放大成整数差异。第二种是假设相同输入 DOUBLE 在两边 ROUND 实现中得到不同结果。还需检查源列实际类型、历史分区更新和写出前隐式转换；目标 DOUBLE 并不证明源列也是 DOUBLE。

**验证方案。**

1. 以 `week` 对齐差异记录，冻结该周全部输入；分别输出 `count(*)`、`count(pdp_gtv)`、`sum(pdp_gtv)`、未舍入 AVG、ROUND 和输入类型。
2. 将两边未舍入均值按原始 DOUBLE 位模式保存，计算距最近半整数的距离。使用同一份物化均值分别执行 ROUND，拆开“AVG 不同”和“ROUND 不同”。
3. 对原始输入按业务精度转 DECIMAL 后重新 SUM/COUNT，并在 1、固定多分区下对照；DECIMAL 只作参照，需先确认量级和精度不溢出。

**判定。** 均值不同且跨半整数时定位到聚合数值稳定性；同一均值仍舍入不同则定位到 ROUND/转换兼容性。未取得差异值前不能确认是哪一种。

<a id="case-96"></a>

### 96 评论距离：近距离 ACOS 敏感性及 FLOAT 写入

**任务：** `hmart_credit.fact_youdao_dp_riskshop_mfc_free_dishes_recog`。Excel 第 35 行；原始记录行号 96。

**运行观测：** size 为 new=1,231、baseline=1,231、baseline_again=1,231；差异列：`reviewdis`。原框架 reason：`unstable_sql`。

**证据与表达式。** `reviewdis` 为 FLOAT，样本 `reviewid=3416068413` 的两值为 `2.637298583984375` 与 `2.640721559524536`，差约 0.003423 米。完整 SQL 使用 `acos(sin(lat1*pi()/180)*sin(lat2*pi()/180)+cos(lat1*pi()/180)*cos(lat2*pi()/180)*cos(lng1*pi()/180-lng2*pi()/180))*6378137`，最终经 `coalesce(g.reviewdis,9999)` 写出。源表为 `mart_credit.dpmid_credit_xmd_review_basic`。

**可能原因。** 球面余弦公式在近距离、ACOS 输入接近 1 时对末位扰动敏感。三角函数、角度换算及加乘中间值差异可放大，再经 DOUBLE→FLOAT 量化。不能把展示出的多位小数简单理解为纯字符串格式差异。源坐标不同仍是必须排除的前置条件。

**验证方案。**

1. 按 reviewid/shopid 及源记录标识固定坐标四元组，逐步导出弧度、SIN/COS、ACOS 输入 `u`、DOUBLE 距离及最终 FLOAT；比较二进制位模式。
2. 统计 `u>1`、`u<-1`、NULL、NaN，并分别计算 `distance<=500` 的结果。SQL 用该条件生成店内评论统计，现有 checksum 未显示这些统计列变化，但需覆盖全量。
3. 将相同输入位模式分别送入 x86/ARM 的 Velox 表达式，固定编译选项并核对数学库；用高精度数学计算及 Haversine 公式作诊断参照。必要时新增 JVM 对照，但现有数据不包含这一对照。

**判定。** 同坐标、三角中间值首先分叉支持数值路径差异；坐标先分叉则查输入。只有 FLOAT 转换处分叉才归因写出转换。不能未经业务确认直接替换距离公式或夹紧所有越界输入。

<a id="case-98"></a>

### 98 召回策略金额：MAX 后 SUM 的累计误差

**任务：** `hmart_ads.ads_recall_flowtype_recall_strategy_onecause`。Excel 第 36 行；原始记录行号 98。

**运行观测：** size 为 new=202、baseline=202、baseline_again=202；差异列：`total_roi_pay_cnt`。原框架 reason：`空`。

**证据与表达式。** 唯一不一致列为 DOUBLE `total_roi_pay_cnt`。SQL 先在 `mart_ads.ads_match_keyword_recall` 中按请求/广告等维度计算 `max(a.roi_amt)`，再按 `slot_id,match_rule,platform_req_category` 计算 `SUM(roi_amt)`。没有具体差异值；`roi=total_roi_pay_cnt*1.0/total_cost` 的 checksum 恰好相同，不能据此排除金额末位差异。

**可能原因。** 首选是浮点 SUM 合并顺序不同。备选是 MAX 阶段遇到 NaN、NULL、正负零或输入不同，以及召回规则解析/关联导致贡献行变化。原始 SQL 包含带反斜杠的 split 模式，JSON 已损坏，复现必须使用原始 CSV 的 SQL。

**验证方案。**

1. 以三个聚合键找出差异组，对 MAX 层的请求等完整分组键做双向 `EXCEPT ALL`，确认贡献行及其 `roi_amt` 一致。
2. 在相同 MAX 层快照上单独执行 SUM，记录非 NULL 数、NaN 数、sum、目标类型，以及高精度/DECIMAL 参照。
3. 在 x86/ARM Velox 上分别固定及改变合并分区数，先区分架构与合并顺序因素；若 MAX 层已不同，再逐步检查源行、规则 split、base_data 关联及 MAX 的特殊值行为。

**判定。** MAX 输出完全相同而 SUM 尾数随合并方式变化，支持聚合误差；贡献行不同则不能用浮点容差结案。

<a id="case-99"></a>

### 99 支付旅行类型：距离分档、去重与样本配对

**任务：** `ba_travel.app_log_pay_geo_type`。Excel 第 37 行；原始记录行号 99。

**运行观测：** size 为 new=3,333,232、baseline=3,333,232、baseline_again=3,333,232；差异列：`geo_type, order_id, travel_distance_type, user_id`。原框架 reason：`空`。

**证据与表达式。** 总量相同，但 `travel_distance_type,geo_type,user_id,order_id` 均有 checksum 差异。SQL 合并门票、度假、酒店、民宿、机票、火车票分支；多个分支先计算距离，再对最终类别和用户/订单分组去重。advance_check 的机票样本仅以 `bu_name=机票,datekey=20260919` 列出相同字段，用户和订单却不同，不能当成同一订单的类别变化证据。

**可能原因。** 距离跨 50/300/1000 公里边界，或者 ACOS 越界产生 NaN 后 CASE 分类不同，均可能改变去重后的类别组合。同一个订单若关联多条站点/常住地记录，类别合并或拆分会改变用户/订单出现次数，即使最终总行数抵消为相同。另一分支酒店直接读取上游 `travel_distance`，不能统一归因本 SQL 三角函数。

**验证方案。**

1. 先按 bu_name 分别统计行数和完整记录多重集；以 `(bu_name,datekey,order_id,user_id)` 检查重复度，对每个键比较类别集合及次数，避免按输出位置配对。
2. 对机票分支保留起讫站编码、business_line、两站坐标与源行标识，检查 `dim_traffic_station` 的关联唯一性；再比较距离原值、isnan 和 CASE 结果。
3. 在两级 GROUP BY 前后分别比对，确认是源行不同、距离类别不同，还是去重导致某些订单出现次数变化；酒店分支单独核对上游距离快照。

**判定。** 只有同一源记录、相同坐标下类别首先变化，才支持数值/NaN 机制。仅调整排序不能解释经核实的逐列无序 checksum 差异。

<a id="case-103"></a>

### 103 商品索引：EXP 得分尾数被嵌入三个字段

**任务：** `ba_dealrank.real_new_item_index_skushangou`。Excel 第 38 行；原始记录行号 103。

**运行观测：** size 为 new=343,627、baseline=343,627、baseline_again=343,627；差异列：`extension_info, index_list, item_personattr`。原框架 reason：`unstable_sql`。

**证据与表达式。** `itemid=17791473177,itemtype=21,dt=20260919,ds=2026091900` 的三列共同包含 `score@0.06519170074968811` 与 `score@0.0651917007496881`。`index_list` 样本的元素顺序相同。得分来自 `meituan_pay_pv / (('1789747200'-start_time)/86400.0) * exp(-0.2*(('1789747200'-start_time)/86400.0))`，随后 CAST STRING、拼接、集合聚合及 JSON 序列化。

**可能原因。** 优先查 EXP 或其输入/乘除链的末位差异，再查同一 DOUBLE 的字符串格式化差异。集合顺序是潜在问题，但不能解释当前三个字段一致出现的数值尾数变化。`collect_set(...)[0]` 还存在多值任选风险，应与得分差异分开验证。

**验证方案。**

1. 固定该商品的 `start_time,meituan_pay_pv`，逐项物化年龄天数、EXP 参数、EXP 输出、未字符串化得分；记录类型和位模式。
2. 同一物化 DOUBLE 分别 CAST STRING；若位模式一致而文本不同，定位格式化，反之继续前推到第一个数值分叉点。
3. 解析两边 JSON/数组，对城市/索引键归一排序后逐项比较数值；另检查商品标题、分类等 `collect_set(...)[0]` 是否确有多个候选值。

**判定。** 当前证据支持得分计算/序列化链，不能按“集合无序”直接豁免。若产品要求文本完全一致，需要明确得分输出精度和格式，而不是只排序。

<a id="case-104"></a>

### 104 酒店距离类型：离散分档被数值边界放大

**任务：** `app_hotel.ads_ord_service_experience_analysis_di`。Excel 第 39 行；原始记录行号 104。

**运行观测：** size 为 new=19,977,229、baseline=19,977,229、baseline_again=19,977,229；差异列：`travel_distance_type`。原框架 reason：`unstable_sql`。

**证据与表达式。** 90 列中只有 INT `travel_distance_type` 不同，无具体样本。该列以 `6371004*acos(u)/1000.0` 和 50/300/1000 公里分档，返回 1/2/3/4，否则 -999，坐标来自 tag 的常住地与 dim_poi 的酒店位置。

**可能原因。** 有限距离在阈值两侧变化，或同位置点 `u≈1` 因末位差异变成域外值、NaN，从而分到不同类型。Spark 的 NaN 比较语义不能直接按 C++ 普通比较理解；`distance>1000` 可能接住 NaN，须实测当前构建。另查关联坐标快照变化。

**验证方案。**

1. 按订单及关联记录键找出差异行，先验证唯一性与坐标相同，再导出 u、distance、isnan、四个比较布尔值和最终 CASE。
2. 将差异划分为“有限值跨 50/300/1000”“有限值/NaN”“NULL/非 NULL”“输入不同”。分别统计，不能混成一类精度差异。
3. 用同位置、近邻及阈值附近坐标在实际两边重复测试；物化距离后只重放 CASE，区分计算差异和比较语义差异。

**判定。** 这是类别输出差异，不应以 DOUBLE 容差直接接受；需要验证边界业务规则及 NaN 处理。

<a id="case-105"></a>

### 105 PBOC 模型 PSI：分桶比例、LN、SUM、STRING

**任务：** `hmart_finrc_model.dm_model_prod_scorecard_pboc_10_0_xgb_d_psi`。Excel 第 40 行；原始记录行号 105。

**运行观测：** size 为 new=2、baseline=2、baseline_again=2；差异列：`psi`。原框架 reason：`unstable_sql`。

**证据与表达式。** 最终只有两行，差异列 `psi` 的目标类型为 STRING。完整 SQL 在中间表计算 `sum(case when label_rate!=0 and label_rate is not null then (bin_col_rate-label_rate)*ln(bin_col_rate/label_rate) else 0 end)`，再透传写出；最终查询的 `a.psi` 不是原始来源。没有保存数值样本，不能确定差异量级。

**可能原因。** 分桶比例除法、LN 实现及 SUM 合并顺序产生微差，并被 STRING 完整保留；也可能源桶计数/历史基准发生变化。CASE 只保护 label_rate，需单独检查 `bin_col_rate=0` 时的 LN/NULL 行为。两边使用同名持久中间表，复现还要排除互相覆盖。

**验证方案。**

1. 对两行分别按 `dqc_column_name,partition_date` 比较；拆出每个 bin 的当前计数、总数、bin_col_rate，以及基准日 `2026-02-20` 的 label_rate。
2. 依次比较比值、LN、单桶贡献、贡献的 NULL 标志、最终 SUM 与 STRING；固定相同桶输入分别重放 LN 和 SUM。
3. 用每次运行独立的 staging 表重跑。记录 STRING 对应数值的绝对/相对误差及格式差异，而非只比 checksum。

**判定。** 桶计数一致后才可研究浮点/LN；只有格式不同则属序列化差异。当前没有证据支持直接标记为可接受。

<a id="case-115"></a>

### 115 客服转接：不完全窗口排序改变过滤结果

**任务：** `hmart_epai_base.csp_fake_performance_incorrect_transfer`。Excel 第 41 行；原始记录行号 115。

**运行观测：** size 为 new=455、baseline=456、baseline_again=456；差异列：`call_cnt, case_id, contact_type, department_name, dt, is_problem_transfer, is_trasfered, last_subbiz_name, lifecycle, mis_name, name, next_subbiz_name, ori_contact_id, ori_contact_id_w, oriani, partition_date, ring_start_time, staff_id, subbiz_name, trasfered_contact`。原框架 reason：`size_diff|unstable_sql`。

**证据与表达式。** new 比 baseline 少 1 条。完整 SQL 多处使用仅按 `ring_start_time` 或 `start_time` 排序的 LEAD、全窗口 LAST_VALUE；随后计算 `is_problem_transfer,is_next_staff_handle,is_last_case_upgrade`，最后以这些标志过滤。日期均是固定字面量派生，不是动态“当前时间”。

**可能原因。** 同一通话/会话内相同时间戳的并列记录次序不同，改变下一处理人、最后子业务/升级标志，从而删除或保留一条结果。工单关联和员工维表一对多可能制造这些并列行。源表更新仍是备选，但不能只因存在日期过滤就认定数据漂移。

**验证方案。**

1. 对电话、在线分支分别做 `EXCEPT ALL`，定位缺失记录所属 `ori_contact_id_w`，拉取该完整分区而非只拉最终缺失行。
2. 在每层窗口前检查 `(分区键,排序时间)` 重复数；保留 contact/service/case/staff 等源唯一标识，逐行列出 LEAD、LAST_VALUE 和最终谓词。
3. 在相同窗口输入上，按业务确认的稳定事件序列补齐排序键做诊断对照；同时核对工单关联及员工表重复键。

**判定。** 同一输入只因并列记录顺序改变而改变谓词，支持 SQL 排序不充分；全序窗口仍分叉才进一步检查窗口算子。不要把补充任意 ID 当成已确认业务修复。

<a id="case-119"></a>

### 119 意向旅行类型：分类变化导致去重组合增加

**任务：** `hmart_jiulv_flow.app_log_intention_geo_type`。Excel 第 42 行；原始记录行号 119。

**运行观测：** size 为 new=15,562,819、baseline=15,562,794、baseline_again=15,562,794；差异列：`bu_name, datekey, device_id, travel_distance_type`。原框架 reason：`size_diff|unstable_sql`。

**证据与表达式。** new 多 25 条，四个输出列全部 checksum 不同。SQL 多分支 UNION ALL，每个分支按 `(bu_name,travel_distance_type,device_id,datekey)` 去重；距离取决于常住地/商品或起讫站坐标。没有差异行样本。

**可能原因。** 同一设备多次访问的类别可能因距离阈值或 NaN 变化发生拆分/合并，改变最终去重组合数量，25 是净增加量，不等于恰好 25 个源事件出错。另需排除常住地/城市/站点维表变化以及关联重复度变化。UNION ALL 本身并不随机丢行。

**验证方案。**

1. 给门票、度假、酒店、民宿、交通分支加诊断来源列，统计每个分支在关联后、距离后、最终去重后的条数及差集。
2. 对差异设备保留原始事件、坐标、u、distance、isnan、类别，按设备比较类别集合；查 50/300/1000 公里附近及 ACOS 定义域边缘。
3. 先比较未去重的源事件多重集；再物化完全相同的类别输入仅重放 GROUP BY。由此区分输入、表达式和分组算子问题。

**判定。** 若源事件相同而类别拆分/合并恰好解释差集，定位到距离分类；类别输入也相同而分组结果不同，则检查分组/类型语义，不能仅靠总数差推断。

<a id="case-120"></a>

### 120 二级业态均价：真实 AVG 列被按位置写入

**任务：** `hmart_caterb2b_srs.second_cat4_avg_minmax_order_ylc`。Excel 第 43 行；原始记录行号 120。

**运行观测：** size 为 new=4,513,404、baseline=4,513,404、baseline_again=4,513,404；差异列：`second_cat4_avg_weight`。原框架 reason：`acceptable_diff|unstable_sql`。

**证据与表达式。** 样本键 `(second_cat_id=1702,sales_grid_id=20,cat4_id=10053155,dt=20260920)`，两值 `0.7597682988346492` 与 `0.7597682988346491`。目标 DOUBLE `second_cat4_avg_weight` 实际对应 SELECT 中 `avg(weight_minmax) AS cat4_avg_weight`，INSERT 按位置写入；表格的“未定位到表达式”不成立。时间衰减加权列另有其列，本例未显示变化。

**可能原因。** AVG 的浮点累加/合并顺序是首选，样本符合末位误差；但仍需确认 base 的 GROUP BY 去重和关联输入一致。framework 的 `is_float_value_equal` 只能表明样本命中过该规则，不能代替全量误差统计。

**验证方案。**

1. 物化完整 base，固定该三维组；比较 `weight_minmax` 的多重集、非 NULL 个数、SUM 和 AVG。
2. 使用相同物化数据对比单分区/多分区聚合及 DECIMAL 参照，确认误差来源和上界。
3. 按目标列顺序核对所有七个非分区输出，避免将未加权 AVG 与 dt_dif 加权均值错配；验证 NULL 输入不计入 AVG 分母。

**判定。** 输入一致且全量差异均在业务容差内，可按数值精度差异处理；不要修改时间衰减公式来修复本列。

<a id="case-121"></a>

### 121 类目召回 JSON：LOG10 权重差异，而非单纯排序

**任务：** `hmart_travel_top_data_gather.tjtg_all_cate3_geohash_recall`。Excel 第 44 行；原始记录行号 121。

**运行观测：** size 为 new=620,217、baseline=620,217、baseline_again=620,217；差异列：`recall_json`。原框架 reason：`unstable_sql`。

**证据与表达式。** `recall_key=tjtg_all_recall_cate3_geohash5_117_wm3yw` 中商品 `1466558989` 的权重为 `0.5331930548010113` 与 `0.5331930548010112`，样本键序相同。权重为 `(uv_ctr*0.2+uv_cxr*0.8)*LOG10(order_cnt+1)*100`；随后按 weight 降序取 `rn<=30`，再拼接字符串。

**可能原因。** LOG10/乘加结果差异或 CAST STRING 差异直接进入文本；权重微差还可能改变 Top 30 边界。并列排序和集合顺序是需要独立验证的次要机制，且实际运行可能把 ROW_NUMBER 改写为 RANK。

**验证方案。**

1. 固定该召回键对应的全部候选及质量表分区，比较三个输入指标、LOG10 输出、未格式化 weight 及其位模式。
2. 使用相同 weight 仅测试 CAST STRING；解析手工 `{deal_id:weight}` 时保留重复键/次数，不能直接转 map 丢失重复记录。它的键未加引号，不是严格标准 JSON。
3. 比较排序前后候选集合，检查第 30 位附近的同分/近似同分记录及实际 rank 改写，输出选入标志。

**判定。** 当前样本已排除“只有顺序不同”的解释；若仅值微差，可讨论权重精度约定；若候选集合变化则需按排序结果差异验证。

<a id="case-122"></a>

### 122 风险参数 JSON：联系电话 token 已不同

**任务：** `hmart_caterb2b_mall_risk_management.topic_risk_customer_fake_new_customer_base_day.900105`。Excel 第 45 行；原始记录行号 122。

**运行观测：** size 为 new=706、baseline=706、baseline_again=706；差异列：`risk_params_json`。原框架 reason：`unstable_sql`。

**证据与表达式。** `customer_id=11949234,dt=20260920,etl_id=900105` 的样本变化位于 `extendField.contactTelToken`，两边 token 内容不同；其他样本字段包括 poiDistance 未显示变化。该字段直接来自 `mart_caterb2b.dim_poi_contact_day.contact_tel_token`，关联条件含 is_kp/istatus/contact_source/is_valid 和固定 dt。为避免无关标识扩散，本文不重复 token 原文。

**可能原因。** 首选排查源数据版本或源表/视图中的 token 生成路径；其次检查同一 customer_id 多条合格联系人导致比较器配错行。SQL 最后是 DISTINCT，若两边都含两种 token，完整行集合可能相同，仅样本配对不可靠；若源表未变化而最终多重集确不同，再排查关联/DISTINCT。SQL 含距离 UDF 不能解释电话 token 变化。

**验证方案。**

1. 对该客户导出所有满足过滤条件的联系人，保留源主键、分区文件版本及 token；核对 new/baseline 是否读到同一份数据，必要时查看视图定义或脱敏函数。
2. 对整个客户的结果 JSON 按结构解析，比较完整记录多重集，不用 customer_id 单键强行一一配对；单独比较 contactTelToken 的集合和次数。
3. 固定联系人快照后在 x86/ARM Velox 上逐层运行关联、投影、DISTINCT；定位最早出现差异的层，再用相同物化输入单独重放该算子，必要时新增回退对照。

**判定。** 现有样本不支持三角函数精度归因；只有排除源版本和配对问题后，才能认定关联/去重错误。

<a id="case-124"></a>

### 124 长账龄余额：LAG、累计窗口与跨年桶分配

**任务：** `hmart_catering.dm_aggr_fin_accounting_precharge_ad_long_account_age_ss`。Excel 第 46 行；原始记录行号 124。

**运行观测：** size 为 new=396,846、baseline=396,846、baseline_again=396,846；差异列：`in_3year, out_3year`。原框架 reason：`unstable_sql`。

**证据与表达式。** 仅 `in_3year,out_3year` 两列不同，目标均为 DECIMAL(20,2)，无具体样本。链路为：按客户/产品/业务累计正负 amount 得到 `current_store_nums`，按 id 做 LAG，计算 `lag_diff` 和 CASE `age_qty`，再按 `days=721..1080` 与 `days>1080` 分桶 SUM。`coalesce(lag(current_store_nums), 'top')` 混用了数值和字符串。

**可能原因。** id 在联合来源或关联后不唯一，可能使 LAG 选取的前驱不同，进而改变两个相邻年桶的分配。累计 SUM 有 ORDER BY 但未显式 ROWS，需核对实际 RANGE 窗口与 peers 处理。其次是字符串/数值比较的隐式转换、累加数值精度或日期 UDF 边界。仅凭目标 DECIMAL 不能排除上游 DOUBLE，但也不能直接归因浮点 SUM。

**验证方案。**

1. 找到差异客户/业务/产品，检查两列差值是否相反、总余额及所有龄段合计是否相同；这种模式支持分配移动，但不是最终证明。
2. 导出该完整流水分区：来源、唯一流水键、id、deliver_date、days、amount、current_store_nums、LAG、lag_diff、age_qty 和 age。检查重复 id，尤其横跨 1080/1081 天的 peers。
3. 查看 analyzed plan 中累计窗口 frame，以及 `'top'` 的类型强制转换。诊断时用独立 first-row 标志替代字符串哨兵，再分别验证；不要同时改窗口和 CASE。
4. 若原始 amount 是精确 DECIMAL、无溢出且全序输入一致，比较每层精确值；若中途转 DOUBLE，才补做数值稳定性实验。

**判定。** 账龄金额需按分精确验收。补排序键、切换 RANGE/ROWS 会改变财务分配语义，只能作为受控定位实验，修复需遵循业务规则。

<a id="case-130"></a>

### 130 CreditPay 模型 PSI：历史分桶到文本的同类链路

**任务：** `hmart_finrc_model.dm_model_prod_scorecard_creditpay_offline_athena_b_8_0_prob_d_psi`。Excel 第 47 行；原始记录行号 130。

**运行观测：** size 为 new=3、baseline=3、baseline_again=3；差异列：`psi`。原框架 reason：`unstable_sql`。

**证据与表达式。** 三行输出只有 STRING `psi` 不同，无数值样本。SQL 将当日桶比例与基准日 `2026-03-21` 的 label_rate 关联，再计算 `(bin_col_rate-label_rate)*ln(bin_col_rate/label_rate)` 并 SUM。中间表前缀为 `xtmp_2377348__`，最终写入 `partition_date=2026-09-21`。

**可能原因。** 比例除法、LN、SUM 或字符串转换的末位差异；备选是历史基准表/当前桶分布或共用 staging 变化。与 105 机制相似，但不能用 105 的结论代替此任务实际三个模型列的检查。

**验证方案。**

1. 按三个 dqc_column_name 分别比较每个 bin 的计数、总数、边界与 label_rate，固定当前和历史快照。
2. 逐桶导出比值、LN、贡献和 NULL 标志；先比较单桶数值，再比较 SUM 和目标 STRING，定位第一个差异。
3. 使用运行独立的 staging，在相同桶输入上分别隔离 LN 与聚合，对比 x86/ARM Velox；检查零比例、缺失基准和极小比例的处理。

**判定。** 桶输入一致、数值只差末位才支持数学/聚合路径；出现缺桶或计数变化先追查分桶和数据来源。

<a id="case-135"></a>

### 135 评论距离：三角误差经三位小数舍入再写 FLOAT

**任务：** `hmart_credit.fact_dp_youdao_daocan_inshop_before_exp_image_review_recog_add`。Excel 第 48 行；原始记录行号 135。

**运行观测：** size 为 new=196,407、baseline=196,407、baseline_again=196,407；差异列：`reviewdis`。原框架 reason：`unstable_sql`。

**证据与表达式。** `reviewid=3411038841,shopid=1335463985` 的两值为 `11.062000274658203` 与 `11.060999870300293`。basic_info 使用与 96 相同的球面余弦公式，main 明确执行 `round(b.reviewdis,3)`，最终列为 FLOAT；因此显示出的长尾包含 FLOAT 存储效果。

**可能原因。** ACOS 近 1 放大的数值差异可能使原距离跨越 0.001 米的舍入边界；也可能同一未舍入距离在 ROUND 实现上分叉。两值不是纯粹同一个三位小数值的不同文本表示。

**验证方案。**

1. 从 `mart_credit.dpmid_credit_xmd_review_basic` 固定该 reviewid 的坐标与源记录；注意 `hp_statdate='3000-12-31'` 是实际查询条件，冻结这一快照。
2. 分别输出 u、DOUBLE 距离、`round(distance,3)`、最终 CAST FLOAT 的位模式，定位差异在哪一步变成 11.061/11.062。
3. 用同一物化距离重放 ROUND，并统计全量距离差和相关条件是否翻转；与 96 共用函数级测试但独立保留本条样本。

**判定。** 数学阶段先不同则查数值路径；ROUND 输入位模式相同但输出不同则查舍入；只在 FLOAT 写入阶段不同再查转换。

<a id="case-138"></a>

### 138 配送距离字符串：加权平均的两位舍入差异

**任务：** `hmart_peisongpa.dynamic_scope_poi_waybill_block_geohash_concat`。Excel 第 49 行；原始记录行号 138。

**运行观测：** size 为 new=3,383,591、baseline=3,383,591、baseline_again=3,383,591；差异列：`avg_delivery_distance`。原框架 reason：`acceptable_diff|unstable_sql`。

**证据与表达式。** `poi_id=9422217,dt=20260921` 中 geohash `ws7emq5` 的距离为 `1122.38` 与 `1122.37`，其余样本排列基本相同。该值为 `round(sum(peisong_total_num*avg_delivery_distance)*1.0/sum(peisong_total_num),2)` 后拼接。虽然 framework 写 `is_unorder_equal`，样本存在明确数值差异。

**可能原因。** 加权分子累加/乘法差异使商跨越 1122.375 附近，或者 ROUND 对同一个 DOUBLE 处理不同。集合顺序并不能消除 0.01 的变化。源平均距离若已不同也会传播到此处。

**验证方案。**

1. 固定 `(platform_poi_id,geohash,dt)` 的源记录，比较权重、源均距、乘积、分子 SUM、权重 SUM、未舍入商、ROUND。
2. 将同一商的位模式送到两边单独 ROUND，结合高精度乘加参照区分聚合与舍入问题。
3. 全量按 geohash 解析为保留重复项的键值记录，先比键集合与次数，再比数值；重新审查 `is_unorder_equal` 的算法和容差，而非直接沿用“acceptable_diff”。

**判定。** 本样本是实数值变化；是否接受需依据配送距离业务精度，而不是无序集合规则。

<a id="case-146"></a>

### 146 风险团购评论：距离微差及独立的选单风险

**任务：** `hmart_credit.fact_dp_youdao_risk_deal_review_recog`。Excel 第 50 行；原始记录行号 146。

**运行观测：** size 为 new=440、baseline=440、baseline_again=440；差异列：`reviewdis`。原框架 reason：`空`。

**证据与表达式。** `reviewid=3427853850,shopid=62999959,group_id=1077667788` 的距离为 `22.75925064086914` 与 `22.759052276611328`，目标 FLOAT。basic_info 从评论坐标直接计算球面余弦距离，最终 `a.reviewdis` 仅是透传。hit 中还有 `row_number() over(partition by a.reviewid)`，原 SQL 没有 ORDER BY，却记录了运行成功，说明必须核对内部改写后的计划。

**可能原因。** 本次只有 reviewdis 差异，优先查三角函数/运算精度及 FLOAT 转换。无序选团购记录是独立潜在风险，但样本 group_id 和其他列一致，不宜将本次距离差异归到选单。

**验证方案。**

1. 固定评论源快照 `hp_statdate='3000-12-31'` 及相关日期，按 reviewid 导出坐标、u、DOUBLE 距离、FLOAT 距离；逐步比较。
2. 单独检查 hit 的候选团购记录和实际窗口计划，确认无 ORDER BY 的 ROW_NUMBER 是如何被接收/改写的，以及 rnk=1 是否真只选一条。
3. 用固定 hit 结果重放距离链；用固定距离重放 hit。分别验证两条路径，避免同时改变而失去定位能力。

**判定。** 同坐标数学阶段分叉支持距离精度；若选单字段也发生变化，再另立窗口选取问题，不混同处理。

<a id="case-147"></a>

### 147 Apollo 模型 PSI：小尾差及告警门槛

**任务：** `hmart_finrc_model.dm_risk_model_dqc__apollo_v4_0_9_ss`。Excel 第 51 行；原始记录行号 147。

**运行观测：** size 为 new=1、baseline=1、baseline_again=1；差异列：`psi`。原框架 reason：`acceptable_diff|unstable_sql`。

**证据与表达式。** 唯一输出行为 `dqc_column_name=score,partition_date=2026-09-22`，psi 为 `0.10809115965745779` 与 `0.10809115965745777`，目标 STRING；`avg_psi=0.0974565252371846,std_psi=0.06295118799535004,is_psi_dqc=0` 相同。公式以 `base_bin_rate` 为基准计算逐桶 LN 贡献再 SUM，之后比较 `avg_psi±4*std_psi` 和 `psi>0.001`。

**可能原因。** 样本支持比例/LN/浮点合并引发末位差异，文本输出保留全部尾数。该样本未跨告警阈值，但不能推出未来或其他模型也不会跨阈值。

**验证方案。**

1. 物化当日及 base_bin_rate 的逐桶数据，比较计数、比例、LN、贡献及 SUM，注意 staging 表独立命名。
2. 对同一桶贡献比较不同合并树；对相同比值只比较 LN；再检查 STRING 输出是否引入额外格式差异。
3. 在告警比较前记录 psi 的数值类型、上下阈值及距离；补充恰好等于/贴近上下限和 0.001 的边界样本。

**判定。** 本样本可作为数值尾差候选，但验收同时要求告警标志一致；不能只按文本 checksum 判业务失败，也不能忽略阈值翻转。

<a id="case-148"></a>

### 148 配送时长：基础 AVG 不同，关联辅助均值相同

**任务：** `hmart_mallpa.eta_segment_delivery_time_hash`。Excel 第 52 行；原始记录行号 148。

**运行观测：** size 为 new=68,310,558、baseline=68,310,558、baseline_again=68,310,558；差异列：`avg_duration_hash`。原框架 reason：`acceptable_diff`。

**证据与表达式。** `(last_hash_6=webnqx,loc_hash_6=webnqx,leave_hour=20,weekend_ornot=0,dt=20260922)` 的 `avg_duration_hash` 为 `452.4541666666667` 与 `452.4541666666666`。来自 `mart_mallpa.infer_e2e_aoi_segtime_stats` 的 `avg(avg_duration)`，目标 DOUBLE。另三列分别来自 v2 表的 15/7/1 天均值，样本相同。

**可能原因。** 首选基础 AVG 的浮点累加/合并差异。CASE 将 weekend 字符串映射为 0/1，而 GROUP BY 引用 weekend_ornot，需查解析计划的分组表达式；不同原始标签可能映射为同一输出键，不能先假定结果键唯一。

**验证方案。**

1. 固定基础源表分区，导出该组所有 avg_duration 与原始 weekend 标签、非 NULL 数、SUM、AVG；先查实际分组键及输出键重复度。
2. 使用相同输入进行单分区/多分区 AVG 对照和精确数值参照。
3. 将 a 子查询物化后再关联 seg1/2/3，核对关联倍数；若 a 已不同则不必先怀疑三个辅助均值分支。

**判定。** 贡献值多重集与分母一致且只差末位时支持 AVG 数值稳定性；关联或分组键变化需要单独处理。

<a id="case-154"></a>

### 154 搜索订单金额：GROUPING SETS 下的 SUM

**任务：** `hmart_ssai.app_query_strong_intent_metrics`。Excel 第 53 行；原始记录行号 154。

**运行观测：** size 为 new=769,319、baseline=769,319、baseline_again=769,319；差异列：`order_amt`。原框架 reason：`acceptable_diff|unstable_sql`。

**证据与表达式。** 样本 `(search_keyword=999,intent_bu_name=医药,page_city_id=10,partition_date=2026-09-22)` 的金额为 `2103.2299999999996` 与 `2103.229999999999`。真实来源是 query_tbl 内的 `sum(p2.order_amt)`，再经 `COALESCE(order_amt,0)`；目标 DOUBLE。订单量/人数及 pay_order_amt 的样本一致。

**可能原因。** SUM 浮点累加误差优先，COALESCE 本身不是首要嫌疑。GROUPING SETS、关键词归一化及关联重复会影响贡献行，需排除；输出维度中的“全国”/哨兵值可能与原始业务值混淆。

**验证方案。**

1. 保留 `grouping_id`，在原 GROUPING SETS 层定位城市级样本；比较归一化后的搜索词关联行及金额多重集。
2. 对同一组记录非 NULL 数、NULL 数、SUM 和 DECIMAL 金额参照，再单独重放 COALESCE；核对目标 DOUBLE 写入。
3. 检查最终 supply-demand 关联是否一对多，并将精确金额到“分”的验收与原 DOUBLE 位级一致性分开记录。

**判定。** 当前样本支持极小浮点差异；若金额业务要求到分一致，应按源金额精度建立精确对照，不应泛化为所有金额差异都可接受。

<a id="case-155"></a>

### 155 热词增长率：比率、聚合与混合数值类型

**任务：** `hmart_ai_vproject.tuan_hot_word_for_ai_sug`。Excel 第 54 行；原始记录行号 155。

**运行观测：** size 为 new=1,521,872、baseline=1,521,872、baseline_again=1,521,872；差异列：`increase_rate`。原框架 reason：`acceptable_diff`。

**证据与表达式。** `search_keyword=+86电竞,dt=20260922` 的 increase_rate 为 `-2.173684210526316` 与 `-2.1736842105263157`。SQL 先计算 dod、wow 两个比率，再按词聚合 `1.0*(sum(dod)*0.8)+(sum(wow)*(1-0.8))`；目标 DOUBLE。

**可能原因。** 比率除法、SUM 合并和最后乘加的中间舍入。需要注意 SQL 字面量 0.8、0.2 可能先作为 DECIMAL，再与 DOUBLE 强制转换，不能未经 analyzed plan 就断言 `1-0.8` 按二进制 DOUBLE 常量折叠。FMA/编译器优化是数值链候选，现有文件没有编译选项证据。

**验证方案。**

1. 固定当日、前一日、前七日的 search_qv 及关联键，检查连接倍数，导出每行 dod/wow、两项 SUM、加权项及最终值。
2. 查看每个表达式实际类型和常量折叠结果；物化相同 SUM 值只重放最后算术，再以相同原始行只测试聚合。
3. 检查 `a3/a4.search_qv` 为 0/NULL 时分母替换为 1 的分支；统计最终值和排序关系是否仅尾差、是否影响消费端 Top N。

**判定。** 相同贡献输入下末位波动可按容差评估；若分母分支或关联行不同，应转查语义/数据来源。

<a id="case-159"></a>

### 159 外部 Python 得分：SQL 路径差异不等于业务 SQL 不同

**任务：** `ba_search.hotel_search_interest_model_semantic_extension_gcn`。Excel 第 55 行；原始记录行号 159。

**运行观测：** size 为 new=42,903,036、baseline=42,903,036、baseline_again=42,903,036；差异列：`number`。原框架 reason：`sql_diff|acceptable_diff|unstable_sql`。

**证据与表达式。** `number` 是 STRING，样本为 `0.8120620611754622` 与 `0.8120620611754621`，两条 embedding 字符串 one/two 在样本中相同。输出由 `TRANSFORM ... USING 'python hotel_search_interest_model_semantic_extension_gcn.py'` 产生。精确 SQL 对比发现，new/baseline 唯一文本差异是 ADD FILE 的 session 目录，分别含 `session_625E98315C0F2A5E` 与 `session_3ACF00DD2DCFDF7E`；业务日期相同。

**可能原因。** 脚本内容/依赖版本、Python 数学或数值库的跨平台路径、并行归约、浮点转字符串都可能产生该尾差。脚本未随包提供，不能把 number 确认为某个具体相似度算法。也不能因临时目录不同就认定“差异与引擎无关”。输入构造有多列各自 `sort_array(collect_list(...))`，同时间戳下这些列可能分别排序而错位，这是另一个需要检查的风险。

**验证方案。**

1. 取回两边实际 ADD FILE 脚本并比 SHA256，同时保存 Python/依赖库/CPU/线程配置。只有脚本哈希相同才能排除脚本版本差异。
2. 保存 TRANSFORM 的原始 stdin 字节，按 query_id 比较所有数组字符串及记录顺序。相同输入分别离线运行相同脚本，记录 number 的原始数值和格式化结果。
3. 若 stdin 不同，检查同一时间戳的多商品记录、NULL 拼接和多个独立数组的对应关系；诊断时以同一 struct 数组整体排序。
4. 若 stdin 完全相同且 Python 输出仍不同，再测试固定依赖、单线程及数值库/平台因素。

**判定。** 现有证据指向外部脚本数值链，不能直接定位 Velox 内建函数。`sql_diff` 与 `is_unorder_equal` 标签均不足以结案。

<a id="case-166"></a>

### 166 商品价格变更：相同时间戳下 LAG 改变去重

**任务：** `hmart_mlive.fact_sup_prod_live_goods_price_detail_day_inc`。Excel 第 56 行；原始记录行号 166。

**运行观测：** size 为 new=2,620,278、baseline=2,620,277、baseline_again=2,620,277；差异列：`comparepricetag, discount_price, goods_id, goods_type, live_id, live_status, market_price, partition_date, query_time, sale_price, timestamp`。原框架 reason：`size_diff|unstable_sql`。

**证据与表达式。** new 多 1 条。三个 LAG 均按 `(goods_id,goods_type,live_id,new_anchor_id,live_status)` 分区，只按 timestamp 排序；最后只保留三种价格与前一行不全相同的记录。目标价格为 DECIMAL(38,18)，discount_price 上游来自 JSON 字符串，NULL 被替换成 -999999 用于比较。

**可能原因。** 并列时间戳中不同价格记录的前后顺序改变，导致连续重复价格消除的数量不同，这是首选假设。备选是 JSON 提取后的隐式数值转换、NULL/空串/非数值处理不同，或源输入有增减。全字段 checksum 不同主要符合记录多一条，不能解读为 11 个表达式都各自出错。

**验证方案。**

1. 对最终结果双向 `EXCEPT ALL`，定位完整窗口分区；检查该分区是否存在重复 timestamp，保留原始事件 ID/采集序号。
2. 输出三个原价、JSON 原文、discount_price 类型、三个 *_price_0、三个 LAG 值及保留标志，分别比较 t_base 和窗口后结果。
3. 在同一 t_base 上按确定的业务事件序列补充排序键重放；若并列是重复采集，先确定应选最新还是保留全部，再验证对应策略。
4. 若 t_base 已不同，单独测 JSON price 为缺失、空串、0、负数、小数及非法文本时的类型和 NULL 结果。

**判定。** 同一 t_base、不同 peers 顺序即可解释多一行时，定位 SQL 排序不足；全序输入仍不同才追窗口算子或比较语义。

<a id="case-169"></a>

### 169 广告 ARPU 等级：0.5 边界的离散翻转

**任务：** `hmart_addata.rpt_acct_tgt_activity_ms`。Excel 第 57 行；原始记录行号 169。

**运行观测：** size 为 new=1,982,922、baseline=1,982,922、baseline_again=1,982,922；差异列：`cpc_arpu_level`。原框架 reason：`unstable_sql`。

**证据与表达式。** `account_id=74810944,hp_stat_month=2026-09` 的 `cpc_arpu_level` 为“中ARPU”与“低ARPU”。链路为按账户 SUM cpc_charge，按城市/分类 AVG cpc_charge，再按 `cpc_charge/avg_cpc_charge>2`、`<0.5` 分类。其他输出列 checksum 相同，但 cpc_charge 和区域平均值本来就不在最终输出中。

**可能原因。** 两级数值聚合的微差可能使比值跨 0.5；亦可能同键关联倍数、源账期输入变化改变了区域均值。现有样本没有原始比值，因此“恰好跨 0.5”仍为待证假设；NaN/NULL/零分母也需检查。

**验证方案。**

1. 固定账户及其城市/类别内的全部账户，不只查询单个账户。记录各账户 cpc_charge、成员个数、区域总额/均值、最终比值及 CASE 每个条件。
2. 若比值接近 0.5，使用金额精度一致的 DECIMAL 对照，比较 `charge*count` 与 `0.5*sum_charge` 的精确关系，注意 NULL 分母与 AVG 非 NULL 计数一致。
3. 逐层检查 launch 维表关联重复、账户到分类的 MAX 规则和输入快照；只在这些一致后改变聚合方式重跑。

**判定。** 这是用户等级变化，不能套用浮点容差直接接受。需要确认边界等号归属，并使最终分类规则稳定且符合业务约定。

<a id="case-172"></a>

### 172 意向召回 JSON：LOG10 尾差与多层排名

**任务：** `hmart_travel_top_data_gather.tjtg_intention_geohash_recall`。Excel 第 58 行；原始记录行号 172。

**运行观测：** size 为 new=3,438,737、baseline=3,438,737、baseline_again=3,438,737；差异列：`recall_json`。原框架 reason：`unstable_sql`。

**证据与表达式。** `recall_key=tjtg_intention_recall_geohash_brand_1000159_wt022` 的商品 `1670629531` 权重为 `7.715425971924328` 与 `7.71542597192433`。表达式为 `1.0+(uv_ctr*0.2+uv_cxr*0.8)*LOG10(order_cnt+1)*100`，之后每个维度/geohash 按 weight 取 `rn<=1000`。上游还有按 order_cnt 选择 `search_cate3_id` 的 `rk=1`。

**可能原因。** 当前样本优先支持数值计算/CAST STRING 尾差；不能只归为 collect_set 无序。另有类别选择并列及 Top 1000 边界变化风险，实际 ROW_NUMBER→RANK 改写可能影响入选数量。

**验证方案。**

1. 对样本商品固定质量表输入，逐步比较 LOG10、乘加与最终 weight 的位模式，然后用同一 DOUBLE 单独测试 CAST STRING。
2. 以 `(dimension,id,geohash5,deal_id)` 比较完整候选集合和次数，覆盖 rank 前后；单独检查第 1000 位附近的并列与近似同分。
3. 上游按商品核对 search_cate3 的候选、order_cnt 和 rk 改写；如果类别归属不同，则先解决类别选择，再检查权重。

**判定。** 数值尾差、类别归属、候选选入和最终字符串顺序应分别记录；当前样本只能直接证明其中的数值文本变化。

<a id="case-175"></a>

### 175 商品分数排名：先确认 RANK 改写与 checksum 算法

**任务：** `hmart_supplymining.shangou_select_item_for_9k9_feed_new`。Excel 第 59 行；原始记录行号 175。

**运行观测：** size 为 new=34,956,933、baseline=34,956,933、baseline_again=34,956,933；差异列：`score_rank_no`。原框架 reason：`unstable_sql`。

**证据与表达式。** 只有 BIGINT `score_rank_no` checksum 不同，SQL 表面为无 PARTITION 的 `row_number() over(order by score desc)`，没有后续 Top N 过滤。score 源于 `POWER(2.000,order_score)-1` 与 `0.4*score+0.6*订单量`。运行 conf 明确包含 `spark.sql.convert.rowNumber.to.rank=true`；没有具体排名差异样本。

**可能原因。** 必须优先检查实际是否变为 RANK。若已改写，DOUBLE score 的微差可使同分组拆分/合并，改变 rank 值多重集；但 score 本身是输出 DOUBLE，且其 checksum 相同，因此必须先查它是否逐行、位级一致，以及 checksum 是否归一化/舍入或碰撞，不能虚构一个已知的精度转换来解释。若仍是完整 ROW_NUMBER，固定 N 行输出的排名值必为 1..N，纯粹交换同分行不应改变“无序、逐列”的 checksum，因此“只因同分顺序随机”不足以解释当前现象。还需排除 checksum 是否包含行位置/其他列及窗口实现缺陷。

**验证方案。**

1. 获取 optimized/executed plan 和 checksum 计算 SQL/代码，确认窗口函数、输出类型、是否存在隐藏过滤以及哈希范围。
2. 若为 ROW_NUMBER，检查 count/min/max/count(distinct rank) 与 rank 直方图，并比理论 1..N；另以唯一商品键比较 `(item_id,rank)`。
3. 若为 RANK，物化 POWER 前后的 score，比较位模式、每个 score 的同分人数及 rank 分布；分别固定相同 score 只重放窗口、固定相同 order_score 只重放 POWER。
4. 在明确函数语义后以稳定商品键打破并列作诊断，不能把增加唯一键造成的 RANK 语义变化当成等价修复。

**判定。** 先分清 RANK、ROW_NUMBER 与比较器，才谈同分排序。此条是本批次最需要执行计划佐证的案例之一。

<a id="case-180"></a>

### 180 广告转化量：DOUBLE SUM 与派生 CVR

**任务：** `hmart_adrec.mt_rec_float_abtest_effect`。Excel 第 60 行；原始记录行号 180。

**运行观测：** size 为 new=166、baseline=166、baseline_again=166；差异列：`roi_cnt`。原框架 reason：`空`。

**证据与表达式。** 唯一差异为 DOUBLE `roi_cnt=sum(roi_cnt)`，按 `exp_tag,strategy_key,item_source,cat0_id,归一化price_mode` 分组，源表 `mart_adrec.mt_rec_float_abtest_data_collection`、dt=20260923。没有具体差值；CVR=`sum(roi_cnt)/sum(click_cnt)` 的 checksum 相同。

**可能原因。** 浮点 SUM 合并顺序不同最符合局部差异，但 `roi_cnt` 虽名为 cnt，目标类型是 DOUBLE，不能当成纯整数计数。CVR 相同可能由最后除法舍入掩盖小差异、NULL 或 checksum 局限造成，必须逐行确认。若源值均为精确可表示整数且总量未超安全范围，则应重新检查其他机制。

**验证方案。**

1. 以五个真实分组键找到差异组，检查源 roi_cnt 类型、是否含小数/NaN/NULL、贡献行多重集。
2. 输出 SUM、非 NULL 数及精确数值参照，在 x86/ARM Velox 上以同一物化输入单独重放 SUM，分别控制聚合合并方式。
3. 同时输出 click_cnt 与未写出前 CVR，验证是否发生数值差异却在除法或输出后被掩盖。

**判定。** 同输入浮点累计差异可进入容差评估；若 roi_cnt 是精确整数且无溢出，不能以浮点理论草率解释。

<a id="case-199"></a>

### 199 百米内去重人数：边界筛选优先于 COUNT DISTINCT

**任务：** `hmart_mdata.dwd_map_loc_uuid_count_d`。Excel 第 61 行；原始记录行号 199。

**运行观测：** size 为 new=137,406,282、baseline=137,406,282、baseline_again=137,406,282；差异列：`loc_uuid_count`。原框架 reason：`空`。

**证据与表达式。** `mid=1197828707880841290,dt=20260924` 的 loc_uuid_count 为 473 与 472。SQL 按 geohash7 关联 POI 和用户定位，计算 `lng_lat_to_distance`，以严格 `distance<100` 筛选，再 `count(distinct uuid)`，目标 INT。

**可能原因。** 距离在 100 米阈值两侧变化，导致一个 UUID 是否有合格定位记录不同；域外/NaN 和坐标输入不同也会影响筛选。不能因最终表达式是 COUNT DISTINCT 就直接断言去重算法错误。该 UDF 的实现及单位必须从实际注册函数确认，文件未提供函数体。

**验证方案。**

1. 对样本 mid 保留全部 geohash7 候选定位，输出 uuid、四个坐标、distance、isnan、`distance<100`，按源定位记录键比较。
2. 对过滤后 `(mid,uuid)` 先 DISTINCT，再双向 EXCEPT 找 UUID 集合差异；对每个 UUID 检查所有定位，确认是否完全失去/新增有效记录。
3. 记录两边 UDF 类/JAR/原生映射、哈希和调用路径，使用同坐标单独重放。最后将完全相同的过滤后快照输入 COUNT DISTINCT。

**判定。** 集合已差一 UUID 时优先定位筛选/输入；只有过滤后完整输入相同、COUNT DISTINCT 仍不同，才将去重算子升级为首要嫌疑。计数差 1 也不能证明只影响一个原始定位点。

<a id="case-201"></a>

### 201 JVM 内存比例：除法、AVG、ROUND、STRING 四层

**任务：** `hmart_mall.fact_com_meta_sprark_memory_dt`。Excel 第 62 行；原始记录行号 201。

**运行观测：** size 为 new=35,279、baseline=35,279、baseline_again=35,279；差异列：`avg_jvm_heap_used_ratio`。原框架 reason：`unstable_sql`。

**证据与表达式。** 唯一差异列 `avg_jvm_heap_used_ratio` 的目标类型为 STRING，来自 `round(avg(a.jvm_heap_used/b.allocated_memory),2)`。按 container_id 关联日志后按 application_id 聚合，再参与其他任务/队列关联。没有保存具体样本。

**可能原因。** 每行除法和 AVG 的微差跨越 0.005 舍入边界、同值 ROUND 语义不同、或最终转 STRING 格式差异都可能。container_id 在容量日志中不唯一会造成重复加权，这是重要备选；不能把“平均比例”误换成“总 used / 总 allocated”，两者业务含义不同。

**验证方案。**

1. 按 application_id 及最终记录维度定位差异，查容量日志 container_id 唯一性，保留采样时刻及原始用量/容量。
2. 导出逐行比值及类型、有效行数、SUM、未舍入 AVG、ROUND 与 STRING；分别物化 AVG 和 ROUND 的结果以隔离后续阶段。
3. 用同一数值位模式重放 ROUND/CAST，并检查零容量、NULL、NaN；用高精度参照验证半分位边界。

**判定。** STRING 可能仅格式不同，也可能实际比例相差 0.01；获取原值后必须分开。不能仅从 checksum 推断舍入缺陷。

<a id="case-202"></a>

### 202 页面停留时间：AVG 跨整数边界后截断

**任务：** `hmart_mapc.app_rpt_gc_surrounding_map_base_indicator`。Excel 第 63 行；原始记录行号 202。

**运行观测：** size 为 new=793,697、baseline=793,697、baseline_again=793,697；差异列：`page_stay_time`。原框架 reason：`unstable_sql`。

**证据与表达式。** 样本维度为“POI地址地图/整体/整体/其他/济源”，日期 2026-09-24，DAU=26；page_stay_time 为 3 与 4。表达式是 `cast(avg(if(page_stay_time<3600000,page_stay_time,0)/1000) as int)`，最终表列 BIGINT，且 GROUP BY CUBE 五个维度。

**可能原因。** 逐行毫秒→秒除法和 AVG 的误差使均值在 4 两侧，随后 CAST INT 截断产生 1 秒差异；目前未保存未截断均值，这只是有针对性的假设。源停留值、维表一对多、CUBE 分组或 NULL 条件处理也可能改变均值。DAU=26 不等于 AVG 分母为 26。

**验证方案。**

1. 在 COALESCE“整体”之前保留 `grouping_id` 和原维度 NULL 状态，避免不同 CUBE 层或真实“整体”字符串被误配；检查相关维表重复键。
2. 对真实同组导出原停留毫秒、IF 后值、秒数、有效行数、SUM、AVG、CAST INT、最终 BIGINT，确认是否为 3.999… 与 4.000…。
3. 以完全相同输入计算高精度参照。若原始毫秒为整数，可先精确累计 IF 后的整数毫秒，再除有效行数和 1000 作诊断；保留原 IF 对 NULL 的实际语义，不擅自改分母。

**判定。** 中间均值微差足以造成最终整数差异，但业务输出差 1 秒不能直接按浮点容差接受；如果均值离整数边界很远，应优先检查行集/分组。

<a id="case-204"></a>

### 204 关联客户风险 JSON：坐标行不同，先核对匹配与快照

**任务：** `hmart_caterb2b_mall_risk_management.topic_fake_achievement_multi_biz_type_customer_cka.903103`。Excel 第 64 行；原始记录行号 204。

**运行观测：** size 为 new=7,981、baseline=7,981、baseline_again=7,981；差异列：`risk_params_json`。原框架 reason：`unstable_sql`。

**证据与表达式。** 样本 `(customer_id=12640013,related_customer_id=30675857,dt=20260924,etl_id=903103)` 中关联客户坐标分别为 `(lat=39.929069,lng=116.319289)` 与 `(lat=39.92902,lng=116.319482)`，距离为 `15.713170758352351` 与 `20.350452048623627`。customer_delivery 从多日历史地址关联而来，最终按 customer_id 连接两端地址，未选择唯一日期/地址。

**可能原因。** 输入坐标已经不同，不能用相同坐标的三角尾差解释 4.6 米距离变化。优先排查多地址组合导致的样本配错、历史/实时维表快照变化；若两边完整地址组合相同，只是比较器将不同组合配成一对，样本是假配对。若同输入组合最终确有缺失/新增，再查关联/DISTINCT；距离数学路径为后续分支。

**验证方案。**

1. 对该客户对取回所有结果 JSON，展开两端 POI、历史日期、源地址主键及坐标；比较完整组合多重集，不能用客户对单键做一对一 join。
2. 冻结 `dim_transport_poi_delivery_address_his`、fact_delivery 及当前配送地址/POI 维表；依次检查 customer_delivery DISTINCT 前后及两端关联的行数、候选组合。
3. 找出能在两边唯一对应的相同坐标组合，只对这些记录重放 Haversine 表达式。另核验 create_time 的确定化设置，样本该列相同，非当前首要原因。

**判定。** 当前样本首先证明坐标不一致；需要完整多重集才能在源漂移、样本误配、关联问题之间选择。不能沿用原表的“已确认跨平台三角精度”标签。

<a id="case-206"></a>

### 206 商品相似推荐：PMI 排名边界改变候选集合

**任务：** `hmart_generalshop.tp_output_recommend_whole_i2i_info_d`。Excel 第 65 行；原始记录行号 206。

**运行观测：** size 为 new=13,791,516、baseline=13,791,516、baseline_again=13,791,516；差异列：`extend_info, rel_product_ids`。原框架 reason：`unstable_sql`。

**证据与表达式。** extend_info 样本 `(platform=MT,bu_name=life,product_id=1651292611)` 有一侧缺少 `1649396059:36.19`；rel_product_ids 样本 `(MT,le,1593973753)` 有一侧缺少 `1255891861:29.86`，日期 2026-09-23。逐项拆解后，前者分别含 **502/503 个不同商品**，后者分别含 **102/101 个不同商品**，不是重复项造成的计数膨胀。两列分别收集 `rk<=500`、`rk<=100` 的商品。PMI=`log2(a.cnt/((b.cnt/d.cnt)*(c.cnt/d.cnt)))`，按 PMI 降序 ROW_NUMBER；样本数量已经超过表面上限，结合 conf，强烈支持优先核查实际 RANK 改写及边界 peers，不能继续按未改写的 Top N 解释。

**可能原因。** 已观察到候选增减，不能只归因 collect_list 顺序。PMI 浮点差异可能拆分/合并同分组，改变 RANK 边界或 ROW_NUMBER 选取；如果实际用 RANK，边界并列数量变化尤其值得检查。四组整数计数或共用 staging 数据不同也是候选。原 SQL 的 `distribute by/sort by` 并不单独证明后续 collect_list 全局稳定。

**验证方案。**

1. 将字符串拆成保留重复项的 `(product_id_y,展示分数)` 记录，明确缺失/新增/次数/数值/顺序五类差异，不能先无序转集合抹掉次数。
2. 对两个样本产品固定 staging 的 product_list，比较 a/b/c/d 的计数、除法中间值、LOG2、未舍入 PMI、实际 rk；输出 100/500 边界周围全部 peers。
3. 获取实际 ROW_NUMBER/RANK 和确定化集合规则。使用相同 PMI 重放窗口，使用相同整数计数重放 PMI，分清数值与排名改写。
4. 固定入选集合后单独验证 `round(pmi,2)` 和 collect_list 序列化。每次运行使用独立 staging 表，排除两边 drop/create 同名表互相覆盖。

**判定。** 即使文本列表顺序不重要，候选成员变化仍需单独验收。`is_unorder_equal` 标签无法证明当前样本只差顺序。

## 6. 执行顺序与结案标准

已知比较平台为 Velox x86 与 Velox ARM。建议先补充 ARM 重复运行，并取得执行计划和比较器实现，尤其确认两侧具体算子路径、ROW_NUMBER 改写、集合确定化和 checksum 的输入范围。之后按以下顺序推进，不需要先重跑所有上亿行任务：

1. **先排除资料/比较前提问题：159、122、204、175。** 分别核对脚本哈希、联系人/地址完整多重集以及实际窗口函数。这些证据能直接改变问题归属。
2. **优先调查业务结果变化：115、119、166、199、169、202、206、104、124。** 这些涉及条数、计数、类别、整数时间、候选成员或账龄金额，不用统一浮点容差豁免。
3. **复用数值最小用例：94、96、98、103、105、120、121、130、135、138、146、147、148、154、155、172、180、201。** 分别建立 ACOS/EXP/LOG10/LOG2/LN、SUM/AVG、ROUND、数值转 STRING/FLOAT 的固定输入测试，逐条回填证据；复用机制，不复用未经证明的根因。

每条结案至少保留：相同输入快照标识、实际执行路径、唯一对应的差异键或完整多重集、首个不同的中间值/算子、能稳定触发的最小数据、单因素对照结果及修复后回归。若最终是业务允许的精度差异，明确字段/单位/容差和离散结果不能变化的约束；若是源漂移或样本误配，应修复测试隔离或比较器，并重新验证原始问题。

## 7. 可直接改造的诊断 SQL

以下仅为只读诊断示例，未执行。运行前依 V0 固定源快照；代码中的原表名与日期来自文件。对临时诊断视图的名称和类型应在两边保持一致。

### 7.1 行号199：先拿出差异 POI 的过滤前候选

```sql
WITH candidate AS (
  SELECT ta.mid, tb.uuid,
         ta.lng AS poi_lng, ta.lat AS poi_lat,
         tb.lng AS user_lng, tb.lat AS user_lat,
         lng_lat_to_distance(ta.lng, ta.lat, tb.lng, tb.lat) AS distance
  FROM mart_mdata.dwd_map_poi_location_neighbour_d ta
  JOIN mart_mdata.dwd_map_mars_data_location_d tb
    ON ta.geohash7 = tb.geohash7
  WHERE ta.dt = '20260924' AND tb.dt = '20260924'
    AND ta.mid = '1197828707880841290'
)
SELECT *, isnan(distance) AS distance_is_nan,
       distance IS NULL AS distance_is_null,
       distance < 100 AS passed
FROM candidate;
```

分别将输出保存为不可变的 baseline_candidate/new_candidate。先比四个坐标和 uuid，再对过滤后的 UUID 集合做双向差集；若同一 UUID 有多条定位，不能只比较一条定位距离。

### 7.2 行号138：分开加权均值与 ROUND

```sql
SELECT platform_poi_id, geohash,
       count(*) AS row_n,
       count(peisong_total_num * avg_delivery_distance) AS numerator_n,
       sum(peisong_total_num) AS weight_sum,
       sum(peisong_total_num * avg_delivery_distance) AS weighted_sum,
       sum(peisong_total_num * avg_delivery_distance) * 1.0
         / sum(peisong_total_num) AS before_round,
       round(sum(peisong_total_num * avg_delivery_distance) * 1.0
         / sum(peisong_total_num), 2) AS after_round
FROM mart_peisongpa.dynamic_scope_poi_waybill_block_geohash
WHERE dt = '20260921'
  AND platform_poi_id = 9422217
  AND geohash = 'ws7emq5'
GROUP BY platform_poi_id, geohash;
```

上述同一 SELECT 只用于第一轮观测。若需严格隔离 ROUND，应把 before_round 及其类型物化到相同文件，再由两边分别读取，避免优化器重新计算前序聚合。

### 7.3 行号166：查窗口 peers

```sql
-- diag_t_base 是从原 SQL 原样物化的 t_base，必须保留全部窗口列。
SELECT goods_id, goods_type, live_id, new_anchor_id, live_status,
       `timestamp`, count(*) AS peer_n
FROM diag_t_base
GROUP BY goods_id, goods_type, live_id, new_anchor_id, live_status,
         `timestamp`
HAVING count(*) > 1;
```

先用最终差集定位的分区缩小查询，再查看 peers 的价格和源事件序列。存在 peers 只是必要线索，只有实际改变 LAG/保留谓词才能解释本案。

### 7.4 行号175：区分 ROW_NUMBER 与 RANK 的排名分布

```sql
-- diag_rank_output 是原查询窗口后、写出前的完整结果。
SELECT count(*) AS n, count(score_rank_no) AS nonnull_rank_n,
       min(score_rank_no) AS min_rank,
       max(score_rank_no) AS max_rank,
       count(DISTINCT score_rank_no) AS distinct_rank_n
FROM diag_rank_output;

SELECT score_rank_no, count(*) AS n
FROM diag_rank_output
GROUP BY score_rank_no;
```

若实际为完整 ROW_NUMBER，且没有过滤，每个 1..N 应恰好出现一次。RANK 允许重复和跳号，应比较同分组大小，不能套用 ROW_NUMBER 的检查标准。

## 8. 实现参考及适用边界

本地 `velox` 仓库 HEAD 为 `d885538c9470952f04afd1cf1822f4af08b8f30e`，与运行记录的 `901e2e0f…` 不同，且本地未找到后者对象。以下代码只解释可疑机制和后续定位位置，**不是对运行版本缺陷的确认**。

- 本地 Spark 方言数学函数注册将 ACOS、EXP、ROUND 等绑定到相应实现；三角函数和 EXP/LOG10 使用 C++ 数学库调用。此次应优先检查 x86/ARM 的 libm、编译选项、架构特定代码及 SIMD 路径。只有执行计划证实两侧某阶段回退情况不一致，才将 JVM/native 路径不同作为该阶段的解释；不能仅凭 GCC 版本不同推断 FMA 已开启。
- 本地 ROUND 浮点实现包含按 10 的幂缩放后 `std::round` 的路径。此次两侧均为 Velox，因此不能以“Spark BigDecimal 与 Velox std::round 不同”直接解释观测；94/135/138/201 应首先区分跨架构舍入前输入已经不同，还是相同输入位模式的 ROUND/转换结果不同。Spark 3.5.1 公开源码的 HALF_UP/BigDecimal 路径仅作为方言语义和可选补充对照参考，参见 [Spark 3.5.1 数学表达式源码](https://github.com/apache/spark/blob/v3.5.1/sql/catalyst/src/main/scala/org/apache/spark/sql/catalyst/expressions/mathExpressions.scala)。
- 本地 AVG 基类逐值及合并阶段均对 sum 做加法累积。这支持检查浮点加法顺序的必要性，但不能据此证明某个案例实际经过该路径。

| 定位点 | 本地代码 |
| --- | --- |
| Spark 数学函数注册 | [velox/functions/sparksql/registration/RegisterMath.cpp:37](C:/Users/Administrator/Desktop/s30082773/boostkit/velox/velox/functions/sparksql/registration/RegisterMath.cpp:37) |
| 三角函数与 EXP | [velox/functions/prestosql/Arithmetic.h:268](C:/Users/Administrator/Desktop/s30082773/boostkit/velox/velox/functions/prestosql/Arithmetic.h:268) |
| 浮点 ROUND | [velox/functions/prestosql/ArithmeticImpl.h:37](C:/Users/Administrator/Desktop/s30082773/boostkit/velox/velox/functions/prestosql/ArithmeticImpl.h:37) |
| Spark LOG10 | [velox/functions/sparksql/Arithmetic.h:360](C:/Users/Administrator/Desktop/s30082773/boostkit/velox/velox/functions/sparksql/Arithmetic.h:360) |
| AVG 累加与合并 | [velox/functions/lib/aggregates/AverageAggregateBase.h:240](C:/Users/Administrator/Desktop/s30082773/boostkit/velox/velox/functions/lib/aggregates/AverageAggregateBase.h:240) |

公开文档用于核对通用语义：[Spark 3.5.6 SQL 函数](https://spark.apache.org/docs/3.5.6/api/sql/index.html)、[Spark 3.5.6 NaN 语义](https://spark.apache.org/docs/3.5.6/sql-ref-datatypes.html#nan-semantics)、[Velox Spark 数学函数](https://facebookincubator.github.io/velox/functions/spark/math.html)。它们均不替代内部构建版本的源码与实际执行计划。

## 9. 文件核对记录

本地已完成：32 条 Excel/分析 CSV/原始 CSV/JSON row 键的关联；原始 CSV detail 严格解析；三次运行 size/checksum、差异字段、SQL 文本及样本的检查。没有执行原始 SQL 中的 INSERT、DROP、CREATE，也没有运行 159 的外部脚本。

下列 SHA256 用于固定本报告分析的四个文件版本：

| 文件 | SHA256 |
| --- | --- |
| 1791959046900_324条_分析.csv | `038154ba95c3eb3284c63c6bdda279842a20c03a3d3da9a5108cb55343f6acc7` |
| 1791959046900_原始数据.csv | `1354c5cc1ce6af6e20eb4a19c377f10050d114d99c81b1eeab73583de4606bef` |
| 1791959046900_原始行号+detail列.json | `2c49425e67d68395ab235bdbf74f58c312a77a9af3904e28fc1f050b7e9bfb08` |
| sql筛选_95条.xlsx | `85a092b8bbfd65755e79432bf3be014ce76503ca6bee0a4e16456321d969f63e` |

### 9.1 差异字段 checksum 追溯表

下表只提供指纹追溯，不表达误差大小。baseline_again 在这 32 条中与 baseline 相同，故不重复列出。

| 行号 | 字段 | new checksum | baseline checksum |
| --- | --- | ---: | ---: |
| 94 | pdp_gtv | -19746418144 | -19561993537 |
| 96 | reviewdis | -40270009969 | -47288035629 |
| 98 | total_roi_pay_cnt | -60400539611 | -58682344122 |
| 99 | geo_type | -3206938379779498 | -3206942013863164 |
| 99 | order_id | 1518867408719 | 1520363352870 |
| 99 | travel_distance_type | -2310069948910994 | -2310077137105210 |
| 99 | user_id | -60860164956195 | -60863119313495 |
| 103 | extension_info | -1068726913171 | -897855661263 |
| 103 | index_list | -1635710064413 | -1594604349240 |
| 103 | item_personattr | -561314724559 | -696003792104 |
| 104 | travel_distance_type | -3306815037801075 | -3306852579010704 |
| 105 | psi | 688914667 | 1749367438 |
| 115 | call_cnt | 320548898350 | 321068119057 |
| 115 | case_id | -25119465348 | -23814284715 |
| 115 | contact_type | 288419601761 | 288566993724 |
| 115 | department_name | -24004376658 | -22401993112 |
| 115 | dt | 774186116340 | 775887624288 |
| 115 | is_problem_transfer | -254609335435 | -255168916392 |
| 115 | is_trasfered | -254609335435 | -255168916392 |
| 115 | last_subbiz_name | -97919986902 | -99151878090 |
| 115 | lifecycle | 19110 | 19152 |
| 115 | mis_name | -21623365655 | -21264526331 |
| 115 | name | 58508507387 | 59716093445 |
| 115 | next_subbiz_name | -193346887322 | -194518962946 |
| 115 | ori_contact_id | 8219000435 | 8571899434 |
| 115 | ori_contact_id_w | 56131311606 | 56484210605 |
| 115 | oriani | -2693830000 | -2062856101 |
| 115 | partition_date | 774186116340 | 775887624288 |
| 115 | ring_start_time | -31995010107 | -31043445795 |
| 115 | staff_id | 1584881349 | 35713126 |
| 115 | subbiz_name | -97919986902 | -99151878090 |
| 115 | trasfered_contact | -100046814524 | -100844741796 |
| 119 | bu_name | -9679737260427927 | -9679707815969933 |
| 119 | datekey | -16744853667715482 | -16744826768903532 |
| 119 | device_id | 1373074385067 | 1363636794321 |
| 119 | travel_distance_type | -10423151878809220 | -10423211174535890 |
| 120 | second_cat4_avg_weight | 734090270459905 | 734089349779134 |
| 121 | recall_json | -10010570547559 | -16779017402132 |
| 122 | risk_params_json | -12639043437 | -13451914903 |
| 124 | in_3year | -563052079670501 | -563052080580540 |
| 124 | out_3year | -591965336138381 | -591963034336732 |
| 130 | psi | -121411961 | -1290549903 |
| 135 | reviewdis | 3475966993144 | 3519195411441 |
| 138 | avg_delivery_distance | -1496354282969 | -1496700498779 |
| 146 | reviewdis | 21020546065 | 18134896773 |
| 147 | psi | -1272110083 | 143533808 |
| 148 | avg_duration_hash | -808174903851135 | -808174651482670 |
| 154 | order_amt | -975291705029523 | -975291023604724 |
| 155 | increase_rate | 97454513124327 | 97452337405548 |
| 159 | number | 356021044176766 | 252728912186510 |
| 166 | comparepricetag | -22120957521339 | -22120957521381 |
| 166 | discount_price | 39984346436571 | 39984346436529 |
| 166 | goods_id | -5491621586892 | -5489780279167 |
| 166 | goods_type | -4012852532454225 | -4012850709372276 |
| 166 | live_id | -15027391803169 | -15026117005001 |
| 166 | live_status | 2840653744122145 | 2840651979090571 |
| 166 | market_price | -284263724436347 | -284264554272735 |
| 166 | partition_date | 1630194258158730 | 1630193636013195 |
| 166 | query_time | -2128244446702641 | -2128243780865434 |
| 166 | sale_price | -24103096498162 | -24102451403149 |
| 166 | timestamp | 131022510408865 | 131020441687020 |
| 169 | cpc_arpu_level | 177512320048703 | 177530898398413 |
| 172 | recall_json | -44081883004750 | -27975341530383 |
| 175 | score_rank_no | -4263191891087 | -4306555963873 |
| 180 | roi_cnt | 23538019276 | 21149671812 |
| 199 | loc_uuid_count | -5960198473305172 | -5960021990470104 |
| 201 | avg_jvm_heap_used_ratio | -9744591869610 | -9744195704962 |
| 202 | page_stay_time | -105707714474142 | -105708539567375 |
| 204 | risk_params_json | 66133017850 | 93772704346 |
| 206 | extend_info | -335012120068 | -325562093282 |
| 206 | rel_product_ids | -540992440674 | -575518021827 |
