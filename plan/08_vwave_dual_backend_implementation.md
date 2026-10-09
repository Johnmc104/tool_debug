# vwave 双后端首版实施与验收

日期：2026-10-09

分析与计划检查点已提交：`f2f7412`。本轮在其后实施；代码、测试证据和本记录一并纳入本轮 Git 提交，安装包未发布。前置计划见 [07](07_vwave_dual_backend_dev_plan.md)。

## 1. 已实现的使用方式

```bash
# 原用法，默认 NPI
vwave open run.fsdb

# 从当前 VERDI_HOME 选择配套 reader
vwave open run.fsdb --backend ffr

# 显式选择 SDK
vwave open run.fsdb --backend ffr --verdi-home /path/to/verdi

# 查询使用会话中的后端；不重复解析当前 EDA 环境
vwave get -s tb.clk -t 25000 --json
vwave status --json

# 同文件切换后端
vwave open run.fsdb --backend npi

# 独立运行目录
vwave open other.fsdb --backend ffr --run-dir /tmp/other-wave
vwave info --run-dir /tmp/other-wave --json
```

新参数只接受 `npi`、`ffr`，默认 open 选择 NPI。查询省略参数时沿用已有会话；显式指定只核对模式，不匹配时提示重新 open。`--verdi-home` 仅用于 ffr open。未知参数、缺失参数值明确失败。

FFR 当前接受的 reader 构建为 **T-2022.06-SP2**、**Y-2026.03-SP2**。其他构建报告未验证，后续通过 ABI 与行为矩阵扩展。NPI 保持已有初始化和许可证行为，不随 shell 切换为任意 NPI SDK。

## 2. 实现结构

| 产物 | 工作 | 厂商库关系 |
|---|---|---|
| `vwave` | 参数、会话选择、启动后台、查询客户端 | 不直接链接 NPI 或 FsdbReader |
| `vwave-npi-worker` | 共用 server + `NpiBackend` | 链接编译配套 NPI / npiL1 |
| `vwave-ffr-worker` | 共用 server + `FfrBackend` | 运行时加载所选 nsys / nffr |

两个 worker 共用 `WaveBackend` 接口、dispatcher 和 JSON handlers。厂商句柄只留在对应 backend 中；没有增加中间常驻进程或额外查询转发。后台通过 fork + exec 启动，不把前端已有地址空间中的厂商状态带入。

FFR 使用已验证的 Linux x86_64 C++ ABI：按 SDK 接口调用虚方法，普通成员入口用 `dlsym` 绑定带 this 参数的函数指针。实际绑定覆盖文件打开、文件信息、时间定位、取值、前后遍历和释放句柄。当前无需工程 shim；这是 P0 原型验证后的实现选择。

加载次序为 zlib、所选 `libnsys.so`、所选 `libnffr.so`，使用 `RTLD_NOW | RTLD_GLOBAL` 解决配套符号。每个进程只有一套 reader。核对实际库来源，并在创建 reader 前检查构建标识和必须入口；错误保留原始 `dlopen/dlsym` 信息。

NPI 的编译配套信息由实际 NPI worker 报告，CLI 不再依据自身的编译路径推断。这样可独立构建轻量前端，也能保留另一版本编译的 NPI worker。新增参数不会直接交给 `npi_init`，初始化参数在 worker 内构造并保持有效生命周期。

已用当前 CLI 搭配单独编译的 2022 NPI worker 验证 open、查询与 close；即使 shell 的 `VERDI_HOME` 指向 2026，会话仍正确报告并使用 worker 配套的 2022 安装。

## 3. 会话与 EDA 切换

保留原 `source_info` 路径文件，新增原子写入的 `session.json`，记录后端、文件身份、SDK 目录、reader/support 库路径、SHA-256 和适配器标识。查询不重新计算库哈希。

再次 open 的行为：

- 文件、后端和库身份相同，复用原 PID。
- 更换后端、SDK 目录，或同路径库内容更新，启动新 worker。
- 库或接口预检失败，保留原会话；不自动回退 NPI。
- 文件实际打开失败，清理未就绪的 PID/socket/元数据，保留错误日志。
- 有存活 PID 但 socket 状态无法核实，拒绝停止该 PID，避免旧 PID 文件误指其他进程。

旧 NPI 会话仍可查询和关闭；没有新元数据的旧会话在再次 open 时重建。`--run-dir` 可以独立用于查询与关闭，修复了旧版本未同时指定 `--fsdb` 就忽略覆盖目录的问题。相对目录会先转为绝对路径，带空格目录已验证。

## 4. 查询结果的兼容处理

普通四态数字信号、宽总线和数组元素已接入现有层次、点值、多信号、范围、边沿和计数命令。重点对照并保留了这些旧行为：

- reader 会重复回调部分树记录，需要按路径合并；隐藏 scope 不进入用户层次。
- 普通总线声明的 `[left:right]` 从外部名字中去除，保留位范围字段；升序范围和数组元素分别处理。
- 转义名字保留 NPI 使用的尾部空格；数组列出父项，元素可独立查询。
- 点值的 `actual_time` 沿用旧输出中的请求时间，不改成底层 VC 时间。
- 范围先输出 begin 时刻的值，即使 begin 没有真实 VC；`total_changes` 完整统计，limit 只限制输出数量。
- `vc-count` 计真实 VC，与范围的起始快照不同；同时间记录按照原查询结果对照。
- rising/falling 保留原目标值查找及起点排除行为，包括原 forward 从 `t+1` 开始后继续排除该起点的边界。
- X/Z 在 bin、hex、oct、dec 中的大小写、混合组显示及宽值十进制转换与 NPI 对照。

FFR 对 real、string、事务/属性值、非四态编码及整个数组的值读取返回明确的未支持错误。struct/class 等复杂分组、持续写入文件和大规模生产波形尚未完成专项验收，不作为本轮通过范围。

## 5. 验收矩阵

| 场景 | 配套关系 | 结果 |
|---|---|---|
| 现有样本 | 2022 dumper、FSDB 6.0；分别测试两套 reader | 两套与 NPI 对照，包含会话切换与失败检查 |
| 新建 2022 样本 | VCS / Verdi T-2022.06-SP2，FSDB 6.0 | 数字、X/Z、130 bit 总线、数组、转义名字、同时间变化通过 |
| 新建 2026 样本 | VCS / Verdi Y-2026.03-SP2，FSDB 6.4 | 相同查询矩阵通过，验证配套新波形 |
| CentOS 7.9 / glibc 2.17 | 2022 reader、现有样本 | 完整 CLI fork/exec、open/get/count/status/close 通过 |
| CentOS 7.9 / glibc 2.17 | 请求 2026 reader | 明确报告缺失 GLIBC，原 2022 会话仍可查询 |

现有样本双 SDK 检查 **924 项**，两套配套样本分别 **1709 项**，合计 **4342 项，差异为 0**。原 NPI 回归 **62/62**。测试检查退出状态及解析后的 JSON，不只匹配输出字符串。

附加覆盖：显式安装优先于错误环境、隐式 VERDI_HOME 选择、已打开会话不跟随 shell 环境改变、独立目录、同路径库内容变化、非法 ELF、缺入口、未验证构建、损坏 FSDB、未支持类型及旧 PID 误指其他进程。

构建使用本机 GCC 9.5，CLI 与 FFR worker 静态链接 C++ 运行库。两个 ELF 的最高 GLIBC 要求均为 **2.17**，没有 NPI/nffr/nsys 的 `NEEDED` 或固定 SDK RPATH。实际进程映射核对了配套 nffr/nsys，FFR worker 未加载 NPI。

CentOS 7 验收环境的镜像层：

```text
quay.io/centos/centos:7
sha256:2d473b07cdd5f0912cd6f1a703352c82b512407db6b05b43f2553732b55df3bc
CentOS Linux release 7.9.2009 (Core)
glibc 2.17
```

本机 Docker 创建容器和新挂载操作挂起，因此本轮改用经过哈希核验的镜像运行库，在 `unshare -Ur chroot` 中完成实际运行。仅复制工具、样本及两套 reader/support 库，没有完整 Verdi 安装。该方法不隔离网络；两个 license 环境变量设为 `1@127.0.0.1`，strace 跟踪中未出现 AF_INET/AF_INET6 调用，通信均为本地 Unix socket。Docker 构建/验收脚本保留，当前机器上该路径未通过。

发布包已检查包含 CLI、vsignal 和两种 worker，未包含厂商 `.so`；包可在本地生成，未发布。`make vwave-ffr` 可单独构建 CLI 与 FFR worker，不需要 NPI 头文件或链接库。

## 6. 复现与过程文件

- [双后端差分与切换测试](../test_vwave/test_backends.py)
- [配套样本源代码](../test_vwave/fixtures/backend_fixture.sv) 与 [构建脚本](../test_vwave/build_backend_fixture.sh)
- [Docker CentOS 7 验证脚本](../test_vwave/test_centos7_backend.sh)
- [CentOS 7 运行库提取脚本](../reverse_analysis/scripts/extract_centos7_runtime.py) 与 [chroot 验证脚本](../test_vwave/test_centos7_chroot.py)
- [阶段证据](../reverse_analysis/evidence/dynamic_backend/summary.json)

源代码和脚本保存于工程，测试编译产物、生成波形、镜像运行库保存在被忽略的 `build/`。阶段证据保留测试摘要、ELF 要求、CentOS 7 命令结果和网络跟踪；厂商库、SDK 头文件及镜像层不纳入 Git。

## 7. 后续重点

首版完成双后端接入和两套已验证 SDK 的动态切换。下一步按实际使用反馈补充更大波形的打开耗时、查询延迟和峰值内存；复杂值类型与分组逐项扩展。新增 SDK 需先验证入口、结构与查询矩阵，再扩充支持表。

远程 server/client 分离、自包含厂商库和原生 FSDB 磁盘格式解析继续作为独立任务。
