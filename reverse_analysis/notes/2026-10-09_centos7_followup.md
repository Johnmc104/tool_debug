# 2026-10-09：CentOS 7 与 NPI 底层关系补充记录

用户接受复用独立 reader `.so`，重点是无需许可证服务器、能在 CentOS 7 运行，以及它与 NPI 的关系。

1. 检查两版本 `libnffr.so` / `libnsys.so` 的未定义动态符号版本。2026 版最高要求分别为 GLIBC 2.27 / 2.28；2022 版均为 GLIBC 2.17。
2. 检查本机已有容器镜像。`centos:7` 和 `manylinux2014_x86_64` 都是 CentOS 7.9 / glibc 2.17，后者提供 GCC 10.2.1。
3. 在 manylinux2014 中使用 2022 SDK 编译相同探针，静态链接 libstdc++ / libgcc，运行库相对路径为 `$ORIGIN/lib`。探针自身最高要求 GLIBC 2.14。
4. 在纯 `centos:7` 中只挂载探针、reader 的 linux64 目录和样本，不挂载完整 Verdi。设置两个 License 地址为 `1@127.0.0.1`，关闭网络。成功读取 132 条事件，所有 JSON 记录与原 2026 reader 输出一致。
5. 使用同一个 glibc 2.17 探针加载 2026 版 reader，加载阶段返回 1，报告缺失 GLIBC 2.18 / 2.25 / 2.26 / 2.27 / 2.28。该负例只验证动态库加载，未验证跨版本 API ABI。
6. NPI 局部反汇编显示 `open_file()` → `open_file_with_ffr()` → 内部 `ffrObject::*ChkLic()`。这些 ffr / ffrDisk 符号在 `libNPI.so` 中是本地定义，不是外部未解析导入；直接 call 的地址也在同一 ELF 中。
7. NPI 的 `NEEDED` 不含 `libnffr.so` 或 `libnsys.so`。结合完整调用证据，可确认当前普通 FSDB 打开路径使用内置 reader 实现；不能通过替换独立 SDK 库改变它。

复现脚本与结果见 `scripts/analyze_reader_compat.py`、`scripts/test_centos7_reader.sh`、`evidence/compatibility/`、`evidence/centos7/`。

实验中的一次 Docker 挂载失败来自只读父挂载中尚未创建 `lib` 子目录，创建空挂载点后已解决。此次未修改主工程 backend。
