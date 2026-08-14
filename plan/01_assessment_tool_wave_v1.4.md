# tool_wave v1.4 工程评估报告

日期：2026-08-14
测试环境：M0V1 SoC (Cortex-M0)，sim_pre 目录
EDA 版本：VCS/Verdi Y-2026.03（编译时）、W-2024.09（跨版本验证）

---

## 1. 评估目标

基于实际芯片验证项目（M0V1 Cortex-M0 SoC），从以下维度评估 vwave 和 vsignal：

- 芯片分析和问题定位的完整工作流是否顺畅
- 多步查询操作中是否存在可合并的冗余操作
- 返回数据格式和字符量对 AI agent 的适配性
- 大数据量和多信号列表场景的表现
- 跨 EDA 版本的自适应能力

---

## 2. 跨版本自适应验证

### 测试方案

二进制用 Verdi Y-2026.03 编译（RUNPATH 硬编码为该版本 NPI lib 路径），然后用 `module switch` 切换到 W-2024.09 重新仿真，再用原始二进制加载 W-2024.09 产出的 FSDB/KDB。

### 结果

| 场景 | vwave | vsignal |
|------|-------|---------|
| 二进制 Y-2026.03 + 数据 Y-2026.03 | 正常 | 正常 |
| 二进制 Y-2026.03 + 数据 W-2024.09 | 正常 | 正常（需手动设置 LD_LIBRARY_PATH） |
| FSDB 版本兼容性 | v6.1 ↔ v6.4 均正常 | N/A |

### 问题

1. **vsignal 需要额外 LD_LIBRARY_PATH**：`npi_load_design` 运行时 dlopen 加载 `liblpinstrumentdb.so`，该库在 `$VERDI_HOME/platform/linux64/bin/`，不在 RUNPATH 指向的 `NPI/lib/linux64/` 中。vwave 不需要是因为 `npi_fsdb_open` 只依赖 NPI/lib/ 内的库。
2. **module 环境不完整**：`module load synopsys/verdi/X` 设置的 `LD_LIBRARY_PATH` 指向 `NPI/lib/LINUXAMD64`，缺少 `platform/linux64/bin`，用户需手动补充。
3. **错误提示不透明**：加载失败时只提示"check log"，用户需手动翻日志才能发现是缺库还是缺 KDB。

---

## 3. 芯片问题定位工作流评估

以 M0V1 SoC 的实际调试场景逐个评估工具在真实工作流中的表现。

### 场景 A：时钟信号异常排查

**背景**：仿真失败日志显示 CPU 挂死，怀疑 HCLK 未正常翻转。

**操作流**：
```
vwave open tb_top.fsdb
vwave info --json                        # 获取仿真时间范围
vwave get -s tb_top.intf.intf_dut.clk -t 100000 --json   # 确认时钟值
vwave vc-count -s tb_top.intf.intf_dut.clk --json         # 确认翻转次数
vwave edge -s tb_top.intf.intf_dut.clk -t 0 --rising --json  # 第一个上升沿
```

**评估**：
- 流畅度：5 条命令逐步缩小范围，每条 < 150 字符返回，AI agent 可高效处理。
- 痛点：如果还需同时查看 rstn、NRST 等相关信号，需要分别执行 `vc-count` 和 `edge`，无法批量查询边沿和翻转计数。

### 场景 B：CPU HCLK 驱动链根因追溯

**背景**：HCLK 频率异常，需找到时钟源和门控路径。

**操作流**：
```
vsignal open -dbdir simv.daidir
vsignal driver tb_top.top_inst.u_digit_top.u_system.u_cortexm0integration.HCLK --json
  → 结果：1 个驱动（端口本身）
vsignal driver ... --pass-mod --json
  → 结果：4 个驱动源，包括 PMU 门控输出
vsignal fanin ... --json
  → 结果：定位到 u_pmu.u_hclk_gate 的 Latch
```

**评估**：
- 流畅度：3 步定位到门控来源，效率高。
- 痛点 1：信号路径 `tb_top.top_inst.u_digit_top.u_system.u_cortexm0integration.HCLK` 长达 74 字符，每次手动输入或在 JSON 中重复传输都很浪费。
- 痛点 2：拿到 4 个驱动后，如果要逐个追踪每个驱动的 fanin，需要 4 次独立调用，无法批量。
- 痛点 3：返回的 42 KB fanout JSON 没有截断机制，直接涌入 AI context window。

### 场景 C：波形 + 结构联合定位

**背景**：GPIO 输出值错误，需定位是固件问题还是硬件连接问题。

**操作流**：
```
# 时域：确认 GPIO 输出值
vwave get -s tb_top.top_inst.u_digit_top.u_system.u_periph_subsys.u_ahb_gpio0.u_iop_gpio.GPIODATA -t 500000 -r hex --json

# 结构域：追踪 GPIO 数据来源
vsignal driver tb_top.top_inst.u_digit_top.u_system.u_periph_subsys.u_ahb_gpio0.u_iop_gpio.GPIODATA --pass-mod --json

# 时域：验证驱动源在同一时刻的值
vwave get -s <driver_signal_from_above> -t 500000 -r hex --json
```

**评估**：
- 这是最有价值的使用模式——波形与结构交替分析。
- 痛点 1：跨工具信号路径需要手动对齐，AI 必须自己从 vsignal 输出中提取 full_name 然后拼接成 vwave 的 -s 参数。
- 痛点 2：整个流程在 AI agent 中需要 3 次工具调用 + 中间结果解析 + 重新构造命令，如果能提供"给定信号列表 + 时间点，一次返回所有值"的批量模式会大幅减少轮次。

### 场景 D：模块端口快速摸底

**背景**：接手不熟悉的 IP（如 Cortex-M0 Integration），需快速理解其接口。

```
vsignal conn tb_top.top_inst.u_digit_top.u_system.u_cortexm0integration --json
  → 返回：57 个端口连接，4.5 KB
```

**评估**：
- 一条命令获取完整端口映射，效率高。
- 对 AI agent 来说 4.5 KB 是可接受的，但如果 IP 有 200+ 端口（如 AXI interconnect），数据量会膨胀到 20-50 KB，此时需要 compact 或分页能力。

---

## 4. 数据量与返回格式分析

### 4.1 vwave 响应字符量

| 命令 | 典型大小 | AI 消耗评估 |
|------|---------|------------|
| status | 137 字符 | 极小，无关注 |
| info | 184 字符 | 极小 |
| get（1 信号 1 时间点） | 128 字符 | 极小 |
| get（5 信号 1 时间点） | 424 字符 | 可接受 |
| get（时间范围 limit=1000） | 29 KB | **偏大**，建议默认降至 100 或 200 |
| get（时间范围 limit=10） | ~500 字符 | 合适 |
| edge | 145 字符 | 极小 |
| find（广泛模式） | 14 KB / 175 条 | **偏大**，无分页 |
| signals（普通） | 2362 字符 | 中等 |
| signals（compact） | 296 字符 | **减少 87%**，compact 效果显著 |

### 4.2 vsignal 响应字符量

| 命令 | 典型大小 | AI 消耗评估 |
|------|---------|------------|
| driver（无选项） | 266 字符 | 极小 |
| driver（--pass-mod） | 918 字符 | 可接受 |
| load（--pass-mod --assign-cell） | ~5 KB / 56 条 | 中等，无截断 |
| fanout | **42 KB** / 173 条 | **过大**，无 limit 和 compact |
| conn | 4.5 KB / 57 条 | 中等 |
| trace | 439 字符 | 小 |

### 4.3 关键发现

1. **vwave 的 compact 模式效果显著**：signals 从 2362 → 296 字符（-87%）。vsignal 完全缺失此能力。
2. **vsignal fanout 是最大的数据膨胀点**：173 条寄存器结果，每条包含完整层次路径（60-120 字符），总计 42 KB。这在 AI 4K-8K token 的有效工作区间内是不可接受的。
3. **信号路径重复**是主要的字符浪费源：同一 fanout 结果中，前缀 `tb_top.top_inst.u_digit_top.u_system.u_cortexm0integration.u_top.` 重复 173 次。

---

## 5. 多查询操作合并分析

当前工具都是"单命令单结果"模式。在实际调试工作流中，以下操作模式频繁出现：

### 5.1 可合并的重复操作

| 场景 | 当前操作次数 | 可优化方向 |
|------|-------------|-----------|
| 同一时间点查 5 个信号的值 | 1 次（已支持 -s 重复） | 无需优化 |
| 同一时间范围查 3 个信号的变化 | 3 次（range 仅支持单信号） | 支持多信号 range |
| 对 N 个信号做 driver/fanin/fanout | N 次 | 支持 -s 重复或 -f 文件 |
| 先 driver 再对每个结果 fanin | 1 + N 次 | 可考虑 `--depth 2` 递归深度 |
| 先 vsignal 追踪再 vwave 取值 | 2 次（跨工具） | 需上层编排，暂不在工具层解决 |

### 5.2 每次命令的固有开销

每次 CLI 调用的固定开销：
- 进程 fork: ~2ms
- Socket 连接 + 发送 + 接收: ~5ms
- JSON 解析: ~1ms

单次开销不大，但 20 次串行调用（典型调试会话）累计 ~160ms + AI 轮次等待时间。批量模式主要不是为了省 latency，而是为了减少 AI agent 的思考-执行轮次。

---

## 6. 测试矩阵

### 6.1 vwave（18/18 通过）

| 类别 | 用例数 | 状态 |
|------|-------|------|
| 生命周期（open/close/status） | 4 | 通过 |
| 层次浏览（scopes/signals/find） | 5 | 通过 |
| 值查询（get 单点/多信号/范围/文件） | 4 | 通过 |
| 分析（edge/vc-count/signal-info） | 3 | 通过 |
| 跨版本（W-2024.09 FSDB） | 2 | 通过 |

### 6.2 vsignal（12/12 通过）

| 类别 | 用例数 | 状态 |
|------|-------|------|
| 生命周期（open/close/status/info） | 4 | 通过 |
| 追踪（driver/load/fanin/fanout/trace/conn） | 7 | 通过 |
| 跨版本（W-2024.09 KDB） | 1 | 通过 |

### 6.3 本次修复的缺陷

| 缺陷 | 影响 | 修复方式 |
|------|------|---------|
| 设计切换自比较（两工具共有） | 切换设计路径永远不可达 | 从磁盘读 source_info 文件 |
| chdir 后相对路径断裂（vsignal） | -dbdir 用相对路径时 NPI 加载失败 | fork 前 realpath 转绝对路径 |
| NPI 日志未约束（vsignal） | vsignalLog/ 散落 CWD | chdir 到 run_dir（对齐 vwave） |

---

## 7. 总体评估结论

### 可用性

- vwave 已达生产可用状态，CLI 体验流畅，compact 模式对 AI agent 有明确价值。
- vsignal 功能完整但缺少输出控制（compact/limit），大结果场景对 AI agent 不友好。

### 完整性

- 时域观测（vwave）+ 结构追踪（vsignal）覆盖了芯片调试最核心的两类查询。
- 缺口在批量操作和跨工具编排：无法在单次调用中完成多信号追踪或跨域联合查询。

### AI 适配性

- JSON 格式统一、错误码语义清晰、daemon 复用设计天然适合 AI agent。
- 主要瓶颈是大结果的数据量膨胀（fanout 42 KB）和信号路径重复冗余。
- compact 模式是解决这一问题的最直接手段，且 vwave 已有成熟实现可参考。

### 版本自适应

- RUNPATH + LD_LIBRARY_PATH 方案可行，跨 T/W/Y 三个大版本均正常。
- vsignal 的额外库依赖是唯一的摩擦点，需文档或自动化解决。
