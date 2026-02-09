# tool_debug — FSDB 波形读取 & 网表信号追踪工具集

基于 Synopsys Verdi NPI 接口的命令行工具集，包含两个独立二进制：

| 工具 | 用途 | 数据源 | 运行时目录 |
|------|------|--------|------------|
| **vwave** | FSDB 波形值读取 | `.fsdb` 波形文件 | `.wave_run/` |
| **vsignal** | 网表信号驱动/负载追踪 | VCS KDB 数据库 或 RTL 源文件 | `.vsignal_run/` |

两者共享相同的架构模式（fork 驻留服务 + UDS 通信），但功能完全独立，可同时运行互不干扰。

## 快速开始

```bash
# 编译全部
make

# ── vwave: 波形读取 ──
vwave open test_vwave/tb_top.fsdb
vwave info
vwave scopes
vwave signals tb.intf
vwave get-value -s tb.intf.clk -t 500000
vwave get-value -s tb.intf.paddr -t 1500000 -r hex
vwave close

# ── vsignal: 信号追踪 ──
vsignal open -dbdir simv.daidir         # 加载 VCS KDB 数据库
vsignal driver top.u_sub.data_out       # 追踪信号驱动源
vsignal load   top.data_in              # 追踪信号负载
vsignal fanin  top.data_out             # FanIn 寄存器连接
vsignal fanout top.clk                  # FanOut 寄存器连接
vsignal trace  top.data_in top.data_out # 信号间路径追踪
vsignal conn   top.u_sub                # 实例端口连接
vsignal close
```

## 编译 & 测试

```bash
make                 # 编译 vwave + vsignal
make vwave           # 仅编译 vwave
make vsignal         # 仅编译 vsignal
make clean           # 清理构建产物

make test            # 运行全部测试（vwave 62 项 + vsignal 27 项）
make test-vwave      # 仅运行 vwave 测试
make test-vsignal    # 仅运行 vsignal 测试
```

---

## vwave — FSDB 波形读取

### 功能

从 FSDB 波形文件中读取信号值、层次结构信息，支持单点/范围/批量查询。

### 命令参考

```bash
vwave open  <file.fsdb>                    # 加载波形（启动后台服务）
vwave close                                # 关闭波形（停止服务）
vwave status                               # 查看服务状态
vwave info                                 # FSDB 文件信息（时间范围、版本等）
vwave scopes  [<path>]                     # 列出层次作用域
vwave signals <path>                       # 列出指定层次下的信号
vwave signal-info <name>                   # 查看信号详情（位宽、方向）
vwave get-value [options]                  # 读取信号值
```

### Get-value 选项

| 长格式 | 缩写 | 说明 |
|--------|------|------|
| `--signal` | `-s` | 信号全路径（可重复） |
| `--signal-file` | `-f` | 信号列表文件 |
| `--time` | `-t` | 时间点 |
| `--begin` | `-b` | 范围起始 |
| `--end` | `-e` | 范围结束 |
| `--radix` | `-r` | 进制格式：bin/hex/oct/dec |

### 示例

```bash
# 单信号 @ 时间点
vwave get-value -s tb.intf.clk -t 500000

# 多信号 @ 时间点（hex 格式）
vwave get-value -s tb.intf.paddr -s tb.intf.pwdata -t 1500000 -r hex

# 信号列表文件 @ 时间点
vwave get-value -f signals.txt -t 1000 -r hex

# 单信号 @ 时间范围（返回所有变化点）
vwave get-value -s tb.intf.clk -b 0 -e 100000
```

---

## vsignal — 网表信号追踪

### 功能

基于 NPI 网表 L1 API，对 VCS 编译生成的 KDB 数据库进行静态信号追踪分析。

### KDB 数据库生成

```bash
# VCS 编译时加 -kdb 即可生成 KDB 数据库
vcs -sverilog -kdb design.v -o simv
# 生成 simv.daidir/kdb.elab++
```

### 命令参考

```bash
vsignal open  -dbdir <kdb_dir>             # 从 KDB 数据库加载设计
vsignal open  <source.v> [...]             # 从 RTL 源文件加载设计
vsignal close                              # 关闭设计
vsignal status                             # 查看服务状态
vsignal info                               # 查看设计信息

vsignal driver  <signal>                   # 追踪信号驱动源
vsignal load    <signal>                   # 追踪信号负载
vsignal fanin   <signal>                   # FanIn 寄存器连接
vsignal fanout  <signal>                   # FanOut 寄存器连接
vsignal trace   <from_sig> <to_sig>        # 信号间路径追踪
vsignal conn    <instance>                 # 实例端口连接
```

### 追踪选项

| 选项 | 适用命令 | 说明 |
|------|----------|------|
| `--assign-cell` | driver, load, trace | 穿透 assign 单元 |
| `--pass-mod` | driver, load | 穿透模块边界 |
| `--stop-at-pin` | fanin, fanout | 在引脚处停止 |
| `--report-primary-port` | fanin, fanout | 报告顶层端口 |
| `--scope <name>` | fanin, fanout | 限定搜索范围 |
| `--level high\|low` | conn | 连接层级（默认 high） |

### 底层 NPI L1 API 映射

| 命令 | NPI L1 函数 |
|------|-------------|
| `driver` | `npi_nl_trace_driver()` |
| `load` | `npi_nl_trace_load()` |
| `fanin` | `npi_nl_sig_2_fanIn_reg_conn()` |
| `fanout` | `npi_nl_sig_2_fanOut_reg_conn()` |
| `trace` | `npi_nl_sig_2_sig_conn()` |
| `conn` | `npi_inst_port_2_high/low_conn_sig()` |

---

## 工程结构

```
tool_wave/
├── Makefile                            统一构建系统
├── README.md                           本文件
├── spec.md                             需求规格文档
├── doc/
│   └── proposal_driver_load_trace.md   vsignal 技术方案
│
├── src_vwave/                          vwave 源码
│   ├── main.cpp                        入口：CLI 解析、fork 服务、命令分发
│   ├── common/
│   │   ├── protocol.h                  JSON 协议、命令字、错误码
│   │   ├── json_parser.h              轻量 JSON 解析器
│   │   └── run_dir.h                  .wave_run/ 运行时目录管理
│   ├── server/
│   │   └── server_core.h              NPI 服务（FSDB 操作 + 请求分发）
│   └── client/
│       └── client_core.h              UDS 通信 + 输出格式化
│
├── src_vsignal/                        vsignal 源码
│   ├── main.cpp                        入口：CLI 解析、fork 服务、命令分发
│   ├── common/
│   │   ├── protocol.h                  JSON 协议（追踪命令定义）
│   │   ├── json_parser.h              轻量 JSON 解析器（含 bool 支持）
│   │   └── run_dir.h                  .vsignal_run/ 运行时目录管理
│   ├── server/
│   │   └── server_core.h              NPI 服务（KDB 加载 + L1 追踪）
│   └── client/
│       └── client_core.h              UDS 通信 + 输出格式化
│
├── test_vwave/                         vwave 测试（62 项）
│   ├── tb_top.fsdb                     测试波形文件
│   └── run_test.sh                     自动化测试脚本
│
├── test_vsignal/                       vsignal 测试（27 项）
│   ├── example.v                       测试 RTL 设计
│   └── run_test.sh                     自动化测试脚本（含 VCS 编译）
│
└── build/bin/
    ├── vwave                           vwave 可执行文件
    └── vsignal                         vsignal 可执行文件
```

## 共享架构

两个工具采用相同的 C/S 架构模式：

```
┌─────────────────────┐     fork      ┌─────────────────────┐
│  CLI (client)       │ ───────────── │  Server (daemon)     │
│                     │               │                      │
│  参数解析           │    UDS        │  NPI 初始化          │
│  请求构建           │ ◄───────────► │  数据加载            │
│  输出格式化         │   Socket      │  命令处理            │
│                     │               │  事件循环            │
└─────────────────────┘               └─────────────────────┘

vwave:   加载 FSDB → 查询信号值/层次结构
vsignal: 加载 KDB  → 追踪驱动/负载/FanIn/FanOut/路径
```

### 关键设计

- **单一二进制**: 每个工具同时包含 server 和 client 代码，`open` 时 fork 出 daemon
- **CWD 绑定**: 运行时文件在当前目录下（`.wave_run/` / `.vsignal_run/`），避免路径冲突
- **自动检测**: 后续命令自动从 CWD 向上搜索运行时目录，无需重复指定参数
- **互不干扰**: 两工具使用不同的运行时目录和 socket 文件，可同时运行
- **JSON 模式**: 所有命令支持 `--json` 输出，便于脚本/AI Agent 集成

## 全局选项

两个工具共有的选项：

| 选项 | 说明 |
|------|------|
| `--json` | JSON 输出模式 |
| `--run-dir <path>` | 覆盖运行时目录路径 |
| `-h, --help` | 显示帮助信息 |

## 依赖

- Synopsys Verdi NPI (`VERDI_HOME`，默认 `/opt/Synopsys/verdi/T-2022.06-SP2`)
- GCC 9+ (C++14)
- Linux (Unix Domain Socket, fork/setsid)
- VCS（仅 vsignal 测试需要，用于生成 KDB 数据库）
