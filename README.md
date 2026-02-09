# vwave — FSDB Waveform Reader

基于 Synopsys Verdi NPI 接口的命令行波形读取工具。单一可执行文件，内置驻留式后台服务，首次加载后续查询无需重复指定文件路径。

## 快速开始

```bash
# 编译
make

# 加载波形
vwave open test/tb_top.fsdb

# 查询（自动检测运行中的服务，无需指定文件）
vwave info
vwave scopes
vwave scopes tb
vwave signals tb.intf
vwave get-value -s tb.intf.clk -t 500000
vwave get-value -s tb.intf.paddr -t 1500000 -r hex
vwave get-value -s tb.intf.clk -b 0 -e 100000

# 批量读取
vwave get-value -f signals.txt -t 1000 -r hex

# 关闭波形
vwave close
```

## 核心特性

| 特性 | 说明 |
|------|------|
| **单一二进制** | `vwave` 同时包含后台服务和客户端逻辑，无需单独启动服务 |
| **自动检测** | 首次 `open` 后，后续命令自动从 CWD 向上搜索 `.wave_run/` 定位服务 |
| **波形切换** | `vwave open other.fsdb` 自动关闭旧波形并加载新文件 |
| **常驻加速** | NPI 加载一次，后续查询走 Unix Socket 通信，毫秒级响应 |
| **JSON 输出** | 所有命令支持 `--json` 输出，便于脚本集成 |

## 命令参考

### 波形生命周期

```bash
vwave open <file.fsdb>      # 加载波形（启动后台服务）
vwave close                  # 关闭波形（停止服务）
vwave status                 # 查看服务状态
```

### 文件与层次查询

```bash
vwave info                              # FSDB 文件信息（时间范围、版本等）
vwave scopes                            # 列出顶层层次
vwave scopes <path>                     # 列出子层次
vwave signals <path>                    # 列出指定层次下的信号
vwave signal-info <name>                # 查看信号详情（位宽、方向）
```

### 信号值读取

```bash
# 单信号 @ 时间点
vwave get-value -s <signal> -t <time>

# 多信号 @ 时间点
vwave get-value -s <sig1> -s <sig2> -t <time>

# 信号列表文件 @ 时间点
vwave get-value -f <signal_file> -t <time>

# 单信号 @ 时间范围（返回所有变化点）
vwave get-value -s <signal> -b <begin> -e <end>

# 指定进制：bin(默认), hex, oct, dec
vwave get-value -s <signal> -t <time> -r hex
```

### 选项缩写

| 长格式 | 缩写 | 说明 |
|--------|------|------|
| `--signal` | `-s` | 信号全路径 |
| `--signal-file` | `-f` | 信号列表文件 |
| `--time` | `-t` | 时间点 |
| `--begin` | `-b` | 范围起始 |
| `--end` | `-e` | 范围结束 |
| `--radix` | `-r` | 进制格式 |
| `--json` | | JSON 输出 |
| `--fsdb` | | 显式指定波形路径（跳过自动检测） |

## 工程结构

```
tool_wave/
├── Makefile                        构建系统
├── README.md                       本文件
├── spec.md                         需求规格文档
├── src/
│   ├── main.cpp                    入口：CLI 解析、fork 服务、客户端调度
│   ├── common/
│   │   ├── protocol.h              JSON 协议定义、命令字、错误码
│   │   ├── json_parser.h           轻量 JSON 解析器（零依赖）
│   │   └── run_dir.h               运行时目录管理 + 自动检测
│   ├── server/
│   │   └── server_core.h           NPI 服务逻辑（FSDB 操作 + 请求分发）
│   └── client/
│       └── client_core.h           客户端逻辑（Socket 通信 + 输出格式化）
├── test/
│   ├── tb_top.fsdb                 测试波形文件
│   └── run_test.sh                 自动化测试脚本（62 项测试）
└── build/
    └── bin/
        └── vwave                   可执行文件
```

### 模块职责

| 模块 | 文件 | NPI 依赖 | 职责 |
|------|------|----------|------|
| **入口** | `main.cpp` | 间接 | CLI 参数解析、`open`/`close` 命令处理、fork 管理 |
| **协议** | `protocol.h` | 无 | `JsonObject` 构建器、命令字常量、错误码、响应封装 |
| **解析** | `json_parser.h` | 无 | 最小 JSON 解析器（string/int/array） |
| **运行目录** | `run_dir.h` | 无 | `.wave_run/` 路径管理、PID/Socket/Log/FSDB 持久化、自动检测 |
| **服务** | `server_core.h` | 是 | NPI 初始化、FSDB 加载、所有查询 handler、事件循环 |
| **客户端** | `client_core.h` | 无 | UDS 通信、JSON 请求构建、信号文件读取、输出格式化 |

## 运行时文件

所有运行时文件存放在 **FSDB 文件所在目录** 下的 `.wave_run/` 子目录：

```
<fsdb_dir>/.wave_run/
├── wave_server.pid          服务 PID（防止重复启动）
├── wave_server.sock         Unix Domain Socket
├── wave_server.log          服务日志（daemon stdout/stderr）
└── fsdb_path                已加载的 FSDB 绝对路径（用于自动检测）
```

- `open` 时自动创建，`close` 时清理（日志保留）
- PID 文件用于检测服务是否存活
- `fsdb_path` 文件使自动检测机制能找回关联的 FSDB 路径

## 自动检测机制

后续命令不指定 `--fsdb` 时，`vwave` 从当前工作目录向上逐级搜索 `.wave_run/` 目录：

1. 找到 `.wave_run/wave_server.pid`
2. 检查 PID 进程是否存活
3. 若存活，使用该目录的 socket 和 fsdb_path
4. 若未找到，提示使用 `vwave open` 先加载波形

## 依赖

- Synopsys Verdi NPI (`VERDI_HOME` 环境变量，默认 `/opt/Synopsys/verdi/T-2022.06-SP2`)
- GCC 9+ (C++14)
- Linux (Unix Domain Socket, fork/setsid)

## 测试

```bash
# 运行全部测试（62 项）
bash test/run_test.sh
```

测试覆盖：
- T1~T2: 波形加载、重复加载（幂等）
- T3: 自动检测（从子目录定位服务）
- T4~T5: 服务状态、文件信息
- T6~T8: 层次结构、信号列表、信号详情
- T9~T11: 单/多信号取值、信号文件批量读取
- T12: 时间范围取值（变化点列表）
- T13: 进制格式（bin/oct/dec/hex）
- T14: 错误处理（不存在的信号/层次）
- T15: 关闭 + 重启
- T16: 波形切换
