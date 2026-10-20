# vwave/vsignal 源码复用与治理方案

## 结论

两套工具适合共享运行时基础设施，不适合把业务后端合并成一个“大工具”。当前代码已经共享 `src_common/tw` 下的 JSON、协议基础、client、daemon、运行目录和 UDS server loop；本轮继续把 LSF 传输层中完全一致的辅助函数抽到公共层。

## 复用边界

| 代码 | 结论 | 原因 |
|---|---|---|
| `src_common/*.h` 的 JSON/client/daemon/server loop | 继续共享 | 与波形或网表业务无关 |
| worker 参数 hex 编码/解码 | 已抽到 `tw/transport.h` | 两个 worker 的边界协议相同 |
| LSF 命令拆分、PID/status 字段 | 已抽到 `tw/lsf.h` | 输入输出语义相同，适合集中修复 |
| vwave/vsignal `lsf_proxy.h` 中的 RemoteProcess | 暂不合并 | vwave 需要 reader 初始化、stdout 隔离和更强的退出回收；vsignal 是 NPI/KDB 协议 |
| `common/session.h` | 保持工具专用 | vwave 保存 reader、库指纹和 FSDB 元数据；vsignal 保存 KDB/source 参数 |
| `server/handlers.h`、backend/NPI/FFR | 保持工具专用 | 命令集合、错误语义和厂商 ABI 不同 |
| `common/protocol.h` | 保持工具专用，底层响应 helper 共享 | command/error code 是产品 API，不能因代码相似而耦合 |
| `common/run_dir.h` | 保持轻量 wrapper | `wave_run`/`vsignal_run`、source 文件和清理项不同 |

## 治理规则

1. `src_common` 只能依赖标准库和 `tw` 自身；不能 include `src_vwave`、`src_vsignal` 或厂商 NPI/FFR 头文件。
2. 工具层只通过 `src_common` 提供的 transport/daemon/server/client API 访问进程与套接字；LSF、NPI、FFR 的业务适配留在各自目录。
3. 新增跨工具函数先放入公共头文件并补最小单元/烟测；禁止复制到另一套 `common/` 后再修两个版本。
4. session schema、stdio protocol 名称和 command/error code 是稳定接口；重构时保持兼容，必要时增加 schema/version 字段而不复用另一工具的 schema。
5. 版本专用 worker 继续由构建规则明确绑定 NPI RPATH；公共层不得感知 `VERDI_HOME`、FSDB reader 或 KDB 结构。

## 本轮变更

- 新增 `src_common/transport.h`：统一 hex worker session 编码。
- 新增 `src_common/lsf.h`：统一 LSF command split、PID 替换和 status 字段注入。
- vwave/vsignal 两套 proxy 和 worker 改用公共 helper，保留各自 RemoteProcess、session、backend 和协议实现。

## 后续阶段

- 增加 `make check-source-governance`，检查公共层反向依赖、重复 transport helper 和发布包 worker 清单。
- 把两套 proxy 的公共生命周期抽象为可组合的 transport adapter，前提是保留 vwave 的 stdout 隔离、超大响应限制、强制回收和 vwave/vsignal 协议校验。
- 为公共 helper 增加独立 C++ smoke test，避免只能通过完整 EDA/NPI 回归发现命令行转义或状态字段回归。
