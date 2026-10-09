# 2026-10-09：配套 SDK 切换与发布方式

用户明确：主要使用配套 reader，不要求旧 reader 读取新年代波形。需要考虑切换 EDA，以及动态加载用户安装库、自包含发布两种方式。

本次使用先前在 glibc 2.17 环境按 2022 SDK 构建的同一个探针，在当前较新主机分别运行两套库。每次通过 `ldd` 确认 libnffr / libnsys 都来自所选目录，再设置不可用 License 地址，使用 strace 观察网络调用。

两次读取现有旧样本均通过，全部 JSON 记录一致，未观察到网络调用。该实验验证的是运行时库切换与既有查询子集，不是 dlopen 实现，也没有验证 2026 写入器样本。

两版 `ffrObject` 提取出的 111 组虚函数声明序列一致。提取未预处理全部平台宏，其他结构体、接口与特性仍需按适配范围验证。SDK 本身有版本化接口类和 `ffrGetInterface()`，后续适配应研究其接口约定。

兼容计划已改为：外部配套库为动态模式，随包固定 SDK 为自包含模式；会话打开时固定 reader，查询不跟随 shell 环境变化，新的 open 检测 reader 身份变化后再切换。

证据：`evidence/reader_switch/summary.json` 及各版本的 ldd、stdout、stderr、网络跟踪。复现：`scripts/test_reader_switch.py`。
