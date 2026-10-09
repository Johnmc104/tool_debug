# FFR 功能缺口与修复方案

日期：2026-10-09。分析基线：`968a0f3`。

## 1. 结论

当前 FFR 后端还不能作为 NPI 的完整替代。问题主要出在工程适配层：把信号简化为“完整路径 → idcode”，只实现普通数字值遍历，漏掉了 NPI 在 reader 之上提供的位选、片选、结构体、组合数组和类型转换。

建议继续使用动态 FsdbReader，并定向分析 libNPI 的 FDA 层以补齐语义。公开 SDK、NPI 行为对照、局部反汇编三者结合；遇到公开接口缺少元数据时，再分析 NPI 如何向底层 reader 请求相关信息。当前没有证据要求重写 FSDB 磁盘解析器。

本轮先完成问题复现和调用链取证，随后已实施第一阶段修复。现在又补上了当前已验证 T-2022/Y-2026 Reader 的 packed struct 展开路径；string 仍未完成。

第一阶段已修改主工程后端：位选/片选、unpacked struct 路径、普通静态数组中间层和组合值、real 值/范围/计数，以及组合信号的 edge/count 已接入。修复后普通扩展矩阵保持原有 924 项、0 差异；结构体/数组样本的差异主要收敛为 packed struct 字段、string 和少量元数据语义。

## 2. 已复现的问题

| 场景 | NPI 实际结果 | 当前 FFR 结果 | 原因 |
|---|---|---|---|
| `fixture.data[3]` | 可查；1000ps 值为 `1` | （第一阶段已修复） | 只有树中完整信号索引，没有按需创建位选视图 |
| `fixture.data[7:4]`、`ascending[0]` | 片选和升序位范围可查 | （第一阶段已修复） | 没有选择范围解析与位偏移计算 |
| `memory[1][3]`、`matrix[1][2][3]` | 数组叶元素再做位选可查 | （第一阶段已修复） | 需先解析已存在的数组元素，再解析其 packed 位选 |
| `pair_value.a`（unpacked struct） | 保留正确字段路径 | （第一阶段已修复） | `tree()` 忽略 STRUCT_BEGIN/END，字段直接拼接 scope |
| `packet.payload`、`packet.tag`（packed struct） | 按字段读取 | （本轮已修复） | 对已验证 Reader 打开内部 packed-child 展开开关，再按公开树回调建立字段视图 |
| packed struct 父值 | 如 `{1010,00010011}` | （本轮已修复） | 按字段声明顺序合成值，并保留结构体花括号显示 |
| `matrix[1]` | 二维数组中间层可查 | （第一阶段已修复） | ARRAY_BEGIN 只建立最外层父项 |
| `memory`、`matrix` 父值 | 如 `{1,205}`、`{{120,154},{188,222}}` | （第一阶段已修复） | 缺少组合值与多个叶元素事件的合并 |
| `analog`（real） | 如 `-2.500000E+00`，可遍历和计数 | （第一阶段已修复） | Cursor 统一限制 1 byte/bit，连不需要值转换的计数也被拒绝 |
| string | 当前 NPI 的 bin/hex/oct/dec 字符串查询也失败，但 VC 计数可用 | 查询和计数都报未支持 | 需要区分 NPI 格式限制、Reader 原始编码和工程接口能力 |

位选和片选问题在配套 2022、2026 样本上均复现。结构体、二维数组和录波暂停区间使用新增 VCS/Verdi 2026 样本验证。普通 `bit [7:0]` 在该样本中以四态数字编码返回，可正常读取；不能仅根据 SV 源码声明推断 Reader 的编码。

暂停录波后，普通数字叶信号的 X 值及恢复值在本轮时间点对照一致；未观察到这一样本的 dumpoff 差异。真正没有初始 VC 的文件仍需另建样本，不能把暂停录波验证当作该边界已经通过。

## 3. libNPI 能提供哪些线索

本轮分析本机 `Y-2026.03-SP2/libNPI.so`，保存了库 SHA-256 和选定函数的地址、长度及反汇编。主要路径为：

```text
npi_fsdb_sig_by_name
  → fda_file_t::get_sig_by_name
  → fda_get_sig_by_name：分段解析、转义名字处理

npi_fsdb_goto_time
  → FDA 的叶信号 / 部分信号 / 组合信号 cursor
  → ffrVCIterOne::ffrGotoXTag / ffrGetXTag

npi_fsdb_vct_value
  → fda_vct_value_skip_check：根据请求格式分派转换
  → 数字 / real / string / 组合值的转换实现
```

`fda_vch_leaf_t::goto_time()` 明确调用 Reader 的时间定位、取时间和最大 VC 时间接口，并处理定位失败后的边界。`fda_vch_partial_t` 和 `fda_vch_composite_t` 是独立实现，组合定位还使用堆结构。说明位选和数组父值不能等同于一个普通 idcode 的原始事件序列。

例如 `data[3]` 在当前样本中的范围结果只有 5 项；父总线其他位发生变化时，不能直接把所有父总线记录截取后计入子位变化。修复必须对照范围、edge 和 vc-count，不能只修点值。

real 的转换路径包含 `fda_val_real_t::get_value_str_bin()` → `fda_double_to_char()`，并对 wreal 特殊 X/Z 做额外判断。普通 real 可以通过公开 Reader API 获得 8 字节 double；独立探针已读出 `1.25`、`-2.5`。格式化和特殊值处理是适配层工作。

FDA 的多数相关实现为本地符号，不能直接通过常规 `dlsym` 当作稳定接口使用。把这些私有对象和内部地址直接嵌入 FFR worker，会重新绑定 libNPI 的对象布局与版本，不能据此承诺保留当前免 NPI 初始化的运行方式。

## 4. 哪些内容公开 SDK 已经够用

普通数字位选、片选：读取父总线后按声明范围投影即可；需要工程自己维护视图和事件过滤。unpacked struct 的层次修复：公开 STRUCT_BEGIN/END 回调已经给出父项名字和结构类型。多维静态数组：公开 ARRAY_BEGIN/END 给出各层名字，叶元素具有自己的 idcode。普通 real：公开 VC 接口和 bytes-per-bit 元数据足以取出 double。

packed struct 的普通公开树默认只返回 `packet[11:0]`，`dtidcode=0`；`ffrGetVarInfoByVarIdcode()` 也没有返回字段 descriptor，`ffrHasDataTypeDef()` 对本样本为 false。继续检查 Reader 本地符号后，确认内部入口 `ffrObject::ffrEnablePackedModeExpandChildFlow(char)` 可以让同一公开树回调返回 `STRUCT_BEGIN(packet, PACKED_STRUCT)` 以及 `tag`、`payload` 字段。当前实现只对已验证的两个 SDK 版本启用对应入口偏移，并在树回调结束后按字段顺序计算聚合宽度；没有嵌入 libNPI 对象或调用 FDA 私有对象。

这个入口不是稳定 FFR ABI。后续 SDK 即使版本字符串相近，也必须重新取证入口位置和回调结果；找不到匹配入口时应继续使用普通父总线视图并报告能力边界，不能把当前偏移推广为通用兼容方案。

string 原始 VC 的 byte-count 为 4，不能按普通字符数组或 float 直接解释。本轮仅记录事件及元数据，未确认其引用值到文本的完整转换路径。当前 NPI worker 调用 L1 时统一传入数字 radix，也没有提供 `npiFsdbStringVal` 查询，因此 string 需要同时设计工程输出格式及后端能力。

## 5. 推荐修改方式

现有加载器、独立 worker、会话及 SDK 切换保留。第一阶段已将 `FfrBackend` 内部表示调整为可解析视图：

```text
SignalNode
  Leaf：idcode、编码、真实位范围
  Slice：父节点、选择范围、位偏移
  Composite：结构体或数组、维度、字段/元素及声明顺序

ValueCursor
  LeafCursor：Reader 原始事件
  SliceCursor：投影并识别子范围的有效变化
  CompositeCursor：合并子事件，重建组合值
```

同一份 SignalNode 用于名称解析、signal-info、点值、范围、edge、vc-count，避免每个命令分别补丁。scope 栈与结构体/数组的分组栈分开：分组属于信号，不能把它伪装成模块 scope。叶数组的真实索引名称优先精确匹配；匹配不到才尝试解析末尾位选，避免把 `memory[1]` 错当成 memory 的第 1 bit。

能力判断也需拆开。创建 cursor、取时间和计数不应依赖某一种 radix 转换是否实现；无法转换值时只拒绝相应值查询。unknown 编码和缺少字段布局必须明确报告，不能返回看似成功但层次或值格式错误的结果。

后续按以下顺序实施，每步都有可验收的结果：

1. packed struct：当前已完成 T-2022.06-SP2/Y-2026.03-SP2 的展开适配；新增 EDA/Reader 版本必须重新验证内部入口和字段顺序。
2. string、动态数组、class 等按实际需求扩展，不与 packed struct 混为一个大版本。
3. 对组合信号补充同时间多字段变化、无初始 VC 和大规模性能测试。
这不是完整克隆 NPI。先覆盖 vwave 现有命令实际需要的信号解析、类型描述和事件语义，继续由厂商 Reader 负责磁盘读取与解压。

## 6. 验证与证据

修复前的新增矩阵共 **781 项查询，641 项与 NPI 存在差异**：2022/2026 现有样本扩展矩阵各 202 项、172 项差异；2026 新样本 377 项、297 项差异。其中包括首版已经声明不支持的类型，不应把 641 解释为 641 个独立 bug。

第一阶段修复后，现有样本 924 项仍为 0 差异；配套 2022 样本 3404 项、配套 2026 样本 1709 项均为 0 差异。扩展位选矩阵为 202 项、47 项差异；启用 packed struct 展开后，结构体/二维数组矩阵 377 项降为 26 项差异。剩余主要是 string 未支持、FFR 列出二维数组中间节点的元数据差异，以及一个数组位选 edge 边界语义差异；`packet.payload`、`packet.tag`、packed struct 父值和 `packet[7:0]` 点值/范围已对齐。

此前 4342 项、0 差异只覆盖当时的数字信号矩阵；不能推出所有数字访问形式和所有类型都兼容。新增位选、结构体和数组中间层正好补出了原测试的空白。

- [2022 扩展查询摘要](../reverse_analysis/evidence/ffr_support/expanded2022_queries.json)
- [2026 扩展查询摘要](../reverse_analysis/evidence/ffr_support/expanded_queries.json)
- [结构体与二维数组查询摘要](../reverse_analysis/evidence/ffr_support/support_queries.json)
- [第一阶段修复后的位选摘要](../reverse_analysis/evidence/ffr_support/expanded_after_fix.json)
- [第一阶段修复后的结构体/数组摘要](../reverse_analysis/evidence/ffr_support/support_after_fix.json)
- [第一阶段配套样本回归](../reverse_analysis/evidence/ffr_support/fixture_after_fix_2022.json)、[2026 回归](../reverse_analysis/evidence/ffr_support/fixture_after_fix_2026.json)
- [packed struct 展开后的结构体/数组差分](../reverse_analysis/evidence/ffr_support/support_packed_after_fix.json)
- [packed struct 内部入口取证](../reverse_analysis/evidence/ffr_support/packed_expand_probe.txt)
- [Reader 原始树与 real 事件](../reverse_analysis/evidence/ffr_support/support_raw.txt)
- [libNPI 定向调用链](../reverse_analysis/evidence/ffr_support/npi_calls.json)
- [差分与取证脚本](../reverse_analysis/scripts/analyze_ffr_support.py)
- [新增 SV 样本](../reverse_analysis/probes/ffr_support_fixture.sv)、[独立 Reader 探针](../reverse_analysis/probes/ffr_support_probe.cpp)

完整查询结果与 VCS 编译/仿真日志保存在忽略的 `build/ffr_support_analysis/`；工程只保存自编样本、探针、脚本和文本证据，不纳入厂商库或 SDK 头文件。

复现查询与调用链：

```bash
python3 reverse_analysis/scripts/analyze_ffr_support.py \
  --sample build/backend_fixtures/Y-2026.03-SP2/fixture.fsdb \
  --sdk /opt/Synopsys/verdi/Y-2026.03-SP2

python3 reverse_analysis/scripts/analyze_ffr_support.py --support-fixture \
  --sample build/ffr_support_analysis/support.fsdb \
  --sdk /opt/Synopsys/verdi/Y-2026.03-SP2 \
  --output build/ffr_support_analysis/support_queries.json
```
