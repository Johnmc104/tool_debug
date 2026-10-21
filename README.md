# tool_wave v1.12 — FSDB 波形读取 & 网表信号追踪

命令行工具集。vwave 支持 NPI 和动态加载 FsdbReader 两种后端，vsignal 使用 Verdi NPI。包含两个独立工具：

- **vwave** — 读取 FSDB 波形：信号值查询、边沿查找、变化计数
- **vsignal** — 追踪网表连接：驱动/负载、fanin/fanout、路径追踪、端口连接

两工具均采用 daemon 模式（open 一次，反复查询），支持 `--json` 结构化输出，并可用 `VWAVE_RUN_MODE=local|lsf` 在本地 worker 和 LSF 计算节点 worker 之间切换。

## 安装

```bash
# 编译（需要 VERDI_HOME 环境变量）
make VERDI_HOME=/path/to/verdi FFR_SDK_HOME=/path/to/verdi

# 仅构建无需 NPI 的 vwave CLI + FFR worker
make vwave-ffr FFR_SDK_HOME=/path/to/verdi

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

### 选择读取后端

原用法默认使用 NPI。选择 FsdbReader 时，加一个参数：

```bash
# 使用当前 VERDI_HOME 中的配套 reader
vwave open tb_top.fsdb --backend ffr

# 显式选择另一套安装
vwave open tb_top.fsdb --backend ffr --verdi-home /path/to/verdi

# 查询沿用打开时的后端和库
vwave get -s tb_top.clk -t 500000 --json
vwave status --json

# 同一波形切回 NPI
vwave open tb_top.fsdb --backend npi

# 多会话使用不同运行目录
vwave open other.fsdb --backend ffr --run-dir /tmp/other-wave
vwave info --run-dir /tmp/other-wave --json
```

FFR 库来源为 `--verdi-home` 或当前 `VERDI_HOME`，加载配套 `share/FsdbReader/linux64/libnffr.so` 和 `libnsys.so`。库加载失败会报告原因，不自动换成 NPI。当前已验证并接受的 reader 构建为 **T-2022.06-SP2**、**Y-2026.03-SP2**；其他构建明确报未验证，后续按 ABI 和行为验证扩展。

切换 EDA 环境不会改变已有会话。再次 `open --backend ffr` 时，如果安装目录或库哈希变化，会重启该会话；本地模式会先做 worker 加载预检，失败时保留原会话。`status/info` 的 JSON 包含实际 backend、SDK 路径、库路径和 SHA-256。查询可显式指定 `--backend` 核对当前模式，不匹配时提示重新 open。

### LSF 计算节点模式

`vwave` 和 `vsignal` 都支持通过 `VWAVE_RUN_MODE` 选择本地 daemon 或 LSF 计算节点 worker：

```bash
export VWAVE_RUN_MODE=lsf
export VWAVE_LSF_BSUB=/opt/LSF/10.1/linux3.10-glibc2.17-x86_64/bin/bsub
export VWAVE_LSF_BKILL=/opt/LSF/10.1/linux3.10-glibc2.17-x86_64/bin/bkill
export VWAVE_LSF_QUEUE=interactive
vwave open tb_top.fsdb --backend ffr --verdi-home /path/to/verdi --run-dir /tmp/wave
vwave status --run-dir /tmp/wave --json
vwave get -s tb_top.clk -t 500000 --run-dir /tmp/wave --json
vwave close --run-dir /tmp/wave

# vsignal 复用同一组 VWAVE_LSF_* 环境变量
vsignal open -dbdir simv.daidir --run-mode lsf --run-dir /tmp/signal
vsignal driver top.u_cpu.HCLK --run-dir /tmp/signal --json
vsignal close --run-dir /tmp/signal
```

LSF 模式在登录机保留本地 UDS proxy，通过 `bsub -I` 启动计算节点上的
`vwave-*-worker --stdio-loop`；后续独立执行的本地查询仍连接同一 proxy。
`--backend ffr` 的 reader 预检在 LSF 模式不会于登录机执行：登录机只读取库文件
指纹，实际 `dlopen(libnffr.so/libnsys.so)`、许可证检查和 FSDB 打开由计算节点
worker 完成，`open` 等待远端 `ready` 响应。这使登录节点 glibc 2.17、计算节点
glibc 2.28 的 Verdi Y-2026.03-SP2 组合可以正常工作；库目录和 worker 必须在
计算节点可访问的共享文件系统上。
`VWAVE_RUN_MODE=local` 或 `--run-mode local` 可切回原有本地模式。
`VWAVE_LSF_RESOURCE`、`VWAVE_LSF_QUERY_TIMEOUT` 和 `--lsf-job-name` 可分别设置资源、查询超时和作业名。
必须使用真实 LSF 的 `bsub/bkill`，避免系统中的 Slurm 兼容包装器。

FFR 支持四态数字信号、宽总线、位选/片选、unpacked/packed struct 字段和静态数组组合值，复用原有查询命令；real 已支持普通值、范围和计数。string 按 NPI 当前行为处理：点值返回读取错误，范围为空，计数和 edge 事件仍可查询；事务/属性值仍返回明确的未支持错误。范围查询先输出起始时刻的值，`vc-count` 仅计真实记录；边沿查找保持原 NPI 命令的边界行为。

补充测试曾发现总线位选/片选、数组中间层、struct 字段路径和 packed struct 展开问题，现已完成修复。当前支持范围、Reader 版本限制和 string 兼容语义见 [FFR 功能缺口分析](plan/09_ffr_feature_gaps_and_solution.md)。

运行时 `vwave`、`vwave-*-worker`、`vsignal` 与 `vsignal-worker-*` 必须位于同一目录。发布包包含 vwave 的 NPI/FFR worker，以及
绑定 Verdi 2022、2024、2026 NPI 的三套 vsignal worker。两个 CLI 都不链接 NPI；NPI/FFR worker 在实际执行节点加载配套厂商库和许可证环境。

CentOS 7/glibc 2.17 已验证 **2022 reader** 的完整 open、查询和 close。2026 reader 自身需要更高 glibc，动态加载不能降低该要求。构建时静态链接 C++ 运行库，以减少用户切换 EDA 后的 GLIBCXX 冲突；发布仍应核验生成二进制的 GLIBC 要求。

### v1.12 发布说明

- 新增 `VWAVE_RUN_MODE=lsf`：通过登录机 proxy、LSF interactive job 和 stdio-loop worker 支持计算节点运行、登录机查询；保留 `local` 模式。
- vwave/vsignal CLI 增加 `--version`，公共编译和打包流程统一使用 `VERSION=1.12`。
- vsignal 拆分为 NPI-free CLI 与 `vsignal-worker`，复用 `VWAVE_RUN_MODE` 和 `VWAVE_LSF_*` 实现本地/LSF 查询。
- vsignal 发布包同时提供 `vsignal-worker-2022`、`vsignal-worker-2024`、`vsignal-worker-2026`；CLI 根据 `VERDI_HOME` 自动选择对应 worker。
- 修复 FFR LSF 模式在登录节点错误执行 reader preflight 的问题，改为等待计算节点 worker 完成实际加载。
- 新增 VCS `W-2024.09` KDB 兼容支持；`VCS_HOME`/`VCS_BIN` 可指定 `test-vsignal` 使用的 VCS。
- 新增 `src_common/transport.h` 和 `src_common/lsf.h`，统一两个工具的 worker session 编解码、LSF 命令拆分及状态字段处理；业务 session、协议和厂商库适配仍保持隔离。
- FFR 与 NPI 的普通位选、二维数组、struct、packed struct、real 和组合信号查询已完成对照验证。
- T-2022.06-SP2、Y-2026.03-SP2 配套样本及 CentOS 7/2022 reader 验证通过；FFR worker 仍不携带厂商 reader，运行时需提供配套 SDK。
- packed struct 展开依赖已验证 Reader 的内部开关，新增 EDA/Reader 版本必须重新做 ABI 和行为验证。
- 当前发布归档：`tool_wave-1.12-linux-x86_64.tar.gz`，同时提供 SHA-256 校验文件。

### VCS 2024 / KDB 兼容性

`vsignal` 已验证支持 VCS `W-2024.09` 生成的 KDB（`vcs -kdb`）。验证覆盖
`open/status/info/driver/fanout/conn/close`，并确认同一套源码可使用 Verdi
`W-2024.09` NPI 编译运行。VCS 只负责生成 KDB，`vsignal` 的 NPI 版本由构建时
的 `VERDI_HOME` 决定；运行时必须保持 NPI 头文件、库和 `VERDI_HOME` 配套，不能
混用不同 Verdi 版本的库。

```bash
# 使用 VCS 2024 生成 KDB并运行完整 vsignal 回归
export VCS_HOME=/opt/Synopsys/vcs/W-2024.09
export VERDI_HOME=/opt/Synopsys/verdi/W-2024.09
make VERDI_HOME="$VERDI_HOME" vsignal
make test-vsignal VCS_BIN="$VCS_HOME/bin/vcs"
```

也可以只切换 KDB 生成器，继续使用默认发布版（Y-2026.03-SP2）`vsignal`：

```bash
make test-vsignal VCS_HOME=/opt/Synopsys/vcs/W-2024.09
```

当前验证矩阵使用匹配的 VCS/Verdi 版本：`T-2022.06-SP2`、`W-2024.09`、
`Y-2026.03-SP2` 均已完成 `open/info/driver/close`。VCS 2024 生成的 KDB
可由 Verdi 2024/2026 读取；Verdi 2022 对 VCS 2024 KDB 不兼容，必须使用
VCS 2022 生成的 KDB。其他补丁版本需要按同样流程重新验证。

### vsignal 多版本 Verdi 选择

发布包中的三套 NPI worker 使用各自安装目录的头文件、库和 RPATH 编译：

| `VERDI_HOME` 路径 | 选择的 worker | NPI 库 |
|---|---|---|
| 包含 `2022`（如 `T-2022.06-SP2`） | `vsignal-worker-2022` | `$VERDI_HOME/share/NPI/lib/linux64` |
| 包含 `2024`（如 `W-2024.09`） | `vsignal-worker-2024` | `$VERDI_HOME/share/NPI/lib/linux64` |
| 包含 `2026`（如 `Y-2026.03-SP2`） | `vsignal-worker-2026` | `$VERDI_HOME/share/NPI/lib/linux64` |

`vsignal open` 在登录机只运行 NPI-free CLI；本地模式或 LSF 模式的 worker
再加载 NPI。`VERDI_HOME` 未设置或路径不含上述年份时，保留 `vsignal-worker`
作为兼容回退，其版本由构建时的 `VERDI_HOME` 决定。切换 Verdi 版本前应关闭
旧会话并重新执行 `open`，避免一个运行中的 server 继续使用旧 NPI。

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

**全局选项**: `--json`、`--compact`、`--depth N`、`--fsdb <path>`、`--run-dir <path>`、`--backend npi|ffr`；`--verdi-home` 仅用于 `open --backend ffr`

`vwave --version` 和 `vsignal --version` 输出当前发布版本；`make package` 会校验 CLI 版本与 `VERSION` 文件一致。

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

源码复用边界、公共层依赖规则和后续治理任务见 [vwave/vsignal 源码复用与治理方案](plan/16_source_reuse_and_governance.md)。

```bash
make                  # 编译 → release/bin/
make build            # 同上
make deploy-bin       # 安装到 $VTOOL_HOME/bin/
make package          # 打包 tar.gz + sha256 → dist/
make release          # 创建 git tag + GitHub Release
make test             # 运行全部测试
make test-vwave-lsf   # vwave LSF proxy smoke test
make test-vsignal-lsf # vsignal LSF proxy smoke test
make check-source-governance # 检查 vwave/vsignal 公共层边界和发布清单
make version          # 显示版本号
make pkg-info         # 显示打包配置
make clean            # 清理 release/ dist/ build/
```

## 依赖

| 依赖 | 说明 |
|------|------|
| **VERDI_HOME** | Verdi 安装路径（`module load synopsys/verdi` 或手动 export）；决定 `vsignal` 的 NPI 编译/运行库 |
| **VCS_HOME / VCS_BIN** | `test-vsignal` 生成 KDB 时使用的 VCS 安装或可执行文件；可选，默认从 `PATH` 查找 |
| **VWAVE_RUN_MODE** | 运行模式：`local`（默认）或 `lsf`；`--run-mode` 可只覆盖当前 `open` |
| **VWAVE_LSF_BSUB / VWAVE_LSF_BKILL** | LSF 的 `bsub`、`bkill` 可执行文件路径；默认从 `PATH` 查找 |
| **VWAVE_LSF_QUEUE / VWAVE_LSF_RESOURCE** | LSF 队列和资源表达式；也可用 `--lsf-queue`、`--lsf-resource` |
| **VWAVE_LSF_QUERY_TIMEOUT** | LSF worker 查询超时（秒），默认 `360` |
| **GCC 9+** | C++14 编译器 |
| **Linux** | Unix Domain Socket, fork/setsid |
| **VCS -kdb** | vsignal 需要 KDB 数据库（`vcs -kdb ...` 编译生成） |

> `vsignal-worker` 通过 RPATH 绑定编译时的 NPI 库；CLI 本身不依赖 NPI。VCS 2024 KDB 已完成交叉版本验证。
> FFR worker 独立动态加载打开时选中的 reader，不调用 NPI 初始化。

### 双后端验证

```bash
# 原 NPI 回归
make test-vwave

# 现有样本，两套 reader 与 NPI 对照；使用临时会话目录
python3 test_vwave/test_backends.py \
  --sdk /opt/Synopsys/verdi/T-2022.06-SP2 \
  --sdk /opt/Synopsys/verdi/Y-2026.03-SP2

# 用指定 VCS/Verdi 生成配套数字、数组、X/Z 和 glitch 样本
bash test_vwave/build_backend_fixture.sh /path/to/vcs /path/to/verdi
python3 test_vwave/test_backends.py --fixture \
  --sample build/backend_fixtures/<version>/fixture.fsdb --sdk /path/to/verdi

# Docker 中构建并验证 CentOS 7（需要已缓存镜像）
bash test_vwave/test_centos7_backend.sh

# 无挂载替代验收：提取公开 CentOS 7 库，使用用户命名空间 chroot
python3 reverse_analysis/scripts/extract_centos7_runtime.py
python3 test_vwave/test_centos7_chroot.py --rootfs build/centos7_backend/rootfs
```

开发与验收记录见 [双后端实施记录](plan/08_vwave_dual_backend_implementation.md)。

> 已验证 Verdi 版本：T-2022.06-SP2、Y-2026.03-SP2。2026.03 起 NPI 签出 License feature `VerdiNPI`，若 License 服务器无此 feature，可 `export NPI_LICENSE=Verdi` 回退使用 `Verdi` feature。

## 工程结构

```
src_common/           共享库（tw:: 命名空间）
  daemon.h            fork + daemon + readiness-poll 共享模板
  server_loop.h       事件循环（空闲超时、per-client 超时）
  client.h            UDS 通信（RAII fd、EINTR 安全）
  json.h              JSON 构建/解析
  protocol.h          响应编码、错误码
  run_dir.h           运行目录管理
  npi_env.h           VERDI_HOME 对齐、NPI 初始化失败诊断
  transport.h         worker session 的 hex 编解码
  lsf.h               LSF command 拆分、PID 替换和 status 字段注入

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
