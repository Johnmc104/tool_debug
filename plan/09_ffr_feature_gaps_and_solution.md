# FFR 后端功能边界与重构计划

日期：2026-10-09。对应发布版本：`tool_wave v1.7`。实现基线：`4eacd7a`。

## 1. 结论

FFR 后端继续采用动态加载配套 FsdbReader 的方案，NPI 后端保留，通过 `--backend npi|ffr` 切换。FFR worker 不链接 libNPI、不调用 NPI 初始化，也不携带厂商 reader；运行时从用户指定的 EDA/Verdi 安装加载 `libnffr.so` 和 `libnsys.so`。

当前已验证的 FFR 能力包括：

- 普通四态数字值、宽总线、升序和降序位选/片选。
- unpacked struct 字段路径、packed struct 字段路径和 packed struct 父值。
- 一维/多维静态数组、数组中间节点、数组叶元素再做位选，以及组合值和组合事件。
- real 点值、范围、计数和 edge。
- 与 NPI 一致的范围起点、重复父总线 VC 过滤、组合信号 edge/count 和 string 失败语义。

这不是完整的 NPI 或 FSDB 原生解析器。磁盘格式、压缩、索引和厂商 Reader 仍由 FsdbReader 负责；工程只在公开 FFR API 返回的树、idcode、VC 和类型信息之上重建 NPI 所需的查询语义。

## 2. 兼容范围

当前代码只接受并验证以下 Reader 构建：

| Reader | 配套验证 | CentOS 7/glibc 2.17 |
|---|---:|---:|
| T-2022.06-SP2 | 通过 | 通过 |
| Y-2026.03-SP2 | 通过 | 不通过，库自身需要更高 glibc |

“Reader 版本匹配”包含 EDA 版本、FsdbReader 构建和 FSDB 文件版本三层关系。不能用 2022 Reader 读取所有 2026 波形，也不能因为接口头文件相同就认为不同 patch 版本的内部行为相同。新增版本必须重新验证：

1. `ffrGetVersion`、公开入口和动态库来源检查。
2. 普通树、packed struct 展开树、VC 编码和时间边界。
3. glibc、libstdc++ 和 `libnffr.so`/`libnsys.so` 的运行依赖。

## 3. 已闭环的功能缺口

| 场景 | 重构后的处理 | 验收状态 |
|---|---|---|
| `data[3]`、`data[7:4]`、`ascending[0]` | 建立 Slice 视图，按父总线声明范围投影 | 已通过 |
| `memory[1][3]`、`matrix[1][2][3]` | 先精确匹配数组叶元素，再解析末尾位选 | 已通过 |
| `pair_value.a` | 分离 scope 栈和 struct 分组栈，保留字段路径 | 已通过 |
| `packet.tag`、`packet.payload` | 对已验证 Reader 开启 packed-child 展开，按字段顺序建立叶节点 | 已通过 |
| packed struct 父值 | 用字段值合成结构体值，保留 `{field,...}` 显示 | 已通过 |
| `matrix[1]`、`matrix` | 组合节点保留查询能力；中间节点从 scope signal 列表隐藏以匹配 NPI | 已通过 |
| real | 读取 8 字节 double，单独处理值、范围和计数 | 已通过 |
| string | 点值返回 NPI 同样的 `FILE_READ_ERROR`；范围为空；保留 VC 计数和 edge 时间 | 已对齐，未实现文本解码 |
| Slice 的 backward-any edge | 按 NPI partial cursor 的父事件和投影变化语义重建 | 已通过 |

string 的当前行为是兼容 NPI 的失败行为，不代表 FFR 已经能把原始 string VC 解码成文本。当前 NPI worker 也没有请求 `npiFsdbStringVal`，因此继续伪造文本值会比返回明确错误更危险。

## 4. FFR 后端重构结构

后端内部把“信号路径”和“Reader idcode”分开，节点分为三类：

```text
SignalNode
  Leaf       idcode、类型、bytes-per-bit、声明位范围
  Slice      source、选择范围、父节点到子节点的位映射
  Composite  struct/array 子节点、声明顺序、组合事件来源
```

树回调阶段只建立名称和节点关系，不读取 VC。查询阶段再按需加载叶信号并缓存 cursor：

```text
名称解析
  exact leaf/array element
  → composite child
  → trailing bit/range slice

点值
  Leaf cursor → 编码转换
  Slice       → 父值投影
  Composite  → 子节点值合成

范围、edge、vc-count
  Leaf       → Reader VC 遍历
  Slice      → 父 VC 遍历后过滤投影未变化的记录
  Composite  → 合并各子节点事件时间并去重
```

范围、edge 和计数共用同一套节点解析，避免只修点值而遗漏事件语义。组合数组的中间节点保留在内部 `variables_`，但不塞进 NPI 不会展示的 scope signal 列表。普通字符串在点值、范围、计数和 edge 上分别映射 NPI 的实际返回行为。

## 5. packed struct 的特殊处理

公开 FFR API 对当前样本的默认行为不足以还原 packed struct：

- 普通树只返回 `packet[11:0]`，`dtidcode=0`。
- `ffrGetVarInfoByVarIdcode()` 没有返回字段 descriptor 或布局。
- `ffrHasDataTypeDef()` 对样本返回 false。

逆向确认 `libnffr.so` 内部存在 `ffrObject::ffrEnablePackedModeExpandChildFlow(char)`。开启后，已验证 Reader 会在同一公开树回调中返回：

```text
STRUCT_BEGIN(packet, FSDB_STRUCT_TYPE_PACKED_STRUCT)
VAR tag[3:0]
VAR payload[7:0]
STRUCT_END
```

当前实现只为 `Verdi_T-2022.06-SP2` 和 `Verdi_Y-2026.03-SP2` 使用已取证的入口偏移，并在回调结束后按字段顺序计算聚合宽度。没有调用 FDA 私有对象，也没有嵌入 libNPI 对象布局。

这是版本适配点，不是稳定 FFR ABI。后续 Reader 必须重新取证入口位置、入口前置条件、回调顺序和字段位序；不能把现有偏移复制到其他 EDA 版本。

## 6. libNPI 逆向得到的边界

已保存的调用链表明 NPI 不是简单的 Reader 转发层：

```text
npi_fsdb_sig_by_name
  → fda_file_t::get_sig_by_name
  → fda_get_sig_by_name

npi_fsdb_goto_time
  → fda_vch_leaf_t / fda_vch_partial_t / fda_vch_composite_t
  → ffrVCIterOne::ffrGotoXTag / ffrGetXTag

npi_fsdb_vct_value
  → fda_vct_value_skip_check
  → digital / real / string / composite conversion
```

`fda_vch_partial_t` 和 `fda_vch_composite_t` 说明位选、结构体和数组不能都当成一个普通 idcode。当前 FFR 实现只复现 vwave 命令实际需要的可观察语义，不把 FDA 私有类当作可链接 ABI。

## 7. 验证结果

已完成以下差分和回归：

| 矩阵 | 查询数 | 差异 |
|---|---:|---:|
| T-2022 普通位选/数组 | 202 | 0 |
| Y-2026 普通位选/数组 | 202 | 0 |
| T-2022 配套后端样本 | 1709 | 0 |
| Y-2026 配套后端样本 | 1709 | 0 |
| struct/二维数组/packed/string 专项 | 377 | 0 |

工程测试 `make test-vwave` 通过。v1.7 发布包已包含 `vwave`、`vsignal`、`vwave-npi-worker` 和 `vwave-ffr-worker`，归档校验通过。

## 8. 后续重构计划

按优先级推进：

1. **降低 packed 展开入口的版本耦合**：从“版本字符串 + 固定偏移”改为运行时 ELF 符号定位或受控的 SDK 适配表，并增加入口字节和回调结果校验；找不到匹配入口时明确降级为父总线视图。
2. **补充异常波形样本**：无初始 VC、同一时间多字段变化、dumpoff 后恢复、空文件和持续写入文件。
3. **评估 string 文本解码**：先确认 Reader raw VC 的所有权、长度和编码，再决定是否提供独立输出格式；不改变当前 NPI 兼容语义。
4. **按实际需求扩展动态数组、queue、class 和事务属性**，每类能力单独取证，不与 packed struct 混为一个“大而全”版本。
5. **做生产规模性能验收**：打开耗时、首次查询耗时、多个 cursor 并发查询、内存占用和 Reader 版本切换。

## 9. 证据与复现

- [packed struct 差分结果](../reverse_analysis/evidence/ffr_support/support_packed_after_fix.json)
- [packed struct 入口取证](../reverse_analysis/evidence/ffr_support/packed_expand_probe.txt)
- [剩余差异闭环记录](../reverse_analysis/evidence/ffr_support/remaining_diff_after_fix.txt)
- [2022/2026 位选矩阵](../reverse_analysis/evidence/ffr_support/expanded2022_queries.json)、[扩展查询](../reverse_analysis/evidence/ffr_support/expanded_queries.json)
- [NPI 调用链与反汇编](../reverse_analysis/evidence/ffr_support/npi_calls.json)
- [差分脚本](../reverse_analysis/scripts/analyze_ffr_support.py)
- [专项 SV 样本](../reverse_analysis/probes/ffr_support_fixture.sv)、[Reader 探针](../reverse_analysis/probes/ffr_support_probe.cpp)

专项查询可复现为：

```bash
python3 reverse_analysis/scripts/analyze_ffr_support.py \
  --support-fixture \
  --sample build/ffr_support_analysis/support.fsdb \
  --sdk /opt/Synopsys/verdi/Y-2026.03-SP2 \
  --output build/ffr_support_analysis/support_queries.json
```

完整 VCS 日志和临时差分结果保存在忽略的 `build/ffr_support_analysis/`；厂商库和 SDK 头文件不纳入工程。
