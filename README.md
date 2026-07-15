# tool_wave — FSDB 波形读取 & 网表信号追踪工具集

基于 Synopsys Verdi NPI 接口的命令行工具集，包含两个独立二进制：

| 工具 | 用途 | 数据源 | 运行时目录 |
|------|------|--------|------------|
| **vwave** | FSDB 波形值读取 | `.fsdb` 波形文件 | `.vtool/wave_run/` |
| **vsignal** | 网表信号驱动/负载追踪 | VCS KDB 数据库 或 RTL 源文件 | `.vtool/vsignal_run/` |

两工具独立运行但共享统一的 **C/S 架构、通信协议、事件循环机制**，支持同时运行互不干扰。

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

### Get-value 选项（详见快速开始中的实际用法）

| 长格式 | 缩写 | 说明 |
|--------|------|------|
| `--signal` | `-s` | 信号全路径（可重复） |
| `--signal-file` | `-f` | 信号列表文件 |
| `--time` | `-t` | 时间点 |
| `--begin` | `-b` | 范围起始 |
| `--end` | `-e` | 范围结束 |
| `--radix` | `-r` | 进制格式：bin/hex/oct/dec |

---

## vsignal — 网表信号追踪

### 前置准备：KDB 数据库

```bash
# VCS 编译时加 -kdb 生成 KDB 数据库
vcs -sverilog -kdb design.v -o simv
# 生成 simv.daidir/kdb.elab++
```

### 追踪选项（详见快速开始中的实际用法）

| 选项 | 适用命令 | 说明 |
|------|----------|------|
| `--assign-cell` | driver, load, trace | 穿透 assign 单元 |
| `--pass-mod` | driver, load | 穿透模块边界 |
| `--stop-at-pin` | fanin, fanout | 在引脚处停止 |
| `--report-primary-port` | fanin, fanout | 报告顶层端口 |
| `--scope <name>` | fanin, fanout | 限定搜索范围 |
| `--level high\|low` | conn | 连接层级（默认 high） |

### NPI L1 API 映射

| 命令 | 底层接口 |
|------|----------|
| `driver` | `npi_nl_trace_driver()` |
| `load` | `npi_nl_trace_load()` |
| `fanin` | `npi_nl_sig_2_fanIn_reg_conn()` |
| `fanout` | `npi_nl_sig_2_fanOut_reg_conn()` |
| `trace` | `npi_nl_sig_2_sig_conn()` |
| `conn` | `npi_inst_port_2_high/low_conn_sig()` |

---

## 全局选项

两工具共有：

| 选项 | 说明 |
|------|------|
| `--json` | JSON 输出模式 |
| `--run-dir <path>` | 覆盖运行时目录路径 |
| `-h, --help` | 显示帮助信息 |

---

## 工程结构

```
tool_wave/
├── Makefile                            统一构建系统
├── README.md                           本文件
├── spec.md                             需求规格文档
│
├── .github/skills/tool-wave/           ★ AI Agent Skill（单一入口）
│   ├── SKILL.md                        统一技能描述（自动发现入口）
│   ├── references/
│   │   ├── vwave.md                    vwave 命令详细参考
│   │   └── vsignal.md                  vsignal 命令详细参考
│   └── scripts/
│       └── check-tools.sh             环境检查脚本
│
├── doc/
│   ├── analysis_debug_ai_support.md    调试场景与 AI 支持分析
│   └── proposal_driver_load_trace.md   vsignal 技术方案
│
├── src_common/                         ★ 共享库（tw:: 命名空间）
│   ├── json.h                          统一 JSON 对象 + 解析器（含 bool）
│   ├── protocol.h                      通用响应编码、错误码
│   ├── run_dir.h                       参数化运行时目录管理（.vtool/子目录）
│   ├── client.h                        RAII fd、EINTR 安全通信、请求生成
│   └── server_loop.h                   事件循环（1h 空闲超时、per-client 超时）
│
├── src_vwave/                          vwave 源码（薄封装层）
│   ├── main.cpp                        CLI 解析、fork 服务、命令分发
│   ├── common/                         (◆ 转发至 tw:: + vwave 特定命令字)
│   ├── server/server_core.h            NPI FSDB 处理 + 事件循环
│   └── client/client_core.h            (◆ 转发至 tw::client)
│
├── src_vsignal/                        vsignal 源码（薄封装层）
│   ├── main.cpp                        CLI 解析、fork 服务、命令分发
│   ├── common/                         (◆ 转发至 tw:: + vsignal 特定命令字)
│   ├── server/server_core.h            NPI L1 追踪 + 事件循环
│   └── client/client_core.h            (◆ 转发至 tw::client)
│
├── test_vwave/                         vwave 测试（62 项）
│   ├── tb_top.fsdb                     测试波形文件
│   └── run_test.sh                     自动化测试脚本
│
├── test_vsignal/                       vsignal 测试（27 项）
│   ├── example.v                       测试 RTL 设计
│   └── run_test.sh                     自动化测试脚本（含 VCS 编译）
│
└── build/
    ├── include/tw/ → src_common/       (Makefile 自动建立符号链接)
    └── bin/
        ├── vwave                       可执行文件
        └── vsignal                     可执行文件
```

**图例**: ◆ = 转发至共享库 | ★ = 新增共享库

---

## 架构设计

### 模式

```
┌──────────────────┐  fork   ┌──────────────────┐
│  CLI (client)    │ ─────── │ Server (daemon)  │
│                  │  UDS    │                  │
│ • 参数解析       │◄────────│ • NPI 初始化      │
│ • 请求构建       │ Socket  │ • 数据加载        │
│ • 输出格式化     │         │ • 命令处理        │
└──────────────────┘         │ • 事件循环        │
                             └──────────────────┘
```

### 关键特性

**架构与可靠性**
- **单一二进制**: server + client 合并，`open` 时 fork daemon，独立运行互不干扰
- **CWD 驱动**: 运行时文件存储于当前目录（`.vtool/wave_run/` / `.vtool/vsignal_run/`），避免路径冲突
- **自动检测**: 后续命令自动搜索 CWD 及父目录，无需重复指定参数

**健壮性保障**
- **1h 空闲超时**: 驻留服务 1 小时无操作自动关闭，防止僵尸进程
- **RAII 文件描述符**: ScopedFd 保证所有退出路径均关闭，防止描述符泄漏
- **EINTR 安全**: send/recv 自动重试，处理信号中断
- **OOM 防护**: read_line 限制单行 4MB，防止恶意输入触发内存爆炸
- **按客户端超时**: SO_RCVTIMEO/SO_SNDTIMEO (vwave 30s, vsignal 60s)，防止挂死
- **信号处理**: sigaction() 替代 signal()，避免平台差异，SIGPIPE 防护
- **空指针检查**: NPI 句柄验证，npi_str() 安全包装

**代码复用**
- **src_common 统一库**: 951 行共享代码（JSON、协议、通信、事件循环）
- **消除重复**: 净减少 ~195 行代码（1542 删除，1347 添加）
- **薄封装**: vwave/vsignal 各模块 15-75 行，纯转发至 tw:: 命名空间
- **易维护**: 错误修复、性能优化、安全补丁仅需修改 src_common

**与 AI 集成**
- **JSON 模式**: 所有命令支持 `--json` 输出，便于脚本及 Agent 集成

---

## 依赖

- Synopsys Verdi NPI — 需要在 shell 环境中正确设置以下变量：
  ```bash
  export VERDI_HOME=/path/to/verdi           # Verdi 安装根目录
  export LD_LIBRARY_PATH=$VERDI_HOME/share/NPI/lib/linux64:$LD_LIBRARY_PATH
  ```
  如使用 module 系统则 `module load synopsys/verdi` 即可自动完成。
- GCC 9+ (C++14)
- Linux (Unix Domain Socket, fork/setsid)
- VCS（仅 vsignal 测试需要，用于生成 KDB 数据库）
