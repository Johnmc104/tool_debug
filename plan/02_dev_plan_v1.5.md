# tool_wave v1.5 开发计划

基于 v1.4 工程评估（plan/01_assessment_tool_wave_v1.4.md），制定下阶段开发计划。

---

## 1. 版本目标

**v1.5 核心主题：输出可控性 + 批量操作**

让两个工具的返回数据在大设计场景下对 AI agent 友好——可截断、可压缩、可批量。重点解决评估中发现的 42 KB fanout、信号路径冗余、N 次串行调用等问题。

---

## 2. 优先级划分

### P0 — 必须完成（直接影响 AI agent 可用性）

| 编号 | 项目 | 工具 | 预估工作量 |
|------|------|------|-----------|
| P0-1 | vsignal --compact 模式 | vsignal | 1 天 |
| P0-2 | vsignal --limit N 截断 | vsignal | 0.5 天 |
| P0-3 | vsignal 批量查询（-s 重复 / -f 文件） | vsignal | 1.5 天 |
| P0-4 | vwave 多信号 range 查询 | vwave | 1 天 |

### P1 — 应该完成（提升体验和健壮性）

| 编号 | 项目 | 工具 | 预估工作量 |
|------|------|------|-----------|
| P1-1 | 错误诊断增强（解析日志提取根因） | vsignal | 0.5 天 |
| P1-2 | vsignal info 丰富（顶层模块名、实例数） | vsignal | 0.5 天 |
| P1-3 | vwave find 分页（--offset + --limit） | vwave | 0.5 天 |
| P1-4 | LD_LIBRARY_PATH 自动补全或启动前检测 | vsignal | 0.5 天 |

### P2 — 可以做（长线优化）

| 编号 | 项目 | 工具 | 预估工作量 |
|------|------|------|-----------|
| P2-1 | 信号路径前缀提取（prefix + 相对路径） | 两工具 | 1 天 |
| P2-2 | vwave signals --recursive（递归列信号） | vwave | 0.5 天 |
| P2-3 | vsignal trace --depth N（递归追踪深度） | vsignal | 1 天 |

---

## 3. P0 详细设计

### P0-1：vsignal --compact 模式

**目标**：对齐 vwave 的 compact 实现，减少 vsignal 输出的字符量。

**变更范围**：
- `src_vsignal/main.cpp`：解析 `--compact` / `-c` 参数，传入请求 params
- `src_vsignal/server/server_core.h`：所有 `handle_*` 函数读取 compact 参数
- `nl_handle_to_json()`：compact 模式下只输出 `name`（叶节点名），省略 `full_name`、`def_name`、`cell_type` 等元数据

**预期效果**：

当前 fanout 单条：
```json
{"type":"instance","name":"cm0_core_ctl(@1):Always1#Always0:685:699:Reg",
 "full_name":"tb_top.top_inst.u_digit_top.u_system.u_cortexm0integration.u_top.u_sys.u_core.u_ctl.cm0_core_ctl(@1):Always1#Always0:685:699:Reg"}
```

Compact 模式：
```json
{"name":"u_ctl.cm0_core_ctl(@1):Always1#Always0:685:699:Reg"}
```

预估减少率：60-70%（主要省略 full_name 的共同前缀部分）。

### P0-2：vsignal --limit N

**目标**：限制 fanin/fanout/load/driver 结果数量，避免大扇出信号（如时钟）的结果淹没 AI context。

**变更范围**：
- `src_vsignal/main.cpp`：解析 `--limit` / `-l` 参数（默认无限制）
- `src_vsignal/server/server_core.h`：所有 `handle_*` 函数在构建结果数组时检查 limit
- 返回中增加 `total` 和 `returned` 字段（与 vwave range 查询一致）

**JSON 输出变化**：
```json
{"signal":"HCLK","total":173,"returned":20,"fanout":[...前 20 条...]}
```

### P0-3：vsignal 批量查询

**目标**：减少 AI agent 的调用轮次。对 N 个信号做同一操作时从 N 次调用降为 1 次。

**接口设计**：
```bash
# -s 重复模式
vsignal driver -s top.sig_a -s top.sig_b -s top.sig_c --json

# -f 文件模式
vsignal fanin -f signals.txt --json
```

**变更范围**：
- `src_vsignal/main.cpp`：解析 `-s` 重复参数和 `-f` 文件参数
- `src_vsignal/common/protocol.h`：增加 `BATCH_TRACE_DRIVER` 等批量命令，或复用现有命令传入 signals 数组
- `src_vsignal/server/server_core.h`：batch 版本的 handler，内部循环调用 NPI 并聚合结果

**JSON 输出**：
```json
{"status":"ok","data":{"results":[
  {"signal":"top.sig_a","count":2,"drivers":[...]},
  {"signal":"top.sig_b","count":1,"drivers":[...]},
  {"signal":"top.sig_c","error":"SIGNAL_NOT_FOUND"}
]}}
```

部分信号失败不影响其他信号的结果返回。

### P0-4：vwave 多信号 range 查询

**目标**：`get -b -e` 支持多信号，一次返回多个信号在同一时间窗口的变化。

**当前限制**：range 模式仅支持单信号，多信号时打印 warning 并只处理第一个。

**变更范围**：
- `src_vwave/main.cpp`：去掉单信号限制的 warning
- `src_vwave/server/server_core.h`：`handle_get_value_between` 支持 signals 数组，循环读取每个信号的 vc

**JSON 输出**：
```json
{"status":"ok","data":{"begin":0,"end":100000,"signals":[
  {"signal":"clk","total_changes":10,"changes":[...]},
  {"signal":"rstn","total_changes":2,"changes":[...]}
]}}
```

每个信号独立受 `--limit` 控制。

---

## 4. P1 详细设计

### P1-1：错误诊断增强

**目标**：当 vsignal server 启动失败时，父进程自动读取 server log 的最后几行，提取关键错误信息，直接在 CLI 中展示，而非让用户手动 `cat` 日志。

**实现方式**：
在 `cmd_open` 的 "Server process exited unexpectedly" 分支中，读取 log 文件最后 10 行，用关键词匹配提取错误信息：
- `cannot open shared object file` → 提示 LD_LIBRARY_PATH
- `Failed to find elabDB` → 提示 VCS 编译需加 -kdb
- `license` → 提示检查 License

### P1-2：vsignal info 丰富

当前 `vsignal info` 只返回 `design_source` 和 `pid`。增加：
- 顶层模块名
- 设计加载时的参数

### P1-3：vwave find 分页

增加 `--offset` 和 `--limit` 参数：
```bash
vwave find "*gpio*" --limit 20 --offset 40 --json
```

返回增加 `total`、`offset`、`returned` 字段。

### P1-4：LD_LIBRARY_PATH 自动检测

vsignal 启动时检查 `VERDI_HOME` 环境变量是否存在，以及必要的 `.so` 文件是否可达。如果不可达，在 fork 前输出明确的环境配置提示。

---

## 5. 实施顺序

```
Week 1:
  P0-1  vsignal --compact        (参考 vwave compact 实现)
  P0-2  vsignal --limit          (与 compact 同步测试)

Week 2:
  P0-3  vsignal 批量查询         (-s 重复 + -f 文件)
  P0-4  vwave 多信号 range       (扩展现有 get_value_between)

Week 3:
  P1-1  错误诊断增强
  P1-2  vsignal info 丰富
  P1-3  vwave find 分页
  P1-4  LD_LIBRARY_PATH 检测

验证：
  - M0V1 sim_pre 全功能回归
  - 跨版本验证（T-2022.06-SP2 / W-2024.09 / Y-2026.03）
  - AI agent 端到端工作流测试（Claude Code tool-wave skill）
```

---

## 6. 验收标准

### 功能验收

| 项目 | 验收条件 |
|------|---------|
| vsignal --compact | fanout 输出减少 ≥ 60% |
| vsignal --limit | 返回条数不超过 N，total 字段准确 |
| vsignal 批量 | -s 重复 3+ 信号正常返回，部分失败不影响其他 |
| vwave 多信号 range | 3 信号 range 查询在单次返回中包含所有结果 |
| 错误诊断 | 缺 .so / 缺 KDB / 缺 license 各场景均给出明确提示 |

### 性能验收

| 指标 | 目标 |
|------|------|
| vsignal fanout（173 条、compact + limit 20） | 响应 < 2 KB |
| vsignal 批量 driver（10 个信号） | 响应时间 < 单次 × 1.5（不应线性增长 10 倍） |
| vwave 3 信号 range（limit 100 each） | 响应 < 10 KB |

### 兼容性验收

| 场景 | 要求 |
|------|------|
| Verdi T-2022.06-SP2 | vwave + vsignal 全功能正常 |
| Verdi W-2024.09 | vwave + vsignal 全功能正常 |
| Verdi Y-2026.03 | vwave + vsignal 全功能正常 |
| 不设置 LD_LIBRARY_PATH 时 | vsignal 给出明确错误提示而非 segfault |

---

## 7. 风险与依赖

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| NPI batch API 不存在 | 批量查询需逐个调用 NPI，性能增益有限 | server 端循环调用，至少减少客户端 N 次 fork |
| compact 路径截断可能丢失定位信息 | AI agent 无法用短名反向定位 | compact 模式仍保留 full_name，只是缩短非关键字段 |
| 多信号 range 内存压力 | 3 信号 × 1000 变化 = 较大临时缓冲 | 每个信号独立受 limit 控制 |
| 跨版本 NPI API 差异 | 旧版本可能缺少某些 NPI 函数 | 编译时用旧版本的头文件/库做最低兼容性保证 |
