# FSDB 逆向分析工作区

目标：分析 FSDB 的结构与读取路径，评估在本地自行解析、脱离 NPI 与厂商库的可行性。

初步报告：[01_fsdb_native_reader_feasibility.md](reports/01_fsdb_native_reader_feasibility.md)。

后续实测：[02_reader_centos7_and_npi_relationship.md](reports/02_reader_centos7_and_npi_relationship.md)。2022 reader 已在纯 CentOS 7 容器完成断网读取，可作为免 NPI 的候选方案；2026 reader 的 glibc 要求过高。

## 目录

```text
reverse_analysis/
├── reports/              分析报告与阶段结论
├── notes/                过程记录、假设和待验证问题
├── scripts/              可重复执行的证据采集与实验脚本
├── probes/               独立的最小实验程序
├── evidence/static/      样本指纹、ELF 信息、符号、局部反汇编
├── evidence/probe/       reader 输出、实验条件与网络调用跟踪
├── evidence/compatibility/  两版本 ABI 要求、NPI 内置 reader 调用链
├── evidence/centos7/     CentOS 7 运行记录、镜像指纹与结果对照
└── build/                本地编译产物，不纳入版本管理
```

现有样本引用 `../test_vwave/tb_top.fsdb`，未复制或修改。库引用本机安装路径，未复制厂商二进制或头文件到工程。

## 重复实验

在工程根目录执行：

```bash
# 静态取证，不加载 NPI
python3 reverse_analysis/scripts/collect_static.py

# 最小 reader 读取实验，依赖 g++、strace 和本机 FsdbReader SDK
bash reverse_analysis/scripts/run_reader_probe.sh

# 两版本 glibc 要求与 NPI 底层调用关系
python3 reverse_analysis/scripts/analyze_reader_compat.py

# CentOS 7 实测；使用本机已有的 centos:7 和 manylinux2014 镜像
bash reverse_analysis/scripts/test_centos7_reader.sh
```

可以指定不同安装、样本和信号：

```bash
python3 reverse_analysis/scripts/collect_static.py \
  --verdi-home /path/to/verdi --sample /path/to/input.fsdb \
  --output reverse_analysis/evidence/another_static_run

bash reverse_analysis/scripts/run_reader_probe.sh \
  /path/to/verdi /path/to/input.fsdb full.signal.name
```

静态采集兼容本机 Python 3.6。reader 探针链接 `libnffr`、`libnsys`、zlib、dl 和 pthread，不链接 NPI。脚本把两个 License 环境变量在子进程内设为 `1@127.0.0.1`，并跟踪全部子进程的网络调用；不会修改当前 shell 的配置。

重复 reader 实验会覆盖 `evidence/probe/` 中对应输出。保留阶段证据时应先另存该目录。原始 stdout 存在 `reader.stdout.txt`；厂商库会输出普通文本，脚本另外提取有效 JSON 记录到 `reader.jsonl`。

目前探针是厂商 reader 的对照工具，支持已加载的 1 byte/bit 数字信号，最多输出 10000 个事件。它不是自研 FSDB 解码器；不能把 API 返回的解压后值编码当作磁盘编码。
