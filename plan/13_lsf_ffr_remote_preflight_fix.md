# LSF FFR 远端 preflight 修复

日期：2026-10-20  
问题：登录节点 glibc 2.17，计算节点 glibc 2.28；项目使用 Verdi
Y-2026.03-SP2 生成的 FSDB 6.1。

## 根因

`vwave open --backend ffr --run-mode lsf` 原先先在登录节点 fork
`vwave-ffr-worker --check`。该 worker 会立即 `dlopen` 选定的
`libnffr.so` 和 `libnsys.so`，因此登录节点在提交 LSF 作业前就因
`GLIBC_2.25 not found` 退出。真正的 reader 初始化应发生在计算节点。

## 修复

`src_vwave/main.cpp` 现在按运行模式处理候选库：

- `local`：保留本地 preflight，启动前尽早发现库、ABI 和许可证问题。
- `lsf`：跳过登录节点的 worker preflight；LSF proxy 启动后等待远端
  `vwave-ffr-worker --stdio-loop` 的 `ready`。远端 worker 在
  `backend.open()` / `info()` 中完成实际 `dlopen`、许可证检查和 FSDB 打开。

LSF 分支仍然计算库 SHA-256 并把完整会话元数据传给 worker，没有绕过远端加载
或错误检查。这个改动只修正 preflight 所在节点。

## 回归验证

`test_vwave/test_lsf_mode.py` 新增了远端 preflight 回归：构造不可在本地加载的
伪造 FFR SDK，使用 fake LSF worker 返回 stdio-loop ready。LSF `open/status/close`
成功证明登录节点不会尝试 `dlopen`，原有真实 FFR smoke test 继续执行实际 reader
查询。

验证命令：

```bash
make -B vwave
python3 test_vwave/test_lsf_mode.py
make test-vwave
```

## 部署边界

`vwave`、`vwave-ffr-worker` 和 `vwave-npi-worker` 应放在计算节点可执行的共享
目录；`VERDI_HOME`、`NOVAS_HOME`、`NPI_LICENSE` 和 license 环境也必须由 LSF
作业继承。日常使用时，若登录节点无法加载 Verdi 2026 的 `libnsys.so`，应在
`VWAVE_RUN_MODE=lsf` 下使用 FFR；不要把 FFR worker 放在仅登录节点可见的
`/tmp`。NPI backend 仍受其自身 FSDB/KDB 版本兼容性约束。

如果未来官方版本把 preflight 移到 LSF worker，或提供等价的显式跳过选项，应
重新比较行为并删除本地分支绕过逻辑。
