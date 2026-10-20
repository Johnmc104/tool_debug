# vsignal 多版本 Verdi/NPI 支持

## 目标

让同一份发布包同时支持 Verdi/NPI 2022、2024、2026，并保持本地与 LSF
运行模式使用同一套 `VWAVE_*` 环境变量。

## 实现

- 发布三个版本专用 worker：
  - `vsignal-worker-2022` → `T-2022.06-SP2`
  - `vsignal-worker-2024` → `W-2024.09`
  - `vsignal-worker-2026` → `Y-2026.03-SP2`
- 每个 worker 使用对应版本的 NPI headers、`libNPI.so`/`libnpiL1.so` 和
  `--disable-new-dtags,-rpath`，避免 `LD_LIBRARY_PATH` 把库切换到其他版本。
- `vsignal` CLI 保持 NPI-free。`open` 根据 `VERDI_HOME` 路径中出现的
  `2022`、`2024`、`2026` 选择 worker；无法识别时回退到通用
  `vsignal-worker`。
- LSF proxy 把已经选择的 worker 路径提交到 `bsub -I`，计算节点执行相同的
  版本专用 worker。查询、关闭、队列、资源和超时仍复用：
  `VWAVE_RUN_MODE`、`VWAVE_LSF_BSUB`、`VWAVE_LSF_BKILL`、
  `VWAVE_LSF_QUEUE`、`VWAVE_LSF_RESOURCE`、`VWAVE_LSF_QUERY_TIMEOUT`。

## 验证矩阵

使用匹配的 VCS/Verdi 安装分别生成 KDB：

```bash
VERDI_HOME=/opt/Synopsys/verdi/T-2022.06-SP2 make test-vsignal VCS_BIN=/opt/Synopsys/vcs/T-2022.06-SP2/bin/vcs
VERDI_HOME=/opt/Synopsys/verdi/W-2024.09 make test-vsignal VCS_BIN=/opt/Synopsys/vcs/W-2024.09/bin/vcs
VERDI_HOME=/opt/Synopsys/verdi/Y-2026.03-SP2 make test-vsignal VCS_BIN=/opt/Synopsys/vcs/Y-2026.03-SP2/bin/vcs
```

附加交叉检查表明，Verdi 2026 可读取本次 VCS 2024 KDB；Verdi 2022 对
VCS 2024 KDB 不兼容，NPI 内部会崩溃，因此工具不会尝试自动跨版本转换 KDB。

LSF smoke test使用 fake `bsub` 检查三种 `VERDI_HOME` 分别提交
`vsignal-worker-2022/2024/2026`，并验证 status、job id 和 close。

## 限制

- `VERDI_HOME` 路径需要包含对应四位年份，或使用标准 `T-/W-/Y-` 安装目录。
- 三个 worker 和 `vsignal` 必须部署在同一共享目录；LSF 计算节点必须能访问该目录、
  Verdi 安装和许可证服务。
- VCS/Verdi 其他补丁版本需要重新做 NPI ABI 和 KDB 行为验证。
