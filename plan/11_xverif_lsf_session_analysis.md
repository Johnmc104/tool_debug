# xverif LSF session 机制分析与 tool_wave 适配结论

日期：2026-10-20。分析对象：上一级目录 `../xverif`，代码基线 `dcf13d1`。对照对象：`tool_wave` 当前 `f58c941`。

## 1. 结论

`xverif` 已经实现了可用的 LSF 长连接查询模式，核心是：MCP server 在登录机维护一个长期 session，使用 `bsub -I` 把 `tools/xdebug --stdio-loop` 或 `tools/xcov --stdio-loop` 启动到计算节点，随后通过 bsub 的 stdin/stdout 传输 JSONL 请求和响应。

这条链路与“计算节点运行 server、本地执行查询”的目标一致：本地查询端不访问计算节点的 UDS，也不需要知道计算节点的 PID 或 socket 路径。

`tool_wave` 当前没有这项能力。它的 `vwave open` 仍然本地 `fork + exec` worker，worker 在 `.vtool/wave_run/` 创建 Unix Domain Socket，查询端再连接本机 UDS。仅把 `vwave open` 包在 `bsub` 中，不能让登录机后续命令访问计算节点上的 server。

建议 `tool_wave` 优先复用 xverif 的 stdio-loop/LSF 方案；如果必须让多个独立 CLI 进程在登录机查询同一个远程 session，再增加 xverif 类似的 file transport。

## 2. xverif 的实际架构

```text
MCP client / agent
  → xverif_mcp FastMCP server
  → McpSessionManager
  → XdebugLoopSession
  → LsfLauncher
  → bsub -I tools/xdebug --stdio-loop
  → 计算节点上的 xdebug JSONL loop
```

`xverif_mcp` 的 `direct` 和 `lsf` 模式共享同一个 session 实现，只替换 launcher：

- `DirectLauncher` 直接启动 `[tool, "--stdio-loop"]`。
- `LsfLauncher` 使用 `BsubRunner` 构造 `bsub -I [-J name] [-q queue] [-R resource] tool --stdio-loop`。
- `XdebugLoopSession` 统一处理 ready、open、query、close 和错误状态。

关键实现位置：

- [`bsub.py`](../../xverif/xverif_mcp/src/xverif_mcp/lsf/bsub.py)：bsub 参数构造和 job ID 解析。
- [`protocol.py`](../../xverif/xverif_mcp/src/xverif_mcp/lsf/protocol.py)：双线程读取 stdout/stderr、JSONL request/response、超时和进程组终止。
- [`launchers.py`](../../xverif/xverif_mcp/src/xverif_mcp/sessions/launchers.py)：direct/LSF launcher 和 `bkill` 回收。
- [`loop_session.py`](../../xverif/xverif_mcp/src/xverif_mcp/sessions/loop_session.py)：单 session 生命周期、串行请求和 terminal error 处理。
- [`session_manager.py`](../../xverif/xverif_mcp/src/xverif_mcp/sessions/session_manager.py)：多 session、默认 session、alias/session_id 映射和 stale session 恢复。

## 3. LSF 提交与环境处理

### 3.1 bsub 命令

`BsubRunner.build()` 会：

1. 对 `XVERIF_LSF_BSUB` 做 `shlex.split`。
2. 未显式指定 `-I/-Is/-Ip` 时追加 `-I`。
3. 添加 job name、queue 和 resource。
4. 最后追加 `tools/xdebug --stdio-loop` 或 `tools/xcov --stdio-loop`。

job name 由用户名、MCP PID、随机前缀、backend 和 session alias 组成，例如：

```text
xverif_zhhe_12345_a1b2c3d4_xdebug_case_a
```

这样可以在没有 job ID 的异常路径中使用 `bkill -J` 作为兜底。

### 3.2 job ID 和输出流

`JsonlProcess` 同时读取 bsub 的 stdout 和 stderr，并匹配：

```text
Job <123456> is submitted ...
```

启动阶段 `wait_ready()` 会忽略非 JSON 行，直到收到：

```json
{"type":"ready","protocol":"xdebug-stdio-loop","version":1,"pid":123}
```

ready 之后，stdout 必须是 JSONL；非 JSON 输出会被视为协议污染。这样可以容忍 bsub 的提交提示和 `<<Starting on ...>>`，同时保证业务协议不被诊断日志污染。

### 3.3 环境继承

xverif 文档明确要求 MCP server 的环境中显式配置：

- `VERDI_HOME`、`LD_LIBRARY_PATH`。
- `LSF_ENVDIR`、`LSF_BINDIR`、`LSF_LIBDIR`、`LSF_SERVERDIR`。
- 完整 `PATH`，确保计算节点能找到 EDA 工具、bsub 和 bkill。
- Synopsys/Cadence/Mentor license 变量。

这点很重要：MCP server 通常由 IDE 或 agent 启动，不能假设它继承交互 shell 的 module 环境。

## 4. session 生命周期

### 4.1 打开

```text
session_open
  → LsfLauncher.start()
  → bsub -I
  → wait_ready("xdebug-stdio-loop")
  → 发送 session.open JSON
  → 记录 session_id/job_id
  → state = alive
```

`XdebugLoopSession` 默认把 xdebug backend 的内部 session transport 设置为 `uds`。这个 UDS 是计算节点上 xdebug 与其内部 waveform engine 之间的本地通道；MCP 查询端使用的是 bsub 的 stdin/stdout，因此不需要跨节点访问 UDS。

### 4.2 查询

每个请求带 `request_id`，MCP server 通过 lock 保证同一个 session 的请求串行；不同 session 可以并行。响应按 request ID 匹配，乱序响应会暂存在 `pending`。

### 4.3 关闭和异常

关闭顺序为：

1. 发送 backend `session.close`。
2. 发送 `stdio.quit`。
3. 终止本地 bsub 进程组。
4. 已解析 job ID 时执行 `bkill <id>`；没有 ID 时按唯一 job name 执行 `bkill -J`。

普通业务错误，例如 `SIGNAL_NOT_FOUND`，不会销毁 session。传输超时、管道关闭、backend 返回 `SESSION_DEAD/SESSION_LOST` 等 terminal error 会把 session 标为 dead、清理资源，并要求显式 reopen。

启动超时和查询超时分开配置，默认值适合队列等待和大型 FSDB：

- startup：180 秒。
- query：360 秒。
- close：30 秒。
- bkill：30 秒。

## 5. xdebug 的 stdio-loop 实现

`xdebug/src/main.cpp` 识别 `--stdio-loop` 后进入 `run_stdio_loop()`。loop 实现位于 `src/api/stdio_loop.cpp`：

- 启动立即输出 ready envelope。
- 每行读取一个 JSON request。
- 使用 `request_id` 或 `id` 匹配响应。
- `stdio.quit` 返回确认并退出。
- 其他请求进入统一 Dispatcher。
- 所有响应立即 flush。

因此 xverif MCP 没有重写 debug/query 业务，只是在 transport 和 session 层把既有 action API 搬到了 LSF job。

## 6. file transport

xdebug 还实现了独立的 `transport:"file"`，适用于登录机无法连接计算节点 TCP、且两侧共享同一文件系统的场景：

```text
client → transport/requests/
worker → transport/claims/
worker → transport/responses/
client → transport/done/
```

请求先写 `tmp/`，再通过 `rename()` 原子发布；worker 通过 rename 抢占到 `claims/`；响应写入 `responses/`。同时有 `failed/`、`heartbeat/` 和历史清理机制。该通道不依赖跨节点 UDS，也不要求 TCP 可达。

file transport 与 LSF 可以组合，但它和 MCP 的 `bsub -I` stdio 模式是两条不同路径：

- MCP 长 session：LSF 的 stdin/stdout 是查询通道。
- 多进程或 CLI 远程访问：共享目录 file transport 是查询通道。

## 7. 已执行验证

### 7.1 xverif Python session 测试

在 Python 3.11 下执行：

```bash
PYTHONPATH=../xverif/xverif_mcp/src:.. \
  /usr/bin/python3.11 -m pytest -q \
  ../xverif/xverif_mcp/tests/test_loop_session.py \
  ../xverif/xverif_mcp/tests/test_stdio_loop_session_lifecycle.py \
  ../xverif/xverif_mcp/tests/test_session_errors.py \
  ../xverif/xverif_mcp/tests/test_backend_wiring.py
```

结果：`38 passed in 1.93s`。

覆盖内容包括：

- ready/open/query/close。
- 同 session 请求串行化。
- 普通业务错误保持 session alive。
- 进程崩溃和 backend `SESSION_DEAD` 转换为 `SESSION_LOST`。
- stale session 恢复。
- timeout、reopen 和 cleanup。
- LSF mode wiring。

### 7.2 真实 LSF + JSONL 探针

使用真实 LSF binary 和临时 fake backend，验证了完整 transport 链路：

```text
/opt/LSF/10.1/linux3.10-glibc2.17-x86_64/bin/bsub -I
  → ready
  → JSONL query
  → JSONL stdio.quit
  → process-group cleanup + bkill
```

实际结果：

```text
job_id: 316
ready protocol: xdebug-stdio-loop
query host: eda
query: ok
quit: ok
```

这证明 xverif 的 launcher/protocol 能与当前真实 LSF 交互。该探针没有加载 Verdi 或 FSDB，因此不等价于完整 xdebug waveform 查询验收。

### 7.3 诊断命令限制

直接运行 `tools/xverif-lsf-doctor --fake` 时，系统默认 `python3` 是 3.6.8，无法解析 `from __future__ import annotations`；使用 Python 3.11 后，doctor 仍需要已构建的 `xdebug` binary 和 MCP SDK。当前仓库只有 `tools/xdebug` wrapper，`xdebug/xdebug` 尚未生成，因此没有执行真实 xdebug/FSDB doctor。

这属于验证环境/构建产物限制，不是 LSF launcher 代码失败。Python session 测试和真实 LSF fake backend 探针均已通过。

## 8. 对 tool_wave 的差距

当前 `tool_wave`：

```text
vwave open
  → 本地 fork + exec vwave-npi-worker/vwave-ffr-worker --serve
  → worker 在 .vtool/wave_run 创建 UDS

vwave query
  → 查找本地 run_dir
  → PID 检查
  → 连接本机 UDS
  → 一次请求后断开
```

与 xverif 的差距为：

| 能力 | xverif | tool_wave 当前状态 |
|---|---|---|
| LSF job 提交 | `bsub -I` | 无 |
| 计算节点长期 worker | `--stdio-loop` | 只有本地 `--serve` |
| 跨节点查询通道 | stdin/stdout JSONL | 只有本机 UDS |
| job ID/name 记录 | 有 | 无 |
| bkill 回收 | 有 | 无 |
| startup/query timeout | 分离配置 | 只有本地 daemon timeout |
| session lost/reopen | 有 | 无远程 session 状态机 |
| file transport | xdebug 原生支持 | 无 |

所以当前可区分为：

- **本地使用**：已支持。
- **把单条 vwave 命令作为 LSF job 运行**：可以由外部脚本包装，但不是 tool_wave 的集成能力。
- **计算节点运行 vwave server、登录节点持续查询**：当前不支持。

## 9. 对 tool_wave 的适配建议

### P0：stdio-loop LSF session

这是最接近 xverif、改动最小的路径：

1. 从现有 `server::dispatch_request()` 抽出与 socket 无关的 JSON dispatcher。
2. 为 NPI/FFR worker 增加 `--stdio-loop`：stdout 只输出 ready 和 JSONL response，日志全部走 stderr。
3. 增加一个轻量 LSF launcher：构造 `bsub -I -J -q -R worker --stdio-loop`。
4. 在 client/session 层记录 `job_id`、`job_name`、queue、resource、host 和 protocol version。
5. 关闭时执行 shutdown/quit、进程组终止和 `bkill`。
6. 将 startup、query、close、bkill timeout 分开配置。

该模式适合 MCP 或一个长期代理进程。普通 `vwave` CLI 每次启动一个进程，不适合直接把 stdin 当成持久 session，建议另设 `vwave-stdio-worker` 或明确的 `--stdio-loop` 入口。

### P1：file transport

如果要求多个独立的本地 CLI 进程访问同一个计算节点 session，增加 file transport：

- `requests/claims/responses/tmp/heartbeat/done/failed` 目录。
- 原子 rename 发布和领取 request。
- deadline、stale claim、heartbeat、历史清理。
- `session.json` 中记录 transport、file_dir、job_id、job_name 和 host。
- PID 只作诊断，不能作为跨节点存活判断。

### 不建议的方案

- 只把 `bsub` 加到当前 `vwave open`，因为后续本地查询仍没有跨节点通道。
- 只共享 `.vtool/wave_run`，因为 UDS 和 PID 不具备跨主机语义。
- 仅依赖 TCP，而不处理认证、端口和 job 生命周期。

## 10. 兼容性和风险

1. xverif 生产代码的 job ID parser 只匹配英文 `Job <N> is submitted` 格式；locale 或管理员定制输出可能导致无法记录 job ID，需要保留 job name 兜底。
2. `BsubRunner` 默认命令名是 `bsub`。在本机 `/usr/bin/bsub` 是 Slurm 兼容脚本时，必须通过 `PATH` 或 `XVERIF_LSF_BSUB` 指向真实 LSF binary。
3. `bsub -I` 的队列必须允许 interactive；batch queue 应使用 `-K`，不能混用。
4. LSF job 继承 MCP server 环境，EDA library/license/module 必须显式传递到计算节点。
5. 真实 xdebug waveform 查询仍需构建 `xdebug`、安装 Python MCP SDK，并用真实 FSDB 进行验证；本报告的 fake backend 只验证 LSF transport 和 session protocol。
6. file transport 依赖共享文件系统的 `rename()`、可见性和权限语义，需要独立做 NFS/共享 home 验证。

## 11. 最终判断

xverif 已将“LSF 调度”和“远程查询协议”正确分层：LSF 负责计算节点进程生命周期，stdio-loop 或 file transport 负责查询通道，session manager 负责状态、超时和恢复。`tool_wave` 的后端读取能力可以复用，但现有 UDS server/client 不能直接延伸到跨节点场景。

对 `tool_wave` 的最小可行实现是先增加 stdio-loop + LSF launcher，再决定是否需要 file transport。完成 P0 后，才能称为支持“LSF server + local query”；在此之前只能称为“可被外部 LSF 脚本调用”。
