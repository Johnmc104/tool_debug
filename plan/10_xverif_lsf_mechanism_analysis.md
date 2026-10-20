# xverif LSF 机制分析与 tool_wave 适配建议

日期：2026-10-10。分析对象：上一级目录 `xverif`，代码基线 `dcf13d1`。范围：`xverif_mcp` 的 LSF launcher、stateful session 生命周期，以及 xdebug 的共享文件传输机制。

## 1. 结论

xverif 的 LSF 处理不是把 UDS socket 直接搬到计算节点，而是在 MCP server 中保留一个长期存在的会话对象，由它启动和维护一个 LSF 作业：

```text
MCP client
  → xverif_mcp / McpSessionManager
  → LsfLauncher
  → bsub -I tools/xdebug --stdio-loop
  → 计算节点上的 xdebug/xcov loop
```

LSF 作业的标准输入、标准输出被当作 JSONL 控制通道。每个 session 一个 LSF job，同一 session 的请求用锁串行发送，不同 session 可以并行。xdebug 和 xcov 共用这套 launcher，只替换实际的 `--stdio-loop` 后端。

另外，xdebug 还实现了独立的 `transport:file`。它不依赖 LSF 的标准输入输出，也不要求登录节点能够连接计算节点 TCP 端口；client 和计算节点 daemon 通过共享文件系统的 request/response 目录通信。这个机制更接近当前 tool_wave 的“查询端与 server 分离”问题。

对 tool_wave 的直接判断：当前 `vwave` worker 是基于 UDS 的后台 daemon，`vwave open` 会本地 fork/exec，后续命令通过运行目录中的 socket 查询。因此不能只把 `bsub -I` 加到现有 `open` 命令上就得到 xverif 的效果。需要新增 stdio loop 或 file transport 层。若集群网络不可控，建议优先实现 file transport；若主要由 MCP 或一个长期代理进程使用，再实现 `bsub -I` JSONL 模式。

## 2. xverif 的 LSF launcher

### 2.1 命令构造

`xverif_mcp/lsf/bsub.py` 中的 `BsubRunner` 负责把后端命令包装成 LSF 命令：

```text
bsub -I [-J job_name] [-q queue] [-R resource] tools/xdebug --stdio-loop
```

`-I`、`-Is`、`-Ip` 被视为同一组 interactive 选项；如果用户在 `XVERIF_LSF_BSUB` 中已经指定，就不会重复追加。队列和资源字符串作为参数独立传给 bsub，命令不通过 shell 拼接，减少引号和空格问题。

当前默认队列是 `interactive`。session 可以从工具参数传入 `queue/resource`，也可以使用 `XVERIF_LSF_SESSION_QUEUE` 和 `XVERIF_LSF_SESSION_RESOURCE` 的默认值。LSF launcher 只负责启动，EDA 环境仍通过 MCP server 的环境继承到 bsub，再由 bsub 传给计算节点作业。

### 2.2 作业身份

`JsonlProcess` 同时读取 bsub 子进程的 stdout 和 stderr，并从两侧识别典型输出：

```text
Job <123456> is submitted to queue <interactive>.
```

作业 ID 会保存到 `job_id`，作业名保存在 `job_name`，并通过 session 的 public JSON 暴露。作业名使用用户、MCP 进程 PID、随机前缀、backend 和 session alias 组合，并清理非字母数字字符，避免多个 MCP 进程或用户之间误杀同名作业。

这里有一个需要吸收的边界：当前解析器只匹配英文标准格式和纯数字 job ID。不同 LSF 版本、locale 或管理员定制输出可能导致 job ID 未识别；tool_wave 适配时应保留原始提交输出，并允许显式 job name 作为回收兜底。

### 2.3 标准流协议

`JsonlProcess` 启动 bsub 时创建独立进程组，并分别用后台线程读取 stdout/stderr：

- `wait_ready()` 忽略启动阶段的非 JSON 行，直到收到指定 `type=ready` 和 `protocol`。
- ready 之后，响应必须是一行完整 JSON；非 JSON 输出被视为协议污染。
- 每个请求带 `request_id`，响应按 ID 匹配；不属于当前请求的响应暂存到 `pending`。
- 同一个 session 的 `_call_raw()` 外层持有锁，因此请求实际串行。

这套设计把 LSF 排队、计算节点启动和后端加载都纳入同一个 ready 超时。它不需要远程端口，也不需要查询端知道计算节点的 UDS 路径。

## 3. session 生命周期和故障处理

xverif 的 `XdebugLoopSession` 生命周期为：

```text
open
  → LsfLauncher.start()
  → bsub -I
  → wait_ready()
  → session.open JSONL 请求
  → state=alive

query
  → stdin 写入一行 JSON
  → stdout 按 request_id 读取响应

close
  → session.close
  → stdio.quit
  → 终止本地 bsub 进程组
  → bkill <job_id>，无 job_id 时 bkill -J <job_name>
```

关键行为如下：

| 场景 | xverif 处理 | 对 tool_wave 的启示 |
|---|---|---|
| LSF 排队或后端打开较慢 | ready/startup timeout；错误提示区分队列拥塞和数据量大 | 启动超时不能只按本地 fork 的 30 秒设置 |
| 后端进程崩溃 | 转换为 `SESSION_LOST`，立即清理并要求显式重开 | 不应保留“看似 alive”的旧 session |
| 普通查询错误 | 原样返回，不销毁 session | `SIGNAL_NOT_FOUND` 等业务错误不能误判成 transport 故障 |
| close 时 job ID 已知 | 先终止进程组，再 `bkill <id>` | 优先按 job ID 回收，避免杀错同名 job |
| job ID 未解析 | 按唯一 job name 执行 `bkill -J` | job name 必须全局可区分 |
| bsub/后端退出前未 ready | 返回 open 失败并执行清理 | 要删除半成品 session 元数据 |
| 同名 session 已存在但进程已死 | 标记旧 session 失效并允许重新打开 | 需要 stale session 恢复路径 |

`xverif_lsf_doctor` 和 `fake_bsub` 用于本地验证启动、ready、一次请求、quit 和清理。fake runner 还能模拟排队延迟、stdout 噪声、ready 前退出、ready 后退出和子进程被杀。它能覆盖状态机，但不能替代真实 LSF 的队列、环境和作业回收验证。

## 4. 计算节点环境处理

xverif 文档特别强调：MCP server 是由 IDE 或 agent 启动的子进程，不应假定它继承了交互 shell 中的 module 环境。`bsub` 只会把 MCP server 当前环境传给作业，所以以下变量必须在 MCP 配置中显式给出：

- `VERDI_HOME`、`LD_LIBRARY_PATH`。
- `LSF_ENVDIR`、`LSF_BINDIR`、`LSF_LIBDIR`、`LSF_SERVERDIR` 和完整 `PATH`。
- Synopsys/Cadence/Mentor 等 EDA license 变量。
- `XVERIF_HOME`、`PYTHONPATH` 和 LSF backend/queue 配置。

这对 tool_wave 尤其重要：FFR 动态库和 NPI 都应在计算节点的 worker 进程中加载，查询端不应因为自身没有 Verdi 运行库而无法启动。license 是否签出由计算节点实际 backend 决定，不能仅根据 bsub 提交成功判断。

## 5. xdebug 的 file transport

### 5.1 机制

file transport 是 xverif 的另一条跨节点路径。session daemon 仍在计算节点运行，但不监听需要跨节点访问的 TCP/UDS；client 和 daemon 共享一个 session transport 目录：

```text
transport/
  requests/    client 发布的待处理请求
  claims/      worker 原子领取后的请求
  responses/   worker 写入的响应
  done/        已完成历史
  failed/      超时、过期、坏请求和 stale claim
  tmp/         原子写入临时文件
  heartbeat/   worker 存活文件
```

请求先写入 `tmp/`，完成后用 rename 原子发布到 `requests/`。worker 用 rename 把请求移到 `claims/`，从而避免两个 worker 重复消费。响应写入 `responses/`，client 读取后默认归档到 `done/`。请求超过 deadline、claim 长时间没有完成或 JSON 无效时，进入 `failed/` 并保留原因。

### 5.2 超时和清理

xverif 当前默认值为：普通请求 300 秒、ping/quit 2 秒、轮询间隔 20 毫秒、单个 JSON 64 MiB、claim 超时取 `max(2 * request_timeout, 600s)`、成功历史保留 7 天、失败历史保留 30 天。worker 周期性写 heartbeat 并扫描 stale claim。

这种设计不依赖传统 file lock，适合 NFS 等共享目录，但依赖共享文件系统的 rename、可见性和权限语义。transport 目录应按用户/session 隔离并限制权限；共享目录本身不是身份认证机制。

### 5.3 与 LSF 的关系

file transport 与 LSF 可以组合：

```text
登录节点 client
  → 共享目录写 request
计算节点 LSF job
  → 读取 request，调用 Fsdb/NPI/FFR，写 response
```

此时 LSF 负责作业调度和生命周期，file transport 负责查询通道。它不要求计算节点 TCP 可达，也不要求 client 运行 NPI；但 FSDB、EDA 安装、license 和 FFR/NPI worker 必须在计算节点可用。

## 6. 与当前 tool_wave 的差异

当前 tool_wave 的真实结构是：

```text
vwave open
  → 本地 fork + exec vwave-npi-worker 或 vwave-ffr-worker --serve
  → worker 加载 FSDB，写 session.json/PID/source_info
  → tw::server_loop 监听 .vtool/wave_run/wave_server.sock

vwave query
  → 从 CWD/--run-dir 找运行目录
  → 本机 PID 检查
  → 连接 UDS，发送一次 JSON 请求，读取响应后关闭连接
```

因此存在三个直接差异：

1. `vwave` 查询协议是每次连接一个请求，worker 没有 xverif 的长期 stdin/stdout JSONL loop。
2. 服务发现和存活判断依赖本机 PID 与 UDS 路径；共享目录只能让 client 看见文件，不能让另一个节点连接该 UDS。
3. `vwave open` 的 daemon 由 CLI 本地 fork，不能直接等待一个跨节点的 LSF job 并把后续 CLI 命令绑定到它的 stdin/stdout。

所以，下面的做法都不够：

- 仅把 `bsub -I` 添加到当前 `open`：作业里的 worker 仍监听计算节点 UDS，登录节点后续命令没有查询通道。
- 仅共享 `.vtool/wave_run`：PID 只在计算节点有效，UDS 也不能跨主机连接。
- 仅把 FSDB 路径放到共享盘：这只解决数据可见性，不解决请求通道和生命周期。

## 7. 适配方案比较

| 方案 | 通道 | 对当前代码改动 | 适用场景 | 主要风险 |
|---|---|---:|---|---|
| 本地 direct | UDS | 无 | 单机现有用法 | 保留当前限制 |
| xverif 式 LSF stdio | bsub 连接 stdin/stdout | 中到大 | MCP server 或长期代理持有 session | 需要 stdio-loop；bsub 输出和后端 stdout 必须严格隔离 |
| LSF + file transport | 共享目录 request/response | 中 | 登录节点不可直连计算节点，且有共享 FS | NFS rename/权限/历史清理需要验证 |
| LSF + TCP/SSH 隧道 | TCP | 大 | 集群网络或 SSH 可用 | 认证、端口、隧道生命周期和网络策略 |

推荐优先级：

1. 保留 direct UDS 和 NPI/FFR 双后端，保证本地行为不变。
2. P0 实现 LSF + file transport，复用现有 JSON handler 和 `WaveBackend`，以共享目录完成远程请求。
3. P1 增加 `vwave-stdio-worker` 或 `--stdio-loop`，供 MCP wrapper 采用 xverif 的 `bsub -I` 模式；不要让普通多次执行的 `vwave` CLI 直接依赖一次性的 bsub stdin。
4. TCP/SSH 作为网络可达时的补充，不作为 LSF 机制的前置条件。

## 8. tool_wave 的实施计划

### P0：抽象 transport 和远程 session

- 把当前 UDS client/server 的请求帧抽象为 `WaveTransport`，保留现有命令和 JSON 响应格式。
- 新增 `file` transport 状态目录，至少实现 `requests/claims/responses/tmp/heartbeat/failed/done`。
- worker 通过原子 rename 领取请求，先检查 request deadline，再调用现有 handler；client 读取响应后归档。
- `session.json` 增加 `transport`、`file_dir`、`job_id`、`job_name`、`host` 和协议版本；PID 只作诊断，不作为跨节点存活判断。
- `open` 在 LSF 模式提交计算节点 worker，等待 heartbeat/ready 文件；`close` 发送 shutdown，确认失败时按 job ID 回收。
- LSF 提交输出同时保留 stdout/stderr；job ID 解析失败时使用唯一 job name，不能按共享目录里的 PID 执行本机 kill。

### P1：stdio loop 和 MCP 集成

- 给 FFR/NPI worker 增加 `--stdio-loop`，ready 行和 request/response JSONL 使用 stdout，诊断日志全部走 stderr。
- 复用 `WaveBackend`、handlers 和 session 错误码；同一 session 以 mutex 串行，多 session 由 launcher 并行。
- 增加 `LsfLauncher`、可配置 queue/resource、startup/request/close/bkill timeout，以及 `xwave-lsf-doctor` 类诊断命令。
- job 名包含用户、MCP PID、随机前缀、backend、session alias；优先 `bkill <job_id>`，无 ID 时才按 job name 回收。
- 普通业务错误保持 session alive；transport、worker 崩溃和 terminal backend error 统一为 `SESSION_LOST` 并清理。

### P2：生产化验收

- fake bsub 覆盖排队延迟、stdout/stderr 噪声、ready 前后退出、子进程泄漏和 job ID 丢失。
- 真实 LSF 覆盖多 session 并发、同 session 串行、队列等待、bkill 回收、节点被回收和 worker 超时。
- 共享 FS 覆盖原子 rename、旧 request、stale claim、client 超时、目录权限和跨节点缓存可见性。
- 验证 FFR 运行库、NPI license、FSDB 路径和 job 环境都在计算节点生效；登录节点只运行无厂商依赖的 client/proxy。

## 9. 证据和局限

本报告依据 xverif 当前源代码、README、LSF/file transport 参考文档、单元测试和历史提交分析，没有在本次检查中提交真实 LSF 作业。fake LSF 测试只能证明 launcher 和状态机的本地行为，不能证明具体集群的队列配置、共享文件系统语义、license 环境或作业回收策略。

关键参考：

- [`xverif_mcp/lsf/bsub.py`](../../xverif/xverif_mcp/src/xverif_mcp/lsf/bsub.py)
- [`xverif_mcp/lsf/protocol.py`](../../xverif/xverif_mcp/src/xverif_mcp/lsf/protocol.py)
- [`xverif_mcp/sessions/launchers.py`](../../xverif/xverif_mcp/src/xverif_mcp/sessions/launchers.py)
- [`xverif_mcp/sessions/loop_session.py`](../../xverif/xverif_mcp/src/xverif_mcp/sessions/loop_session.py)
- [`xverif_mcp/sessions/session_manager.py`](../../xverif/xverif_mcp/src/xverif_mcp/sessions/session_manager.py)
- [`xdebug LSF 参考`](../../xverif/xdebug/skill/references/lsf-mcp.md)
- [`xdebug file transport 参考`](../../xverif/xdebug/skill/references/file-transport.md)
- [`xverif_mcp README`](../../xverif/xverif_mcp/README.md)
- [`xdebug file_exchange 实现`](../../xverif/xdebug/src/core/transport/file_exchange.cpp)
