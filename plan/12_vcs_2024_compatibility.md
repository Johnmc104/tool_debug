# VCS 2024 / KDB 兼容性验证

日期：2026-10-20  
范围：`vsignal` 的 VCS KDB 输入、Verdi NPI 编译与运行链路。

## 结论

当前代码可以支持 VCS `W-2024.09`。VCS 2024 生成的 `simv.daidir` 可由现有
`vsignal` 打开并完成结构查询；使用 Verdi `W-2024.09` NPI 头文件和库重新编译
`vsignal` 后，查询结果同样正常。无需修改 NPI 查询实现或协议。

支持边界由 Verdi/NPI 配套关系决定：VCS 只生成 KDB，`vsignal` 链接和运行时使用
构建时 `VERDI_HOME` 对应的 NPI。不能把不同 Verdi 版本的头文件、库或运行环境
混用。当前已验证的组合如下：

| VCS KDB 生成器 | vsignal NPI | 结果 |
|---|---|---|
| W-2024.09 | W-2024.09 | 通过 |
| W-2024.09 | Y-2026.03-SP2 | 通过 |

## 实测步骤

### VCS 2024 生成 KDB

```bash
source /opt/Synopsys/env_2024.sh
export PATH=/opt/Synopsys/vcs/W-2024.09/bin:$PATH
vcs -sverilog -kdb -q example.v -o simv
```

结果：退出码 0，日志包含 `Verdi KDB elaboration done`，生成
`simv.daidir/kdb.elab++`。

### 默认发布版 vsignal（Y-2026.03-SP2 NPI）

在上述 KDB 上执行：

```bash
vsignal open -dbdir simv.daidir --json
vsignal status --json
vsignal info --json
vsignal driver top.data_out --json
vsignal conn top.u_sub --json
vsignal close --json
```

结果：全部返回 `"status":"ok"`；`driver` 返回
`top.u_sub.data_out[7:0]`，`conn` 返回 5 个端口连接。

### Verdi 2024 NPI 编译版 vsignal

```bash
make vsignal \
  BIN_DIR=/tmp/tool_wave_vsignal2024/bin \
  VERDI_HOME=/opt/Synopsys/verdi/W-2024.09
```

该二进制在相同 KDB 上完成 `open`、`driver`、`fanout` 和 `close`，全部通过。

## 工程改动

- `test_vsignal/run_test.sh` 支持 `VCS_BIN`、`VCS_HOME`，选择顺序为
  `VCS_BIN > VCS_HOME/bin/vcs > PATH`，避免测试误用交互 shell 中的其他 VCS。
- `Makefile test-vsignal` 透传上述变量，`make help` 显示 VCS 选择方式。
- README 增加 VCS 2024 兼容矩阵、构建命令和 NPI 配套限制。
- 版本更新为 `1.9`，发布包和 CLI 版本检查沿用公共打包流程。

## 限制

本次实测针对 `W-2024.09`。VCS 2024 的其他补丁版以及 Verdi 2024 的其他补丁版
可能包含 KDB/NPI 行为差异，需要用实际安装重新执行同一组回归。VCS 2024 的
FSDB 读取属于 `vwave` 的 NPI/FFR 兼容性问题，不由本报告覆盖。
