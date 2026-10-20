# vsignal LSF 支持

日期：2026-10-20

## 结论

`vsignal` 现已复用 `vwave` 的运行模式定义：`VWAVE_RUN_MODE`、
`VWAVE_LSF_BSUB`、`VWAVE_LSF_BKILL`、`VWAVE_LSF_QUEUE`、
`VWAVE_LSF_RESOURCE`、`VWAVE_LSF_QUERY_TIMEOUT`。支持：

```bash
vsignal open -dbdir simv.daidir --run-mode lsf
vsignal driver top.u_cpu.HCLK --json
vsignal close
```

## 实现

- `vsignal` 现在是 NPI-free CLI，只负责参数解析、UDS 查询和 LSF proxy。
- 新增 `vsignal-worker`，在本地或 LSF 计算节点加载 Verdi NPI、导入 KDB 并提供 JSONL stdio-loop。
- 登录节点通过 `bsub -I` 保持 proxy；独立的 `status/info/driver/load/fanin/fanout/trace/conn`
  查询继续使用原有 CLI 和 Unix socket。
- worker session 使用 hex 编码的 JSON 传输设计源和 `-dbdir`/RTL 参数，避免 LSF shell
  重建命令时破坏路径中的空格或 JSON 字符。
- `--run-dir` 现在同时适用于 status/query/close，LSF 和 local 会话都可显式指定运行目录。

## 验证

```bash
make -B vsignal
python3 test_vsignal/test_lsf_mode.py
make test-vsignal VCS_BIN=/opt/Synopsys/vcs/W-2024.09/bin/vcs
```

LSF smoke test 使用 fake worker 验证 `open/status/close`、job ID 和运行模式字段；
本地完整 VCS 2024 KDB 回归 28/28 通过。实际部署时必须把 `vsignal` 和
`vsignal-worker` 放在计算节点可执行的共享目录，并让 worker 继承 Verdi、NPI 和 license 环境。
