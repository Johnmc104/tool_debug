# vsignal — 信号驱动/负载追踪工具 技术方案

> 独立于 `vwave` 的新工具，通过 NPI 网表 API 实现信号连接追踪。

## 1. 需求概述

在数字设计调试中，追踪信号的驱动链（driver chain）和负载链（load chain）是核心需求：
- **驱动追踪（fan-in）**：给定信号 → 找到所有驱动它的寄存器/端口
- **负载追踪（fan-out）**：给定信号 → 找到所有被它驱动的寄存器/端口
- **信号路径追踪**：给定两个信号 → 找到它们之间的连接路径
- **端口连接查询**：给定实例 → 查看每个端口的上层/下层连接信号

## 2. 工具定位

| | `vwave` | `vsignal` |
|---|---------|-----------|
| **功能** | 波形值查询（时域） | 信号连接追踪（结构域） |
| **数据源** | FSDB 波形文件 | **KDB 数据库** 或 RTL 源码 |
| **NPI API** | `npi_fsdb_*` 系列 | `npi_nl_*` / L1 Connection 系列 |
| **二进制** | `vwave` | `vsignal`（新建） |
| **源码路径** | `src/` | `src_vsignal/`（独立） |

两个工具完全独立，各自拥有独立的源码、构建目标和运行时。

## 3. NPI API 可行性评估

### 3.1 关键验证：KDB 直接加载 ✅

**已实测验证**：`npi_load_design()` 支持 `-dbdir` 参数直接加载 VCS 编译生成的 KDB 数据库。

```bash
# VCS 编译生成 KDB
vcs -sverilog -kdb design.v -o simv
# → 生成 simv.daidir/kdb.elab++

# vsignal 直接加载 KDB（无需 RTL 源码）
./vsignal open -dbdir simv.daidir
```

实测结果（`/tmp/npi_kdb_test/`）：

| 加载方式 | 命令 | 编译步骤 | 结果 |
|----------|------|----------|------|
| RTL 源码 | `./test example.v` | 需要 Analyzing + Linking | ✅ 正确追踪 |
| **KDB 数据库** | `./test -dbdir simv.daidir` | **跳过**，直接加载 | ✅ 正确追踪，更快 |

KDB 存储了 VCS 编译后的完整网表信息，加载速度远快于从 RTL 源码重新编译。

### 3.2 设计数据来源

VCS 编译流程自然生成 KDB：

```
RTL源码 ──vcs -kdb──→ simv.daidir/kdb.elab++  ──npi_load_design──→ 网表数据
                      （已在仿真流程中生成）      （vsignal 直接加载）
```

不需要用户额外操作，仿真流程中 `vcs -kdb` 已经是标准做法。

### 3.3 可用 API 层次

| 层次 | 头文件 | 库 | 主要 API |
|------|--------|-----|----------|
| **底层 NL** | `npi_nl.h` | `libNPI.so` | `npi_nl_iterate(npiNlDriver/npiNlLoad, hdl)` + `npi_nl_scan()` |
| **高层 L1** | `npi_L1.h` | `libnpiL1.so` | `npi_nl_trace_driver()`, `npi_nl_sig_2_fanIn_reg_conn()` 等 |

### 3.4 L1 高级 API 一览

#### 直接驱动/负载追踪
```cpp
// 查找信号的直接驱动者（net/port/instport 级别）
int npi_nl_trace_driver(char* sigHierName, nlHdlVec_t& hdlVec,
                        int assignCell=0, int passMod=0);

// 查找信号的直接负载者
int npi_nl_trace_load(char* sigHierName, nlHdlVec_t& hdlVec,
                      int assignCell=0, int passMod=0);
```

#### 寄存器级 Fan-in / Fan-out（穿透组合逻辑）
```cpp
// 逆向追踪至驱动寄存器（穿透中间的组合逻辑和assign）
int npi_nl_sig_2_fanIn_reg_conn(char* sigHierName, nlHdlVec_t& hdlVec,
    bool isStopAtPin=false, bool isReportPrimaryPort=false,
    const char* scopeHierName=NULL);

// 正向追踪至负载寄存器
int npi_nl_sig_2_fanOut_reg_conn(char* sigHierName, nlHdlVec_t& hdlVec,
    bool isStopAtPin=false, bool isReportPrimaryPort=false,
    const char* scopeHierName=NULL);
```

#### 信号路径追踪
```cpp
// 查找两个信号之间的连接路径（BFS）
int npi_nl_sig_2_sig_conn(char* fromSigHierName, char* toSigHierName,
                          nlHdlVec_t& hdlVec, int assignCell=0);
```

#### 实例级连接
```cpp
// 查找信号连接到的模块实例 / 原语实例
int npi_nl_sig_2_mod_inst_conn(char* sigHierName, nlHdlVec_t& hdlVec, ...);
int npi_nl_sig_2_primitive_inst_conn(char* sigHierName, nlHdlVec_t& hdlVec, ...);

// 实例端口到高层/低层连接信号
int npi_inst_port_2_high_conn_sig(char* instHierName,
                                   hdl2hdlVecPairVec_t& portSigPairVec);
int npi_inst_port_2_low_conn_sig(char* instHierName,
                                  hdl2hdlVecPairVec_t& portSigPairVec);
```

#### 核心类型
```cpp
typedef std::vector<npiNlHandle>                      nlHdlVec_t;
typedef std::set<npiNlHandle>                         nlHdlSet_t;
typedef std::vector<std::pair<npiHandle, hdlVec_t>>   hdl2hdlVecPairVec_t;
```

## 4. 架构设计

### 4.1 运行模式

`vsignal` 采用与 `vwave` 相同的 C/S 架构（fork daemon + UDS），但数据源是 KDB 而非 FSDB：

```
vsignal open -dbdir simv.daidir       # 启动服务，加载KDB
vsignal driver  TOP.u_cpu.alu_out     # 查直接驱动
vsignal load    TOP.u_cpu.clk         # 查直接负载
vsignal fanin   TOP.u_cpu.alu_out     # 寄存器级fan-in
vsignal fanout  TOP.u_cpu.clk         # 寄存器级fan-out
vsignal trace   TOP.sig_a TOP.sig_b   # 两点间路径
vsignal conn    TOP.u_cpu             # 实例端口连接
vsignal close                          # 关闭服务
vsignal status                         # 查询状态
```

### 4.2 设计加载方式

支持两种设计输入，通过 `open` 参数区分：

```bash
# 方式1: KDB 数据库（推荐，快速）
vsignal open -dbdir simv.daidir

# 方式2: RTL 源文件（兼容，需编译）
vsignal open -f design.f
vsignal open -sv rtl/top.sv rtl/sub.sv
```

### 4.3 目录结构

```
tool_wave/
├── src/                          # vwave 源码（不动）
├── src_vsignal/                  # vsignal 独立源码
│   ├── main.cpp                  # 入口：命令解析、fork daemon
│   ├── server/
│   │   ├── server_core.h         # 服务端：NPI init、设计加载、事件循环
│   │   └── trace_handler.h       # 追踪命令处理器
│   ├── client/
│   │   └── client_core.h         # 客户端：发送命令、格式化输出
│   └── common/
│       ├── protocol.h            # 协议定义（命令、JSON 构建）
│       ├── json_parser.h         # JSON 解析（可复用 vwave 的）
│       └── run_dir.h             # 运行时目录（.vsignal_run/）
├── Makefile                      # 新增 vsignal 构建目标
└── test_vsignal/                 # vsignal 测试
    ├── example.v
    └── run_test.sh
```

### 4.4 NPI 调用流程

```
fork() → 子进程:
  npi_init(argc, argv)
  npi_load_design(design_argc, design_argv)    ← 加载 KDB 或 RTL
  // 无需 npi_fsdb_open（不读波形）
  bind UDS socket
  事件循环: select() → read command → dispatch → response
```

### 4.5 运行时目录

```
<cwd>/.vsignal_run/
├── vsignal_server.pid
├── vsignal_server.sock
├── vsignal_server.log
└── design_source             # 记录加载的设计路径（KDB 或 filelist）
```

## 5. 命令详细设计

### 5.1 `vsignal open`

```bash
vsignal open -dbdir <path>            # 加载 KDB
vsignal open -f <filelist>            # 加载 filelist
vsignal open [-sv] <file1> <file2>    # 加载 RTL 文件
```

服务端启动流程：
1. 创建 `.vsignal_run/` 目录
2. `fork()` → 子进程 `setsid()` 脱离终端
3. `npi_init()` 初始化 NPI
4. 构造 design argv（`-dbdir simv.daidir` 或 `-f design.f`）
5. `npi_load_design()` 加载设计
6. 写 PID、设计路径文件
7. 创建 UDS socket，进入事件循环

### 5.2 `vsignal driver <signal>`

```
请求: {"id":1, "cmd":"trace_driver", "params":{"signal":"TOP.wtmp"}}
响应: {"id":1, "status":"ok", "data":{
  "signal":"TOP.wtmp",
  "type":"driver",
  "results":[
    {"name":"TOP.TOP:Always0#Always0:6:8:Reg.ROH_wtmp", "type":"npiNlInstPort"}
  ],
  "count":1
}}
```

CLI 输出：
```
Drivers of TOP.wtmp:
  [InstPort] TOP.TOP:Always0#Always0:6:8:Reg.ROH_wtmp
```

### 5.3 `vsignal load <signal>`

```
请求: {"id":2, "cmd":"trace_load", "params":{"signal":"TOP.wtmp"}}
响应: {"id":2, "status":"ok", "data":{
  "signal":"TOP.wtmp",
  "type":"load",
  "results":[
    {"name":"TOP.TOP:Always1#Always1:10:12:Reg.IH_wtmp", "type":"npiNlInstPort"}
  ],
  "count":1
}}
```

### 5.4 `vsignal fanin <signal>`

```bash
vsignal fanin TOP.wtmp                             # 默认参数
vsignal fanin TOP.wtmp --stop-at-pin               # 在 pin 处停止
vsignal fanin TOP.wtmp --report-primary-port        # 报告顶层端口
vsignal fanin TOP.wtmp --scope TOP.u_sub            # 限制搜索范围
```

```
请求: {"id":3, "cmd":"fanin_reg", "params":{
  "signal":"TOP.wtmp",
  "stop_at_pin":false,
  "report_primary_port":false,
  "scope":""
}}
响应: {"id":3, "status":"ok", "data":{
  "signal":"TOP.wtmp",
  "type":"fanin_reg",
  "results":[
    {"name":"TOP.TOP:Always0#Always0:6:8:Reg",
     "type":"npiNlInst",
     "source":["example.v:6:6", "example.v:7:7"]}
  ],
  "count":1
}}
```

CLI 输出：
```
Fan-in registers of TOP.wtmp:
  [Inst] TOP.TOP:Always0#Always0:6:8:Reg
         source: example.v:6-7
```

### 5.5 `vsignal fanout <signal>`

同 fanin，调用 `npi_nl_sig_2_fanOut_reg_conn()`。

### 5.6 `vsignal trace <from> <to>`

```bash
vsignal trace TOP.win TOP.wout
```

```
请求: {"id":4, "cmd":"trace_path", "params":{"from":"TOP.win", "to":"TOP.wout"}}
响应: {"id":4, "status":"ok", "data":{
  "from":"TOP.win",
  "to":"TOP.wout",
  "path":[
    {"name":"TOP.net_win",  "type":"npiNlDeclNet"},
    {"name":"TOP.wtmp",     "type":"npiNlDeclNet"},
    {"name":"TOP.net_wout", "type":"npiNlDeclNet"}
  ],
  "hops":3
}}
```

CLI 输出：
```
Path from TOP.win to TOP.wout (3 hops):
  TOP.net_win → TOP.wtmp → TOP.net_wout
```

### 5.7 `vsignal conn <instance>`

```bash
vsignal conn TOP                        # 查看 TOP 模块端口连接
vsignal conn TOP --direction high       # 仅看向上连接
vsignal conn TOP --direction low        # 仅看向下连接
```

## 6. 协议定义

### 6.1 命令字

| 命令 | 描述 |
|------|------|
| `status` | 查询服务状态 |
| `info` | 查询加载的设计信息 |
| `trace_driver` | 直接驱动追踪 |
| `trace_load` | 直接负载追踪 |
| `fanin_reg` | 寄存器级 fan-in |
| `fanout_reg` | 寄存器级 fan-out |
| `trace_path` | 两点间路径追踪 |
| `inst_conn` | 实例端口连接查询 |

### 6.2 错误码

| 错误码 | 含义 |
|--------|------|
| `ok` | 成功 |
| `error` | 通用错误 |
| `signal_not_found` | 信号名不存在 |
| `instance_not_found` | 实例名不存在 |
| `no_path` | 两信号间无路径 |

## 7. 共用代码与差异

### 7.1 可复用部分（从 vwave 复制/调整）

| 模块 | 内容 |
|------|------|
| `json_parser.h` | JSON 解析器 |
| `run_dir.h` 逻辑 | CWD-based 运行目录（改名为 `.vsignal_run/`） |
| `protocol.h` 框架 | JSON 构建器、Entry 基础结构（命令字不同） |
| `main.cpp` 框架 | fork daemon、信号处理、auto-detect 逻辑 |
| `client_core.h` 框架 | UDS 连接、收发、超时处理 |
| `server_core.h` 框架 | socket bind/listen、select 事件循环、buffered read |

### 7.2 差异部分

| 模块 | vwave | vsignal |
|------|-------|---------|
| NPI 初始化 | `npi_init` + `npi_fsdb_open` | `npi_init` + `npi_load_design` |
| 数据输入 | `<fsdb_path>` | `-dbdir <path>` 或 `-f <filelist>` |
| 命令集 | scopes/signals/get-value | driver/load/fanin/fanout/trace/conn |
| 运行目录 | `.wave_run/` | `.vsignal_run/` |
| PID 文件 | `wave_server.pid` | `vsignal_server.pid` |

## 8. 实现优先级

### Phase 1 — 核心能力（~2 天）

| 优先级 | 功能 | NPI API | 复杂度 |
|--------|------|---------|--------|
| P0 | `open -dbdir` 设计加载 + daemon | `npi_load_design()` | 中 |
| P0 | `driver` 直接驱动追踪 | `npi_nl_trace_driver()` | 低 |
| P0 | `load` 直接负载追踪 | `npi_nl_trace_load()` | 低 |
| P0 | `close` / `status` | — | 低 |
| P1 | `fanin` 寄存器级驱动链 | `npi_nl_sig_2_fanIn_reg_conn()` | 低 |
| P1 | `fanout` 寄存器级负载链 | `npi_nl_sig_2_fanOut_reg_conn()` | 低 |

### Phase 2 — 扩展能力（~1-2 天）

| 优先级 | 功能 | NPI API | 复杂度 |
|--------|------|---------|--------|
| P2 | `trace` 两点间路径 | `npi_nl_sig_2_sig_conn()` | 中 |
| P2 | `conn` 实例端口连接 | `npi_inst_port_2_high/low_conn_sig()` | 中 |
| P2 | `open -f` RTL 源码加载 | `npi_load_design()` | 低 |
| P2 | `info` 设计信息查询 | `npi_handle()` 层次遍历 | 低 |

### Phase 3 — 高级功能（评估中）

| 优先级 | 功能 | 说明 |
|--------|------|------|
| P3 | 位级追踪 | 单 bit 粒度的 driver/load |
| P3 | 原语级连接 | `npi_nl_sig_2_primitive_inst_conn()` |
| P3 | vwave + vsignal 联动 | 值追踪 + 结构追踪结合 |

## 9. 构建

### Makefile 新增目标

```makefile
# 现有
vwave: src/main.cpp
	$(CXX) ... -o build/bin/vwave

# 新增
vsignal: src_vsignal/main.cpp
	$(CXX) ... -o build/bin/vsignal

all: vwave vsignal
```

链接库相同：`-lNPI -lnpiL1 -lpthread -lrt -ldl`

## 10. 测试策略

### 10.1 测试设计

使用 NPI 自带的 `example.v`（2-FF pipeline）作为基础测试设计：

```verilog
module TOP(clk, win, wout);
  input  clk, win;
  output reg wout;
  reg wtmp;
  always@(posedge clk) wtmp <= win;
  always@(posedge clk) wout <= wtmp;
endmodule
```

KDB 生成：`vcs -sverilog -kdb example.v`

### 10.2 预期测试用例

| 测试 | 命令 | 预期结果 |
|------|------|----------|
| 加载 KDB | `vsignal open -dbdir simv.daidir` | 服务启动，PID 文件存在 |
| 驱动追踪 | `vsignal driver TOP.wtmp` | 返回 Always0 Reg 输出端口 |
| 负载追踪 | `vsignal load TOP.wtmp` | 返回 Always1 Reg 输入端口 |
| Fan-in | `vsignal fanin TOP.wout` | 返回 Always1 和 Always0 两个 Reg |
| Fan-out | `vsignal fanout TOP.win` | 返回 Always0 Reg |
| 路径追踪 | `vsignal trace TOP.win TOP.wout` | 返回 win→wtmp→wout 路径 |
| 状态查询 | `vsignal status` | 返回设计加载信息 |
| 关闭 | `vsignal close` | 服务退出，PID 文件清除 |
| 重复关闭 | `vsignal close` | 提示无运行服务 |
| 信号不存在 | `vsignal driver TOP.xxx` | 错误信息 |

## 11. 用户工作流

### 典型调试场景

```bash
# 1. 仿真（已在项目流程中完成，生成 KDB）
vcs -sverilog -kdb -f design.f -o simv
./simv

# 2. 加载波形 + 网表（两个独立工具）
vwave open sim/dump.fsdb              # 波形查询
vsignal open -dbdir simv.daidir       # 结构查询

# 3. 发现异常信号值
vwave get-value -s top.cpu.alu_out -t 1000000 -r hex

# 4. 追踪驱动链
vsignal fanin top.cpu.alu_out
#   → top.cpu.u_alu.op_a_reg (FlipFlop)
#   → top.cpu.u_alu.op_b_reg (FlipFlop)

# 5. 查看驱动寄存器的值
vwave get-value -s top.cpu.u_alu.op_a_reg -t 1000000 -r hex

# 6. 继续追踪
vsignal fanin top.cpu.u_alu.op_a_reg

# 7. 完成后关闭
vwave close
vsignal close
```

## 12. 总结

| 维度 | 评估 |
|------|------|
| **API 可用性** | ✅ NPI L1 提供完整的 driver/load/fanin/fanout/path 追踪 API |
| **KDB 加载** | ✅ **已实测验证**，`npi_load_design(-dbdir)` 直接加载 VCS KDB |
| **无需 RTL 源码** | ✅ 只要有 KDB 即可，VCS `-kdb` 编译已是标准流程 |
| **独立架构** | ✅ 新二进制 `vsignal`，独立 `src_vsignal/`，不影响 vwave |
| **实现复杂度** | 中等 — 可大量复用 vwave 的 C/S 框架 |
| **预估工作量** | Phase 1: ~2 天；Phase 2: ~1-2 天 |
