# tool_wave 调试能力与 AI 调用可行性分析

## 1. 结论摘要

当前仓库里的两个工具已经具备较强的离线调试基础能力，但定位要说清楚：

- `vwave` 解决的是时域观测问题，适合回答“某个时间点/时间范围内，某些信号的值是什么”。
- `vsignal` 解决的是结构连接问题，适合回答“这个信号是谁驱动的、又驱动了谁、跨层路径怎么走”。
- 两者组合后，已经能覆盖芯片调试中最常见的两类查询：`现象定位` 和 `根因追踪`。
- 对人工调试来说，这套工具已经可用，尤其适合批处理、脚本化和远程无 GUI 环境。
- 对 AI 调用来说，这套工具也已经具备基础可用性，因为它们都支持稳定 CLI、守护进程复用和 `--json` 结构化输出。
- 但它们目前更像 “AI 可编排的底层原语”，还不是 “面向 AI 的完整调试平台”。缺少统一编排接口、跨工具关联查询、会话记忆、结果排序、语义封装和 MCP/HTTP 服务层。

一句话判断：

**当前版本可以支持调试，也可以支持 AI 调用，但更适合作为 AI Agent 的底层执行工具，而不是开箱即用的完整智能调试系统。**

---

## 2. 两个工具分别解决什么问题

### 2.1 vwave

`vwave` 面向 FSDB 波形文件，核心能力是：

- 打开并驻留加载 FSDB，避免重复打开大波形文件。
- 查询文件信息，如时间范围、scale unit、版本。
- 浏览层次结构和 scope。
- 列出某个 scope 下的信号。
- 查询单点取值。
- 查询时间范围内的变化序列。
- 支持多信号批量取值。
- 支持文本和 JSON 输出。

它对应的是典型的功能调试场景：

- 某个寄存器在某个时刻为什么是这个值。
- 某个握手信号是否在窗口内拉高过。
- 某组关键信号在错误发生时是否一致。
- 某个 interface 下有哪些信号可以进一步分析。

### 2.2 vsignal

`vsignal` 面向 KDB 或 RTL 设计数据，核心能力是：

- 加载 VCS `-kdb` 生成的设计数据库，或者直接加载 RTL。
- 查询某个信号的直接驱动。
- 查询某个信号的直接负载。
- 做寄存器级 `fanin/fanout` 追踪。
- 做两点间路径追踪。
- 查询实例端口的上下层连接。

它对应的是典型的结构调试场景：

- 某个异常信号究竟是谁驱动的。
- 某个顶层输入最终影响到了哪些寄存器。
- 某个模块输出为什么没有传到目标模块。
- 一个实例端口到底接到了哪些上层/下层信号。

### 2.3 组合价值

单独看两个工具都只是半套调试能力：

- 只有 `vwave`，能看到“错了”，但不一定能快速知道“是谁造成的”。
- 只有 `vsignal`，能看到“连通关系”，但不能回答“错误发生时真实值是什么”。

组合后才形成完整闭环：

1. 用 `vwave` 观察异常值和发生时刻。
2. 用 `vsignal` 追溯驱动链、寄存器链、模块边界。
3. 再回到 `vwave` 验证路径上关键节点在相同时间窗口内的值。

这已经是一个典型的离线数字调试工作流。

---

## 3. 当前是否“支持调试”

### 3.1 支持的调试类型

当前实现支持的是 `离线、查询式、证据驱动` 调试，不是交互式单步调试。

已经支持：

- 仿真后离线分析。
- 波形值确认。
- 设计层次浏览。
- 网表连接追踪。
- 根因缩小范围。
- 脚本化批处理。
- AI Agent 自动串联多个查询。

不支持或尚不完整：

- 像 GUI 波形工具那样的可视化联动浏览。
- 运行中的在线仿真控制。
- 断点、单步执行、强制改值。
- 自动生成调试策略或故障排名。
- 时域和结构域的一体化联合查询。
- 多设计、多波形、多会话并发编排能力。

### 3.2 调试成熟度判断

如果用工程视角给一个判断：

- `vwave` 已达到“可投入日常离线波形查询”的成熟度。
- `vsignal` 已达到“可投入结构路径与驱动负载分析”的成熟度。
- 整个项目已达到“可支撑 AI 调试助手原型或团队内部工具链”的成熟度。
- 但距离“企业级智能调试平台”还差一层服务化和产品化封装。

---

## 4. AI 调用是否可行

## 4.1 结论

**可行，而且从当前代码结构看，是明确朝 AI 友好方向设计过的。**

判断依据有四个：

- 所有核心命令都能通过 CLI 触发。
- 两个工具都支持 `--json` 输出。
- 两个工具都采用驻留式 daemon，适合 Agent 在一个调试会话里反复查询。
- 运行目录可自动发现，Agent 不需要每次都重新传完整上下文。

这意味着 AI 不需要链接 C++ 库，也不需要理解 NPI 细节，只要能执行 shell 命令并解析 JSON，就能稳定调用。

### 4.2 AI 友好的点

当前实现中，对 AI 最关键的友好点如下：

#### 1. JSON 协议统一

公共请求构造在 `src_common/client.h` 中，核心请求格式是：

```json
{"id":1,"cmd":"trace_driver","params":{"signal":"top.data_out"}}
```

响应统一为：

- 成功：`{"id":1,"status":"ok","data":...}`
- 失败：`{"id":1,"status":"error","error":{"code":"...","message":"..."}}`

这个格式非常适合 AI 做：

- 结果判定。
- 错误重试。
- 多轮链式调用。
- 最终总结生成。

#### 2. 守护进程复用

两个工具都不是每次查询都重新加载数据，而是：

- `open` 时启动 daemon。
- 后续命令通过 Unix Domain Socket 发 JSON 请求。
- `close` 时结束。

这对 AI 很重要，因为：

- FSDB 和 KDB 加载都比较重。
- AI 调试往往不是一次查询，而是 5 到 20 次递进式查询。
- 若每次都冷启动，延迟和资源成本会非常差。

#### 3. 自动发现运行目录

`vwave` 自动向上查找 `.vtool/wave_run/`，`vsignal` 自动向上查找 `.vtool/vsignal_run/`。

这让 AI 在项目目录内部执行查询时，不必每次都记住 socket 路径或 pid 文件。

#### 4. 错误语义相对清晰

已有的错误码包括：

- `INVALID_PARAMS`
- `INTERNAL_ERROR`
- `SIGNAL_NOT_FOUND`
- `FSDB_OPEN_FAILED`
- `DESIGN_LOAD_FAILED`
- `INSTANCE_NOT_FOUND`
- `NO_PATH`

这让 AI 能做有限但有效的分流，例如：

- `SIGNAL_NOT_FOUND` 时先回退去 `signals` 或 `scopes` 探测。
- `NO_PATH` 时切换到 `driver/load/fanin/fanout` 局部追踪。
- `DESIGN_LOAD_FAILED` 时提醒检查 KDB 或许可环境。

### 4.3 当前对 AI 不够友好的点

虽然可用，但还不算完整的 AI-first 接口，主要缺口有：

#### 1. 没有统一服务接口

当前只能通过 CLI 调用，没有：

- HTTP API
- gRPC
- MCP Server
- Python SDK

这意味着 AI 侧必须具备 shell 执行能力。

#### 2. 没有跨工具联合查询

AI 需要自己编排：

- 先 `vwave get-value`
- 再 `vsignal driver`
- 再 `vwave get-value`

工具本身没有提供类似下面的高阶命令：

- “对异常信号自动做值追踪 + 结构追踪”
- “沿 trace path 自动抓关键节点波形”
- “从失败时间点反推驱动根因”

#### 3. 输出 schema 还比较轻

虽然有 JSON，但目前仍偏“工程可读”，还不是“强 schema、强类型、易供 Agent 自动消费”的接口。

例如还缺：

- schema version
- 更稳定的字段约束
- 更丰富的元数据
- 对空结果、部分结果、降级结果的标准表达

#### 4. 缺少批量诊断工作流

AI 常见需求不是单个查询，而是任务流：

- 给出 10 个 suspect signals。
- 自动找第一个异常时间点。
- 拉出上下游链路。
- 产出结构化结论。

当前这些都要由上层 agent 自己编排。

---

## 5. 实际使用场景梳理

## 5.1 场景一：功能波形定位

场景：某个回归失败，怀疑 `tb.dut.resp_valid` 在 1250000ps 没有按预期拉高。

流程：

1. `vwave open test_vwave/tb_top.fsdb --json`
2. `vwave signal-info tb.dut.resp_valid --json`
3. `vwave get-value -s tb.dut.resp_valid -t 1250000 --json`
4. 若值异常，再查时间范围：
   `vwave get-value -s tb.dut.resp_valid -b 1000000 -e 1300000 --json`

适用价值：

- 定位“有没有变化”。
- 定位“变化发生在什么时候”。
- 快速替代打开 GUI 手工翻波形。

## 5.2 场景二：结构根因追踪

场景：`top.data_out` 错了，但不知道驱动源在哪里。

流程：

1. `vsignal open -dbdir simv.daidir --json`
2. `vsignal driver top.data_out --json`
3. 如果结果穿不过 assign，则：
   `vsignal driver top.data_out --assign-cell --json`
4. 如果跨模块仍不够，再：
   `vsignal driver top.data_out --assign-cell --pass-mod --json`
5. 对关键中间节点继续做 `fanin`。

适用价值：

- 快速找到直接驱动者。
- 判断问题在模块内部还是模块边界。
- 替代手工翻 RTL、手工 grep 连接。

## 5.3 场景三：寄存器级传播分析

场景：某个控制信号没有传播到预期状态机，想知道影响路径。

流程：

1. `vsignal fanout top.ctrl_en --json`
2. 如果需要限定模块范围：
   `vsignal fanout top.ctrl_en --scope top.u_sub --json`
3. 对目的寄存器反向做：
   `vsignal fanin top.u_sub.state_reg --json`

适用价值：

- 看传播终点。
- 看寄存器级依赖关系。
- 做 CDC、控制路径、时序调试前的结构摸底。

## 5.4 场景四：实例接口理解

场景：接手一个陌生 IP，需要先理解 `top.u_sub` 的端口接线。

流程：

1. `vsignal conn top.u_sub --json`
2. 如需底层连接视图：
   `vsignal conn top.u_sub --level low --json`

适用价值：

- 快速建立模块边界认知。
- 帮 AI 或工程师生成模块连接摘要。
- 用于 review 或 debug 前的结构探索。

## 5.5 场景五：波形与结构联合调试

场景：`top.data_out` 在错误时间点值异常，需要定位根因。

建议流程：

1. 用 `vwave get-value` 确认异常时刻。
2. 用 `vsignal driver` 找到直接驱动。
3. 用 `vsignal fanin` 找到上游寄存器或主端口。
4. 再用 `vwave get-value` 对这些关键节点在同一时刻取值。
5. 根据不一致点缩小根因范围。

这是当前项目最有价值的实际落地场景，也是 AI 调试最容易做出效果的场景。

---

## 6. 实际流程与调用链

## 6.1 vwave 调用链

### 用户侧流程

```text
用户 / AI
  -> vwave open <fsdb>
  -> vwave info/scopes/signals/get-value
  -> vwave close
```

### 程序内部调用链

```text
CLI 参数解析
  -> resolve_run_dir / cmd_open / cmd_query
  -> build_request(id, cmd, params)
  -> send_request(socket, json)
  -> UDS 发送到 daemon
  -> dispatch_request()
  -> handle_xxx()
  -> npi_fsdb_* API
  -> make_ok_response / make_error_response
  -> send_line(response)
  -> client print_response
  -> 用户 / AI 消费结果
```

### 对应实际代码模块

- CLI 入口：`src_vwave/main.cpp`
- 请求构造与发送：`src_common/client.h`
- 协议常量：`src_vwave/common/protocol.h`
- 运行目录与自动发现：`src_vwave/common/run_dir.h`
- 服务端分发与 NPI 调用：`src_vwave/server/server_core.h`
- 通用事件循环：`src_common/server_loop.h`

## 6.2 vsignal 调用链

### 用户侧流程

```text
用户 / AI
  -> vsignal open -dbdir simv.daidir
  -> vsignal driver/load/fanin/fanout/trace/conn
  -> vsignal close
```

### 程序内部调用链

```text
CLI 参数解析
  -> cmd_open / cmd_query
  -> build_request(id, cmd, params)
  -> send_request(socket, json)
  -> UDS 发送到 daemon
  -> dispatch_request()
  -> handle_trace_driver / handle_fanin_reg / ...
  -> npi_nl_* / npi_inst_* / npi_L1 API
  -> make_ok_response / make_error_response
  -> client print_response
  -> 用户 / AI 消费结果
```

### 对应实际代码模块

- CLI 入口：`src_vsignal/main.cpp`
- 请求构造与发送：`src_common/client.h`
- 协议常量：`src_vsignal/common/protocol.h`
- 运行目录与自动发现：`src_vsignal/common/run_dir.h`
- 服务端分发与 NPI 调用：`src_vsignal/server/server_core.h`
- 通用事件循环：`src_common/server_loop.h`

## 6.3 AI 调用链

如果是 Agent 调用，真实链路通常会变成：

```text
用户问题
  -> AI 规划查询步骤
  -> shell 执行 vwave / vsignal --json
  -> 工具输出 JSON
  -> AI 解析结果
  -> AI 决定下一跳命令
  -> 汇总为人可读结论
```

典型例子：

```text
“top.data_out 在 1.25us 为什么错了？”
  -> vwave get-value top.data_out @ 1.25us
  -> vsignal driver top.data_out
  -> vsignal fanin <关键驱动点>
  -> vwave get-value <关键驱动点们> @ 1.25us
  -> AI 生成结论
```

所以从架构上说，这两个工具已经天然适合作为 Agent 的“观测器”和“追踪器”。

---

## 7. AI 使用时的推荐编排模式

推荐把 AI 调试拆成 4 层：

### 7.1 探测层

先确认会话资源是否就绪：

- `vwave status --json`
- `vsignal status --json`

如果未启动，再执行 `open`。

### 7.2 发现层

先摸清上下文：

- `vwave info/scopes/signals`
- `vsignal info/conn`

这一步是为了防止 AI 直接对错误路径、错误层次名发请求。

### 7.3 分析层

根据问题类型选择工具：

- 时间点/窗口问题，用 `vwave`
- 驱动/路径/实例连接问题，用 `vsignal`
- 根因分析，两个交替使用

### 7.4 汇总层

AI 对 JSON 结果进行：

- 异常时刻总结
- 上下游链路摘要
- 候选根因排序
- 下一步建议

这比让 AI 直接读大量 RTL 或手工读波形更稳。

---

## 8. 当前方案是否满足 AI 调试需求

## 8.1 能满足的部分

如果把需求定义为：

“让 AI 在已有仿真产物上，自动做波形查询、连接追踪和问题总结。”

那么当前方案已经基本满足。

原因是它已经具备：

- 稳定 CLI 接口。
- JSON 输出。
- 守护进程复用。
- 明确错误码。
- 覆盖时域和结构域两类关键查询。

## 8.2 还不能完全满足的部分

如果把需求定义为：

“让 AI 像一个成熟 debug 平台一样，自动从失败日志一路定位到根因，并给出高置信结论。”

那么当前还不完全满足，缺口主要在上层：

- 没有统一任务编排层。
- 没有与 testcase、日志、断言、coverage 联动。
- 没有结果缓存与 case memory。
- 没有自动选择关键观测点的策略层。
- 没有调试知识库或规则库。

也就是说，**底层观测能力基本够了，智能化闭环能力还没补齐。**

---

## 9. 国内是否能满足需求

## 9.1 从“技术可行性”看

在国内，`技术上可以满足`，但前提比较强。

前提条件包括：

- Linux 环境。
- Verdi NPI 运行环境。
- `VERDI_HOME` 和动态库可用。
- 对 `vsignal` 来说，还需要 VCS 产出的 KDB，或者至少能用 RTL 加载。
- 企业内网允许 AI Agent 执行命令行工具。

只要这些条件满足，国内团队完全可以部署和使用这套工具。

## 9.2 从“行业现实”看

在国内数字芯片设计/验证团队中，这类需求是真实存在的：

- 大量调试工作本质上就是“看波形 + 追连接”。
- 很多团队希望把经验型 debug 逐步转成脚本化、自动化、AI 辅助化。
- 尤其在大回归、夜间批量失败分析、远程服务器环境下，CLI + JSON 的价值很高。

所以从需求侧看，国内是有明确落地空间的。

## 9.3 真正的落地约束

现实约束主要不在代码本身，而在生态和环境：

### 1. 商业 EDA 依赖强

当前方案强依赖 Synopsys 生态：

- FSDB
- Verdi NPI
- VCS KDB

这意味着：

- 大厂、成熟 IC 公司通常具备条件。
- 中小团队、初创团队、没有对应 license 的团队，很难直接落地。

### 2. 国产替代链路目前不完整

如果用户想完全脱离 Synopsys，当前方案不能直接满足。

原因是当前工具本身并不是一个通用波形/网表抽象层，而是直接绑在 NPI API 上。

所以在“国产完全替代”场景下，答案是：

- 当前方案不满足。
- 需要重做后端适配层，支持新的波形格式和新的结构数据库接口。

### 3. AI 接入在国内通常需要私有化

国内很多芯片公司对设计数据安全要求高，因此：

- 工具本身适合私有化部署。
- AI 调用更适合接本地 Agent、本地模型或企业内网模型服务。
- 不太适合把设计数据直接送到公网模型环境中。

这反而说明当前 CLI + 本地 JSON 的架构是合理的，因为它天然适合内网环境。

## 9.4 最终判断

“国内能否满足需求” 要分两层回答：

- 如果是 `已有 Synopsys 仿真/调试环境的芯片团队`，当前方案可以满足，而且很有现实价值。
- 如果是 `希望完全脱离商业 EDA、走纯国产/开源链路的团队`，当前方案暂时不能满足。

因此更准确的说法是：

**当前方案在国内“有条件可落地”，不是“无条件普适可落地”。**

---

## 10. 当前方案的主要优点

- 架构清晰，CLI 和 daemon 分离。
- `vwave` 与 `vsignal` 边界清晰，一个看值，一个看连接。
- 共用 `src_common`，减少重复实现。
- JSON 输出让自动化和 AI 接入门槛低。
- 守护进程复用降低大文件/大设计重复加载成本。
- 运行目录自动发现适合项目内调试。
- 错误码和返回结构基本统一。
- 已有测试脚本覆盖基本工作流。

---

## 11. 当前方案的主要短板

- 没有面向 AI 的统一服务层。
- 没有跨工具联合调试命令。
- 输出 schema 还不够强约束。
- 缺少日志、断言、回归结果、coverage 联动。
- 没有自动根因排序和诊断策略。
- 对外部商业 EDA 环境依赖重。
- `vsignal` 在真实大设计中的性能和结果噪声控制还需要更多工程实践验证。

---

## 12. 建议的演进方向

如果目标是“让 AI 真正稳定用于调试”，建议按下面顺序演进：

### 第一阶段：把当前 CLI 工具变成稳定的 AI 工具底座

- 固化 JSON schema。
- 补充更多错误码与错误上下文。
- 增加统一的 machine-readable 文档。
- 给典型查询增加示例与最佳实践。

### 第二阶段：增加统一编排层

- 提供 Python SDK 或 MCP Server。
- 提供跨工具的高阶命令。
- 支持一个 session 里同时管理 `wave + design + testcase`。

### 第三阶段：构建智能调试流

- 接入 regression log。
- 接入断言失败点。
- 自动选关键时间点和关键观测信号。
- 自动输出 root-cause report。

---

## 13. 最终判断

综合来看：

1. 当前两个工具已经能支持数字设计中的核心离线调试任务。
2. 它们对 AI 调用是友好的，尤其适合命令执行型 Agent。
3. 当前最适合的定位，是 AI 调试系统的“底层执行引擎”。
4. 在国内，有 Synopsys 环境的团队具备明显落地条件。
5. 若要面向更广泛团队或更强自动化目标，下一步必须补服务化和智能编排层。

换句话说：

**现在这套工具已经不是“能不能用”的问题，而是“作为 AI 调试底座已经可用，但距离规模化产品化还有一层平台能力要补”。**