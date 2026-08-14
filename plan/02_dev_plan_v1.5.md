# tool_wave v1.5 开发计划

基于 v1.4 工程评估（plan/01_assessment_tool_wave_v1.4.md），制定下阶段开发计划。

---

## 1. 版本目标

**v1.5 核心主题：输出可控性 + 批量操作**

让两个工具的返回数据在大设计场景下对 AI agent 友好——可截断、可压缩、可批量。重点解决评估中发现的 42 KB fanout、信号路径冗余、N 次串行调用等问题。

---

## 2. 完成状态

### P0 — 全部完成

| 编号 | 项目 | 状态 | 实际效果 |
|------|------|------|---------|
| P0-1 | vsignal --compact 模式 | **已完成** | fanout 38KB → 11KB（-72%） |
| P0-2 | vsignal --limit N 截断 | **已完成** | compact+limit=5: 38KB → 467B（-99%） |
| P0-3 | vsignal 批量查询（-s / -f） | **已完成** | N 信号 1 次调用，部分失败不阻塞 |
| P0-4 | vwave 多信号 range 查询 | **已完成** | 3 信号 range 合并为单次 JSON 返回 |

### P1 — 部分完成

| 编号 | 项目 | 状态 | 说明 |
|------|------|------|------|
| P1-1 | 错误诊断增强 | 待开发 | |
| P1-2 | vsignal info 丰富 | 待开发 | |
| P1-3 | vwave find 分页 | 待开发 | |
| P1-4 | LD_LIBRARY_PATH 自动补全 | **已完成** | 从 VERDI_HOME 推导，无需手动设置 |

### 设计决策变更

评估实施过程中做出以下默认值调整（减少用户参数负担）：

| 项目 | 原设计 | 变更后 | 原因 |
|------|--------|--------|------|
| compact 模式 | 默认关闭，`--compact` 开启 | **默认开启**，`--full` 关闭 | AI agent 和 CLI 用户都更需要精简输出 |
| limit 默认值 | 无限制（0） | **默认 50** | 防止大扇出信号（如时钟）淹没 context |

### P2 — 待评估

| 编号 | 项目 | 工具 | 预估工作量 |
|------|------|------|-----------|
| P2-1 | 信号路径前缀提取（prefix + 相对路径） | 两工具 | 1 天 |
| P2-2 | vwave signals --recursive（递归列信号） | vwave | 0.5 天 |
| P2-3 | vsignal trace --depth N（递归追踪深度） | vsignal | 1 天 |

---

## 3. P0 实现总结

### P0-1 + P0-2：vsignal --compact + --limit

**实现方式**：
- `nl_handle_to_json()` 增加 `compact` 参数：compact 模式仅输出 `name` 字段
- `nl_hdl_vec_to_json()` 增加 `limit` 参数：截断数组，返回 `total` 和 `returned` 计数
- 6 个 handler（driver/load/fanin/fanout/trace/conn）统一读取 compact/limit 参数

**默认行为**：
```bash
vsignal fanout top.HCLK --json          # compact ON, limit=50（默认）
vsignal fanout top.HCLK --full --json   # 完整输出, limit=50
vsignal fanout top.HCLK --limit 0       # compact ON, 无限制
vsignal fanout top.HCLK --full -l 0     # 完整输出, 无限制（等同旧行为）
```

### P0-3：vsignal 批量查询

**实现方式**：客户端聚合——对每个信号发送独立请求到 server，聚合为 `{"results":[...]}` 数组。无需修改 server 协议。

```bash
vsignal driver -s sig1 -s sig2 -s sig3 --json     # -s 重复
vsignal fanin -f signals.txt --json                 # -f 文件
```

**部分失败处理**：单个信号查询失败（如 SIGNAL_NOT_FOUND）不影响其他信号的结果。

### P0-4：vwave 多信号 range 查询

**实现方式**：同 P0-3，客户端聚合多个 `get_value_between` 请求。

```bash
vwave get -s clk -s rstn -s nrst -b 0 -e 200000 --limit 5 --json
```

返回 `{"signals":[{signal, total_changes, changes}, ...]}` 结构。

### P1-4：LD_LIBRARY_PATH 自动补全

**实现方式**：`cmd_open` 中 fork 前检查 `VERDI_HOME` 环境变量，自动补全两个必需路径：
- `$VERDI_HOME/share/NPI/lib/linux64`（NPI 核心库）
- `$VERDI_HOME/platform/linux64/bin`（npi_load_design 运行时依赖）

如果 `VERDI_HOME` 未设置，输出明确 Warning 和 hint。

---

## 4. 测试验证

### 功能验证结果

| 验收条件 | 结果 |
|---------|------|
| vsignal compact fanout 减少 ≥ 60% | 实测 -72%（compact） / -99%（compact+limit=5） |
| vsignal limit 返回条数不超过 N | 通过（total=164, returned=50） |
| vsignal 批量 3+ 信号正常返回 | 通过（3 信号，含 1 个不存在的信号） |
| vwave 多信号 range 单次返回 | 通过（3 信号 range，每信号独立 limit） |
| LD_LIBRARY_PATH 无需手动设置 | 通过（unset LD_LIBRARY_PATH 后 open 成功） |

### 跨版本验证

| 场景 | 结果 |
|------|------|
| 二进制 Y-2026.03 + 数据 W-2024.09 | vwave 正常，vsignal 正常（自动补全 LD_LIBRARY_PATH） |

---

## 5. 下阶段计划（P1 剩余 + P2）

| 优先级 | 项目 | 说明 |
|--------|------|------|
| P1-1 | 错误诊断增强 | server 启动失败时解析 log 提取根因，直接在 CLI 展示 |
| P1-2 | vsignal info 丰富 | 增加顶层模块名、加载参数 |
| P1-3 | vwave find 分页 | --offset + --limit 分页查询 |
| P2-1 | 信号路径前缀提取 | 减少重复路径的字符量 |
| P2-2 | vwave signals --recursive | 递归列出子 scope 信号 |
| P2-3 | vsignal trace --depth N | 递归追踪深度控制 |
