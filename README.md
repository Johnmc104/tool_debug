# tool_wave — FSDB 波形读取 & 网表信号追踪

命令行工具集，基于 Verdi NPI。包含两个独立工具：

- **vwave** — 读取 FSDB 波形：信号值查询、边沿查找、变化计数
- **vsignal** — 追踪网表连接：驱动/负载、fanin/fanout、路径追踪、端口连接

两工具均采用 daemon 模式（open 一次，反复查询），支持 `--json` 结构化输出。

## 安装

```bash
# 编译（需要 VERDI_HOME 环境变量）
make

# 部署到 $VTOOL_HOME/bin/
make deploy-bin
```

## vwave 快速上手

```bash
# 1. 加载波形
vwave open tb_top.fsdb

# 2. 浏览层次
vwave scopes                           # 顶层 scope
vwave scopes tb_top --depth 2          # 展开两层
vwave signals tb_top.intf              # 列出信号

# 3. 查值
vwave get -s tb_top.clk -t 500000                      # 单信号单时间点
vwave get -s tb_top.clk -s tb_top.rst -t 500000 -r hex # 多信号
vwave get -s tb_top.clk -b 0 -e 100000                 # 时间范围
vwave get -s tb_top.clk -s tb_top.rst -b 0 -e 100000   # 多信号范围

# 4. 分析
vwave edge -s tb_top.clk -t 0 --rising         # 找上升沿
vwave vc-count -s tb_top.clk                   # 翻转总数

# 5. 搜索
vwave find "*HCLK*"                            # 通配符搜索

# 6. 关闭
vwave close
```

### vwave 命令速查

| 命令 | 用法 | 说明 |
|------|------|------|
| `open` | `vwave open <file.fsdb>` | 加载波形，启动 daemon |
| `close` | `vwave close` | 关闭 daemon |
| `status` | `vwave status` | 服务状态 |
| `info` | `vwave info` | 时间范围、scale |
| `scopes` | `vwave scopes [path] [--depth N]` | 列子 scope |
| `signals` | `vwave signals <scope>` | 列信号 |
| `signal-info` | `vwave signal-info <signal>` | 信号元数据 |
| `find` | `vwave find <pattern> [--scope path]` | 通配符搜索 |
| `get` | `vwave get -s <sig> -t <time>` | 读值（见下文选项） |
| `edge` | `vwave edge -s <sig> -t <time>` | 找边沿 |
| `vc-count` | `vwave vc-count -s <sig>` | 变化计数 |

**get 选项**: `-s` 信号（可重复）、`-f` 信号文件、`-t` 时间点、`-b/-e` 范围、`-r` 进制（bin/hex/oct/dec）、`--limit` 最大样本数

**edge 选项**: `--rising`、`--falling`、`--dir forward|backward`

**全局选项**: `--json`、`--compact`、`--depth N`、`--fsdb <path>`

---

## vsignal 快速上手

```bash
# 1. 加载设计（VCS 编译需加 -kdb）
vsignal open -dbdir simv.daidir

# 2. 追踪驱动/负载
vsignal driver top.u_cpu.HCLK                         # 谁驱动此信号
vsignal driver top.u_cpu.HCLK --pass-mod               # 穿透模块边界
vsignal load top.u_cpu.HCLK --assign-cell               # 此信号驱动了谁

# 3. 寄存器级 fanin/fanout
vsignal fanin top.u_cpu.HCLK                           # 源寄存器
vsignal fanout top.u_cpu.HCLK --limit 10               # 目的寄存器（限10条）

# 4. 路径追踪 & 端口连接
vsignal trace top.data_in top.data_out                  # 两信号间路径
vsignal conn top.u_cpu                                  # 实例端口映射

# 5. 批量查询（减少调用次数）
vsignal driver -s top.sig_a -s top.sig_b -s top.sig_c   # 3信号1次调用
vsignal fanin -f signals.txt                            # 从文件批量

# 6. 关闭
vsignal close
```

### vsignal 命令速查

| 命令 | 用法 | 说明 |
|------|------|------|
| `open` | `vsignal open -dbdir <kdb_dir>` | 加载 KDB 设计 |
| `open` | `vsignal open <file.v> [...]` | 加载 RTL 源文件 |
| `close` | `vsignal close` | 关闭 daemon |
| `status` | `vsignal status` | 服务状态 |
| `info` | `vsignal info` | 设计元数据 |
| `driver` | `vsignal driver <signal>` | 驱动源追踪 |
| `load` | `vsignal load <signal>` | 负载追踪 |
| `fanin` | `vsignal fanin <signal>` | 反向到源寄存器 |
| `fanout` | `vsignal fanout <signal>` | 正向到目的寄存器 |
| `trace` | `vsignal trace <from> <to>` | 两点间路径 |
| `conn` | `vsignal conn <instance>` | 端口连接 |

**追踪选项**: `--assign-cell`（穿透 assign）、`--pass-mod`（穿透模块）、`--stop-at-pin`、`--report-primary-port`、`--scope <name>`、`--level high|low`

**批量**: `-s` 重复多信号、`-f` 信号列表文件

**输出控制**: 默认 compact 模式（仅叶节点名），`--full` 切换完整输出，`--limit N` 限制结果数（默认 50）

**全局选项**: `--json`

---

## 芯片调试工作流示例

### 场景：GPIO 输出值异常，定位根因

```bash
# ① 时域：确认异常值
vwave open tb_top.fsdb
vwave get -s tb_top.dut.gpio_data -t 500000 -r hex --json

# ② 结构域：找驱动源
vsignal open -dbdir simv.daidir
vsignal driver tb_top.dut.gpio_data --pass-mod --json

# ③ 时域：验证驱动源的值
vwave get -s <driver_signal> -t 500000 -r hex --json

# ④ 结构域：深入追踪
vsignal fanin <driver_signal> --json
```

### 场景：时钟未翻转

```bash
vwave vc-count -s tb_top.dut.HCLK --json                    # 翻转数=0?
vwave edge -s tb_top.dut.HCLK -t 0 --rising --json          # 第一个上升沿?
vsignal driver tb_top.dut.HCLK --pass-mod --json             # 时钟源在哪?
vsignal fanin tb_top.dut.HCLK --json                        # 门控寄存器?
```

---

## 构建 & 部署

```bash
make                  # 编译 → release/bin/
make build            # 同上
make deploy-bin       # 安装到 $VTOOL_HOME/bin/
make package          # 打包 tar.gz + sha256 → dist/
make release          # 创建 git tag + GitHub Release
make test             # 运行全部测试
make version          # 显示版本号
make pkg-info         # 显示打包配置
make clean            # 清理 release/ dist/ build/
```

## 依赖

| 依赖 | 说明 |
|------|------|
| **VERDI_HOME** | Verdi 安装路径（`module load synopsys/verdi` 或手动 export） |
| **GCC 9+** | C++14 编译器 |
| **Linux** | Unix Domain Socket, fork/setsid |
| **VCS -kdb** | vsignal 需要 KDB 数据库（`vcs -kdb ...` 编译生成） |

> vsignal 会自动从 `VERDI_HOME` 推导 `LD_LIBRARY_PATH`，无需手动设置。

## 工程结构

```
src_common/           共享库（tw:: 命名空间）
  daemon.h            fork + daemon + readiness-poll 共享模板
  server_loop.h       事件循环（空闲超时、per-client 超时）
  client.h            UDS 通信（RAII fd、EINTR 安全）
  json.h              JSON 构建/解析
  protocol.h          响应编码、错误码
  run_dir.h           运行目录管理

src_vwave/            vwave 源码
  main.cpp            CLI + CliOptions + parse_args + cmd_query
  server/
    server_core.h     globals + dispatch + run_server
    handlers.h        10 个命令 handler + 信号缓存

src_vsignal/          vsignal 源码
  main.cpp            CLI + CliOptions + parse_args + cmd_query
  server/
    server_core.h     globals + dispatch + run_server
    handlers.h        8 个命令 handler
    npi_helpers.h     NPI 数据转换

release/bin/          编译输出
dist/                 发布归档
```
