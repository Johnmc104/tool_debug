# FSDB 波形读取驻留服务 — 需求规格

## 1. 项目概述

创建一个基于 Verdi NPI 接口的 **驻留式波形读取服务**，向其他应用提供 **命令行 (CLI)** 接口，按时间、层次结构路径读取 FSDB 波形文件中的信号名称及信号值。

### 1.1 核心设计理念

- **驻留式服务**：FSDB 文件一次加载，服务进程常驻，后续多次查询复用已加载数据，避免重复加载的高昂开销
- **CLI 客户端/服务端架构**：非 Web 后端，通过 Unix Domain Socket 通信，客户端为轻量命令行工具
- **信号查询能力**：支持按层次路径 + 时间点/时间范围读取信号名称和值

### 1.2 技术依赖

| 依赖 | 路径 |
|------|------|
| NPI 头文件 | `/opt/Synopsys/verdi/T-2022.06-SP2/share/NPI/inc/npi_fsdb.h` |
| NPI L1 封装 | `/opt/Synopsys/verdi/T-2022.06-SP2/share/NPI/L1/C/inc/npi_L1.h` |
| NPI 示例 | `/opt/Synopsys/verdi/T-2022.06-SP2/share/NPI/example/via_examples/` |
| NPI 库 | `libNPI.so` |

---

## 2. 系统架构

```
┌─────────────┐    Unix Domain Socket    ┌──────────────────┐
│  CLI 客户端  │ ◄─────────────────────► │   驻留服务端      │
│  (wave_cli)  │     JSON 协议            │  (wave_server)   │
└─────────────┘                          │                  │
                                         │  ┌────────────┐  │
                                         │  │ NPI Engine │  │
                                         │  │ (npi_fsdb) │  │
                                         │  └────────────┘  │
                                         │        ▲         │
                                         │        │         │
                                         │  ┌─────┴──────┐  │
                                         │  │ FSDB File  │  │
                                         │  └────────────┘  │
                                         └──────────────────┘
```

### 2.1 组件说明

| 组件 | 语言 | 说明 |
|------|------|------|
| **wave_server** | C++ | 驻留服务进程，链接 libNPI.so，加载 FSDB 文件，监听 Unix Domain Socket |
| **wave_cli** | C++ / Shell | 轻量客户端，解析命令行参数，连接服务端，发送请求并输出结果 |

### 2.2 通信协议

- **传输层**：Unix Domain Socket，运行时文件（socket、PID）统一存放在工程本地 `.wave_run/` 目录下：
  - Socket 文件：`.wave_run/wave_server.sock`
  - PID 文件：`.wave_run/wave_server.pid`
  - 日志文件：`.wave_run/wave_server.log`
  - 通过 `--run-dir <dir>` 可自定义运行目录，默认为 FSDB 文件所在目录下的 `.wave_run/`
- **消息格式**：JSON（每条消息以换行符 `\n` 分隔）
- **请求/响应**：同步请求-响应模式

---

## 3. 功能点拆解

### 3.1 服务端生命周期管理

| 功能点 | 说明 | 优先级 |
|--------|------|--------|
| F-SRV-01 | **启动服务** — 指定 FSDB 文件路径启动服务进程，调用 `npi_init()` + `npi_fsdb_open()` 加载文件 | P0 |
| F-SRV-02 | **后台守护** — 服务以 daemon 方式运行，支持 `-d` 参数后台启动 | P0 |
| F-SRV-03 | **停止服务** — 客户端发送 `shutdown` 命令，或通过 `wave_cli stop` 优雅关闭，调用 `npi_fsdb_close()` + `npi_end()` | P0 |
| F-SRV-04 | **状态查询** — 客户端可查询服务端状态（已加载文件、运行时长、内存占用） | P1 |
| F-SRV-05 | **超时自动退出** — 可配置空闲超时时间，无请求时自动退出释放资源 | P2 |
| F-SRV-06 | **PID 文件管理** — 写入 PID 文件，防止同一 FSDB 文件重复启动服务 | P1 |

### 3.2 波形文件信息查询

| 功能点 | 对应 NPI API | 说明 | 优先级 |
|--------|-------------|------|--------|
| F-INFO-01 | `npi_fsdb_file_property` | **文件基本信息** — 返回文件时间精度(scale unit)、是否完成、版本等 | P0 |
| F-INFO-02 | `npi_fsdb_min_time` / `npi_fsdb_max_time` | **时间范围** — 返回波形的最小/最大时间 | P0 |
| F-INFO-03 | `npi_fsdb_time_scale_unit` | **时间单位** — 返回时间单位字符串 (ps/ns/us 等) | P0 |

### 3.3 层次结构浏览

| 功能点 | 对应 NPI API | 说明 | 优先级 |
|--------|-------------|------|--------|
| F-HIER-01 | `npi_fsdb_iter_top_scope` + `npi_fsdb_iter_scope_next` | **列出顶层 Scope** — 返回所有顶层模块名称 | P0 |
| F-HIER-02 | `npi_fsdb_scope_by_name` + `npi_fsdb_iter_child_scope` | **列出子 Scope** — 给定层次路径，列出其直接子层次 | P0 |
| F-HIER-03 | `npi_fsdb_hier_tree_dump_scope` | **递归列出 Scope 树** — 从指定路径递归展开整个层次树 | P1 |
| F-HIER-04 | `npi_fsdb_scope_property_str(npiFsdbScopeFullName)` | **获取 Scope 全路径** — 返回指定 scope 的完整层次路径名 | P0 |
| F-HIER-05 | `npi_fsdb_scope_property(npiFsdbScopeType)` | **获取 Scope 类型** — 返回 scope 类型 (Module/Interface/Generate 等) | P1 |

### 3.4 信号名称查询（核心功能）

| 功能点 | 对应 NPI API | 说明 | 优先级 |
|--------|-------------|------|--------|
| F-SIG-01 | `npi_fsdb_iter_sig` + `npi_fsdb_iter_sig_next` | **列出 Scope 下信号** — 给定层次路径，列出其下所有声明信号的名称 | P0 |
| F-SIG-02 | `npi_fsdb_sig_by_name` | **按名称查找信号** — 通过完整层次路径信号名定位信号 handle | P0 |
| F-SIG-03 | `npi_fsdb_sig_property_str(npiFsdbSigName)` | **获取信号短名** — 返回不含层次前缀的信号名 | P0 |
| F-SIG-04 | `npi_fsdb_sig_property_str(npiFsdbSigFullName)` | **获取信号全名** — 返回含完整层次路径的信号名 | P0 |
| F-SIG-05 | `npi_fsdb_sig_property(npiFsdbSigLeftRange, npiFsdbSigRightRange)` | **获取信号位宽** — 返回信号的 [left:right] range 信息 | P0 |
| F-SIG-06 | `npi_fsdb_sig_property(npiFsdbSigDirection)` | **获取信号方向** — 返回 input/output/inout | P1 |
| F-SIG-07 | `npi_fsdb_hier_tree_dump_sig` | **递归列出所有信号** — 从指定 scope 递归列出所有信号名 | P1 |
| F-SIG-08 | `npi_l1_fsdb_sig_by_wild_card` | **通配符匹配信号** — 支持通配符 `*` `?` 模式匹配信号名 | P1 |
| F-SIG-09 | `npi_fsdb_iter_member` | **列出复合信号成员** — 对 struct/array 类型信号，展开其子成员信号名 | P2 |

### 3.5 信号值读取

| 功能点 | 对应 NPI API | 说明 | 优先级 |
|--------|-------------|------|--------|
| F-VAL-01 | `npi_fsdb_sig_value_at` | **单信号单时刻取值** — 指定信号全路径 + 时间，返回该时刻的值 | P0 |
| F-VAL-02 | `npi_fsdb_sig_vec_value_at` | **多信号单时刻取值** — 指定多个信号 + 时间，批量返回值 | P0 |
| F-VAL-03 | `npi_fsdb_sig_value_between` | **单信号时间范围取值** — 指定信号 + 时间范围，返回区间内所有变化点(time,value)对 | P0 |
| F-VAL-04 | `npi_fsdb_sig_find_value_forward` | **正向查找特定值** — 从指定时间起向后搜索信号值等于给定值的时刻 | P1 |
| F-VAL-05 | `npi_fsdb_sig_find_value_backward` | **反向查找特定值** — 从指定时间起向前搜索信号值等于给定值的时刻 | P1 |
| F-VAL-06 | `npi_fsdb_sig_find_x_forward` | **正向查找 X 态** — 从指定时间起向后搜索信号出现 X 态的时刻 | P2 |
| F-VAL-07 | `npi_fsdb_sig_vc_count` | **变化次数统计** — 统计指定时间范围内信号值变化的次数 | P2 |

### 3.6 多信号批量操作（支持信号列表文件）

| 功能点 | 说明 | 优先级 |
|--------|------|--------|
| F-BATCH-01 | **信号列表文件格式** — 支持从文本文件读取信号列表，每行一个信号全路径名，`#` 开头为注释行 | P0 |
| F-BATCH-02 | **批量信号名查询** — 传入信号列表文件，返回每个信号的属性（名称、位宽、方向） | P0 |
| F-BATCH-03 | **批量信号取值** — 传入信号列表文件 + 时间点，返回所有信号在该时刻的值 | P0 |
| F-BATCH-04 | **批量时间范围取值** — 传入信号列表文件 + 时间范围，返回所有信号在时间范围内的变化序列 | P1 |

### 3.7 输出格式

| 功能点 | 说明 | 优先级 |
|--------|------|--------|
| F-FMT-01 | **文本格式** — 默认人类可读文本输出，一行一条记录 | P0 |
| F-FMT-02 | **JSON 格式** — `--json` 选项，输出结构化 JSON，便于程序解析 | P0 |
| F-FMT-03 | **CSV 格式** — `--csv` 选项，输出 CSV 格式，便于表格导入 | P2 |
| F-FMT-04 | **进制选择** — `--radix bin|oct|dec|hex` 选项控制信号值显示进制 | P0 |

---

## 4. CLI 命令设计

### 4.1 服务管理命令

```bash
# 启动服务（前台）
wave_server --fsdb <file.fsdb>

# 启动服务（后台 daemon）
wave_server --fsdb <file.fsdb> -d

# 停止服务
wave_cli stop

# 查询服务状态
wave_cli status
```

### 4.2 信息查询命令

```bash
# 查看波形文件信息（时间范围、时间单位等）
wave_cli info

# 列出顶层层次
wave_cli list-scopes

# 列出指定层次的子层次
wave_cli list-scopes --path <scope_path>

# 递归列出层次树
wave_cli list-scopes --path <scope_path> --recursive
```

### 4.3 信号名称查询命令

```bash
# 列出指定 scope 下的信号名
wave_cli list-signals --path <scope_path>

# 递归列出信号（含子层次）
wave_cli list-signals --path <scope_path> --recursive

# 通配符搜索信号
wave_cli find-signals --pattern "top.dut.*.clk"

# 获取单个信号属性（位宽、方向等）
wave_cli signal-info --signal <signal_full_path>
```

### 4.4 信号值读取命令

```bash
# 单信号、单时刻取值
wave_cli get-value --signal <signal_full_path> --time <time>

# 单信号、时间范围取值
wave_cli get-value --signal <signal_full_path> --begin <begin_time> --end <end_time>

# 多信号、单时刻取值（命令行列出）
wave_cli get-value --signal <sig1> --signal <sig2> --time <time>

# 多信号、单时刻取值（信号列表文件）
wave_cli get-value --signal-file <signal_list.txt> --time <time>

# 多信号、时间范围取值（信号列表文件）
wave_cli get-value --signal-file <signal_list.txt> --begin <begin_time> --end <end_time>

# 指定进制
wave_cli get-value --signal <sig> --time <time> --radix hex

# 正向搜索信号值
wave_cli find-value --signal <sig> --value <val> --from <time> --direction forward

# 反向搜索信号值
wave_cli find-value --signal <sig> --value <val> --from <time> --direction backward
```

### 4.5 通用选项

```
--json              输出 JSON 格式
--csv               输出 CSV 格式
--radix <bin|oct|dec|hex>  值显示进制 (默认 bin)
--socket <path>     指定 socket 文件路径（通常自动推断）
--timeout <sec>     请求超时时间 (默认 30s)
```

---

## 5. 信号列表文件格式规约

```
# signal_list.txt 示例
# 以 # 开头为注释
# 每行一个信号全路径名
# 支持空行

top.dut.cpu_core.clk
top.dut.cpu_core.rst_n
top.dut.cpu_core.pc[31:0]
top.dut.memory_ctrl.addr_bus[15:0]
top.dut.memory_ctrl.data_bus[7:0]
```

---

## 6. 通信协议详细设计

### 6.1 请求消息格式

```json
{
  "id": 1,
  "cmd": "get_value",
  "params": {
    "signals": ["top.dut.clk", "top.dut.rst_n"],
    "time": 5000,
    "radix": "hex"
  }
}
```

### 6.2 响应消息格式

```json
{
  "id": 1,
  "status": "ok",
  "data": {
    "time": 5000,
    "values": [
      {"signal": "top.dut.clk", "value": "1"},
      {"signal": "top.dut.rst_n", "value": "0"}
    ]
  }
}
```

### 6.3 错误响应

```json
{
  "id": 1,
  "status": "error",
  "error": {
    "code": "SIGNAL_NOT_FOUND",
    "message": "Signal 'top.dut.xyz' not found in FSDB file"
  }
}
```

### 6.4 命令字列表

| 命令字 (cmd) | 说明 | 对应 CLI 子命令 |
|-------------|------|----------------|
| `status` | 查询服务状态 | `wave_cli status` |
| `shutdown` | 关闭服务 | `wave_cli stop` |
| `file_info` | 波形文件信息 | `wave_cli info` |
| `list_scopes` | 列出层次 | `wave_cli list-scopes` |
| `list_signals` | 列出信号名 | `wave_cli list-signals` |
| `find_signals` | 通配符搜索信号 | `wave_cli find-signals` |
| `signal_info` | 信号属性查询 | `wave_cli signal-info` |
| `get_value_at` | 单时刻取值 | `wave_cli get-value --time` |
| `get_value_between` | 时间范围取值 | `wave_cli get-value --begin --end` |
| `find_value` | 搜索特定值 | `wave_cli find-value` |
| `vc_count` | 变化次数统计 | `wave_cli vc-count` |

---

## 7. NPI API 映射关系汇总

```
服务端内部处理流程:

启动:  npi_init() → npi_fsdb_open(fsdb_path) → fileHandle 缓存
关闭:  npi_fsdb_close(fileHandle) → npi_end()

层次浏览:
  顶层scope:    npi_fsdb_iter_top_scope(file) → npi_fsdb_iter_scope_next(iter)
  子scope:      npi_fsdb_scope_by_name(file, name, NULL) → npi_fsdb_iter_child_scope(scope)
  scope属性:    npi_fsdb_scope_property_str(npiFsdbScopeFullName, scope)

信号查询:
  scope下信号:  npi_fsdb_iter_sig(scope) → npi_fsdb_iter_sig_next(iter)
  按名查找:     npi_fsdb_sig_by_name(file, name, NULL)
  信号属性:     npi_fsdb_sig_property_str(npiFsdbSigFullName, sig)
                npi_fsdb_sig_property(npiFsdbSigLeftRange, sig, &val)
                npi_fsdb_sig_property(npiFsdbSigRightRange, sig, &val)
                npi_fsdb_sig_property(npiFsdbSigDirection, sig, &val)
  通配符匹配:   npi_l1_fsdb_sig_by_wild_card(file, pattern, scope, &vec)
  成员展开:     npi_fsdb_iter_member(sig) → npi_fsdb_iter_sig_next(iter)

信号取值:
  单信号@时间:   npi_fsdb_sig_value_at(file, name, time, val, format)
  多信号@时间:   npi_fsdb_sig_vec_value_at(file, nameVec, time, valVec, format)
  单信号@范围:   npi_fsdb_sig_value_between(file, name, begin, end, vcVec, format)
  正向搜索值:    npi_fsdb_sig_find_value_forward(file, name, value, begin, &time, format)
  反向搜索值:    npi_fsdb_sig_find_value_backward(file, name, value, begin, &time, format)
  正向搜索X:     npi_fsdb_sig_find_x_forward(file, name, begin, &vc, format)
  变化次数:      npi_fsdb_sig_vc_count(file, name, begin, end)
```

---

## 8. 实现阶段规划

### Phase 1（MVP — 核心可用）

- [ ] 服务端：FSDB 加载 / 关闭 / daemon 运行
- [ ] 服务端：Unix Domain Socket 监听 + JSON 协议处理
- [ ] 文件信息查询 (F-INFO-01~03)
- [ ] 顶层/子层次浏览 (F-HIER-01~02)
- [ ] Scope 下信号名列出 (F-SIG-01~05)
- [ ] 单信号单时刻取值 (F-VAL-01)
- [ ] 多信号单时刻取值 (F-VAL-02)
- [ ] 信号列表文件批量取值 (F-BATCH-01, F-BATCH-03)
- [ ] CLI 客户端基本命令
- [ ] 文本 + JSON 输出格式 (F-FMT-01~02, F-FMT-04)

### Phase 2（增强功能）

- [ ] 递归层次/信号浏览 (F-HIER-03, F-SIG-07)
- [ ] 通配符信号搜索 (F-SIG-08)
- [ ] 时间范围取值 (F-VAL-03)
- [ ] 批量时间范围取值 (F-BATCH-04)
- [ ] 正向/反向搜索 (F-VAL-04~05)
- [ ] 信号方向/类型查询 (F-SIG-06)
- [ ] 服务状态查询 + PID 文件 (F-SRV-04, F-SRV-06)

### Phase 3（完善优化）

- [ ] X 态搜索 (F-VAL-06)
- [ ] 变化次数统计 (F-VAL-07)
- [ ] 复合信号成员展开 (F-SIG-09)
- [ ] CSV 输出格式 (F-FMT-03)
- [ ] 空闲超时退出 (F-SRV-05)
- [ ] 多 FSDB 文件支持（单服务加载多文件）

---

## 9. 构建与部署

### 9.1 编译依赖

```makefile
VERDI_HOME = /opt/Synopsys/verdi/T-2022.06-SP2
NPI_INC    = $(VERDI_HOME)/share/NPI/inc
NPI_L1_INC = $(VERDI_HOME)/share/NPI/L1/C/inc
NPI_LIB    = $(VERDI_HOME)/share/NPI/lib/linux64
LIBS       = -lNPI -lpthread
```

### 9.2 运行环境

```bash
export LD_LIBRARY_PATH=$VERDI_HOME/share/NPI/lib/linux64:$LD_LIBRARY_PATH
export NOVAS_HOME=$VERDI_HOME
```

---

## 10. 错误码定义

| 错误码 | 说明 |
|--------|------|
| `OK` | 成功 |
| `FSDB_OPEN_FAILED` | FSDB 文件打开失败 |
| `SCOPE_NOT_FOUND` | 指定层次路径不存在 |
| `SIGNAL_NOT_FOUND` | 指定信号名不存在 |
| `INVALID_TIME` | 时间参数非法或超出范围 |
| `INVALID_PARAMS` | 请求参数格式错误 |
| `SERVER_BUSY` | 服务端正忙 |
| `FILE_READ_ERROR` | 信号列表文件读取失败 |
| `INTERNAL_ERROR` | 服务端内部错误 |

