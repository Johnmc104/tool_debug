# tool_wave v1.5 源码工程分析报告

日期：2026-08-14
范围：src_common/、src_vwave/、src_vsignal/ 全部源码

---

## 1. 文件规模与函数规模

### 1.1 文件行数

| 文件 | 行数 | 评估 |
|------|------|------|
| src_vwave/server/server_core.h | 724 | **偏大**，handlers + NPI 调用 + run_server 混合 |
| src_vwave/main.cpp | 630 | **偏大**，参数解析 + cmd_open/close/query + main 混合 |
| src_vsignal/main.cpp | 623 | **偏大**，同上结构 |
| src_vsignal/server/server_core.h | 479 | 中等 |
| src_common/json.h | 260 | 合理 |
| src_common/server_loop.h | 244 | 合理 |
| src_common/run_dir.h | 211 | 合理 |
| src_common/client.h | 187 | 合理 |
| 其他（protocol/json_parser/run_dir/client_core） | 15-75 | 精简 |

**目标**：单文件控制在 400 行以内，server_core.h 和 main.cpp 需要拆分。

### 1.2 超大函数（>100 行）

| 函数 | 文件 | 行数 | 问题 |
|------|------|------|------|
| `cmd_query()` | vwave/main.cpp | 175 | 所有命令的 if-else 链，每个分支 10-30 行 |
| `cmd_query()` | vsignal/main.cpp | 175 | 同上，增加了批量查询逻辑后更复杂 |
| `main()` | vsignal/main.cpp | 151 | 参数解析 + 信号收集 + 路径分发 |
| `cmd_open()` | vwave/main.cpp | 149 | 服务器检测 + fork + 等待就绪 |
| `cmd_open()` | vsignal/main.cpp | 142 | 同上，增加了 LD_LIBRARY_PATH 逻辑 |
| `main()` | vwave/main.cpp | 126 | 参数解析 |

**目标**：单函数控制在 80 行以内。

---

## 2. 参数传递分析

### 2.1 最严重的参数膨胀

**vwave cmd_query 签名（17 个参数）**：
```cpp
static int cmd_query(
    const wave::RunDir& run_dir, bool json_mode,
    const std::string& command,
    const std::string& scope_path,
    const std::string& signal_name,
    const std::vector<std::string>& extra_signals,
    const std::string& signal_file,
    int64_t time_val, int64_t begin_time, int64_t end_time,
    const std::string& radix,
    bool compact_mode,
    const std::string& find_scope,
    int depth,
    const std::string& edge_type,
    const std::string& edge_dir,
    int64_t limit_val);
```

**vsignal cmd_query 签名（12 个参数）**：
```cpp
static int cmd_query(
    const vsignal::RunDir& run_dir, bool json_mode,
    const std::string& command,
    const std::vector<std::string>& signals,
    const std::string& positional2,
    bool assign_cell, bool pass_mod,
    bool stop_at_pin, bool report_primary_port,
    const std::string& scope,
    const std::string& level,
    bool compact_mode, int64_t limit_val);
```

### 2.2 优化方案

引入 Options struct，将解析后的参数打包传递：

```cpp
struct QueryOptions {
    // 通用
    bool json_mode = false;
    bool compact_mode = true;
    int64_t limit_val = 50;

    // vwave 特有
    int64_t time_val = -1;
    int64_t begin_time = -1;
    int64_t end_time = -1;
    std::string radix = "bin";
    int depth = 1;
    std::string edge_type = "any";
    std::string edge_dir = "forward";

    // vsignal 特有
    bool assign_cell = false;
    bool pass_mod = false;
    bool stop_at_pin = false;
    bool report_primary_port = false;
    std::string scope;
    std::string level = "high";
};
```

可以分层：通用 `tw::QueryOptions` + 工具特化子类，也可以各工具独立定义。

---

## 3. 耦合分析

### 3.1 代码重复

| 重复模式 | 位置 | 行数 | 说明 |
|---------|------|------|------|
| cmd_open 逻辑 | vwave/main + vsignal/main | ~150×2 | 服务器检测/fork/等待，结构几乎相同 |
| cmd_close 逻辑 | vwave/main + vsignal/main | ~30×2 | 完全相同 |
| main() 参数解析框架 | vwave/main + vsignal/main | ~130×2 | 选项解析 for 循环结构相同 |
| json_parser.h | vwave/common + vsignal/common | 15×2 | 仅 namespace 别名不同 |
| protocol.h 结构 | vwave/common + vsignal/common | ~55×2 | 框架相同，命令/错误码不同 |
| run_dir.h | vwave/common + vsignal/common | ~75×2 | 框架相同，DOT_DIR/PREFIX 不同 |
| client_core.h | vwave/client + vsignal/client | ~20×2 | 仅 using 声明 |

**总重复量**：约 475 行 × 2 = 950 行中约 475 行是结构性重复。

### 3.2 已有的共享基础设施（src_common/）

| 模块 | 行数 | 复用者 |
|------|------|--------|
| json.h (JsonObject/JsonParser) | 260 | 两工具的 server + client |
| server_loop.h (create_and_run_loop) | 244 | 两工具的 server |
| client.h (send_request/print_response) | 187 | 两工具的 client |
| run_dir.h (tw::RunDir) | 211 | 两工具的 RunDir wrapper |
| protocol.h (make_ok/error_response) | 58 | 两工具的 server |

共享层设计良好，覆盖了网络 I/O、JSON、运行目录管理。

### 3.3 可提取到 src_common/ 的重复代码

| 候选 | 来源 | 预估节省 |
|------|------|---------|
| cmd_open 模板（fork + daemon + wait） | vwave/vsignal main.cpp | ~120 行 |
| cmd_close 模板 | vwave/vsignal main.cpp | ~30 行 |
| 参数解析框架 | vwave/vsignal main.cpp | ~50 行（通用选项部分） |
| 多信号批量聚合逻辑 | vsignal cmd_query batch | 可泛化为 tw::client::batch_query() |

---

## 4. 数据结构与性能

### 4.1 当前数据结构

- **NPI handle 向量**：`nlHdlVec_t` (std::vector) 由 NPI 填充，逐个转 JSON 字符串。无中间缓存。
- **JSON 构建**：全部用 `std::ostringstream` 字符串拼接。无 DOM 构建和序列化分离。
- **请求/响应**：完整 JSON 字符串在内存中构建，通过 UDS 单次发送。无分块传输。

### 4.2 性能瓶颈分析

| 瓶颈 | 场景 | 影响 |
|------|------|------|
| NPI 调用无缓存 | 多次查询同一信号的 driver/load | 每次都调 NPI，无本地缓存 |
| JSON 字符串拼接 | 大 fanout（164 条） | ostringstream 多次 realloc |
| 客户端批量：N 次 socket 连接 | batch query 10 信号 | 每信号一次 connect/send/recv |
| handle_list_signals 全量遍历 | 大 scope 下信号列表 | 无分页，全量迭代后截断 |

### 4.3 缓存策略建议

**适合缓存的数据**（设计数据在 server 生命周期内不变）：
- `info` 结果（设计元数据，加载后固定）
- `conn` 结果（实例端口连接，网表静态数据）
- 信号名到 NPI handle 的映射（避免重复 `npi_fsdb_sig_by_name`）

**不适合缓存的数据**：
- FSDB 波形值查询（时间参数每次不同）
- NPI NL trace 结果（参数组合多变）

**建议实现**：server 端 `std::unordered_map<std::string, npiHandle>` 缓存信号/scope handle 查找结果。对 vwave 的 `sig_by_name` 调用，同一信号多次 `get` 时避免重复 NPI 查找。

### 4.4 批量查询的连接复用

当前 vsignal batch 对每个信号创建新的 socket 连接。优化方向：

- **短期**：复用 socket fd，在同一连接上发送多个请求（server_loop 已支持多消息处理）
- **长期**：server 端增加 batch command，接收信号数组，内部循环调用 NPI

---

## 5. 架构拆分建议

### 5.1 server_core.h 拆分

当前 server_core.h 混合了三类职责：

```
server_core.h (724 行 / vwave, 479 行 / vsignal)
├── NPI helper 函数（nl_handle_to_json, nl_obj_type_str 等）
├── 命令 handler 函数（handle_status, handle_trace_driver 等）
└── run_server() 入口（初始化 + 事件循环）
```

建议拆分为：

```
server/
├── npi_helpers.h        NPI 数据转换（handle_to_json, vec_to_json 等）
├── handlers.h           命令 handler 函数
└── server_core.h        run_server() + dispatch_request()
```

### 5.2 main.cpp 拆分

当前 main.cpp 混合了四类职责：

```
main.cpp (630 行 / vwave, 623 行 / vsignal)
├── print_usage()
├── cmd_open()
├── cmd_close()
├── cmd_query()      ← 最大函数，所有命令的分发
└── main()           ← 参数解析
```

建议拆分为：

```
cli/
├── usage.h           帮助文本
├── options.h         QueryOptions struct + 参数解析
├── commands.h        cmd_open / cmd_close / cmd_query
└── main.cpp          仅 main() 入口
```

### 5.3 提取共享 cmd_open/cmd_close 模板

两工具的 cmd_open 高度相似（fork + daemon + wait-ready 循环）。差异仅在：
- vwave: `chdir` 到 run_dir（已有）
- vsignal: `chdir` 到 run_dir + LD_LIBRARY_PATH 补全

可提取为 `tw::daemon::fork_server()`，通过回调注入工具特有的 pre-fork 逻辑。

---

## 6. 分阶段实施计划

### Phase 1：参数结构化（减少参数传递）

| 任务 | 工具 | 变更 |
|------|------|------|
| 定义 vwave::QueryOptions struct | vwave | main.cpp 新增 struct，cmd_query 改为接收引用 |
| 定义 vsignal::QueryOptions struct | vsignal | 同上 |
| main() 解析结果填入 struct | 两工具 | 消除 17/12 个裸参数 |

**验收**：cmd_query 签名缩减为 `(RunDir&, command, QueryOptions&)`。

### Phase 2：server_core.h 拆分

| 任务 | 工具 | 变更 |
|------|------|------|
| 提取 npi_helpers.h | vsignal | nl_handle_to_json 等迁移 |
| 提取 handlers.h | 两工具 | 各 handler 函数迁移 |
| server_core.h 瘦身 | 两工具 | 仅保留 run_server + dispatch |

**验收**：server_core.h 降至 100 行以内。

### Phase 3：main.cpp 拆分 + cmd_open 共享

| 任务 | 工具 | 变更 |
|------|------|------|
| 提取 options.h（struct + 解析） | 两工具 | main() 瘦身 |
| 提取 commands.h（cmd_open/close/query）| 两工具 | main.cpp 仅剩 main() |
| 提取 tw::daemon::fork_server() | src_common | cmd_open 去重 |

**验收**：main.cpp 降至 80 行以内。

### Phase 4：缓存与连接复用

| 任务 | 工具 | 变更 |
|------|------|------|
| 信号 handle 缓存 | vwave server | unordered_map<name, handle> |
| batch socket 复用 | vsignal client | 单连接多请求 |
| info 结果缓存 | 两工具 server | 首次查询后缓存 |

**验收**：相同信号连续 10 次 get 耗时降低 ≥ 30%。

---

## 7. 风险评估

| 风险 | 影响 | 缓解 |
|------|------|------|
| 拆分引入 include 顺序依赖 | 编译失败 | header guard + 前向声明 |
| options struct 跨 client/server 传递 | 需序列化/反序列化 | struct 仅在 client 侧使用，通过 JSON params 传递到 server |
| cmd_open 共享模板过度抽象 | 工具特化逻辑难嵌入 | 用 lambda/回调注入差异，不过度泛化 |
| 缓存一致性 | server 热重载设计后缓存失效 | 当前设计不支持热重载，无风险 |
