---
name: tool-wave
description: 'Read FSDB waveforms and trace netlist signals using vwave/vsignal CLI tools. Use when: reading signal values from FSDB files, querying waveform hierarchy, tracing signal drivers/loads, analyzing fanin/fanout register connections, tracing paths between signals, inspecting instance port connections. Requires VERDI_HOME environment variable.'
---

# tool-wave — FSDB 波形读取 & 网表信号追踪

两个 CLI 工具，基于 Synopsys Verdi NPI，通过 fork-based daemon + UDS 通信：

| 工具 | 用途 | 数据源 |
|------|------|--------|
| **vwave** | FSDB 波形值读取 | `.fsdb` 波形文件 |
| **vsignal** | 网表信号驱动/负载追踪 | VCS KDB 数据库 或 RTL 源文件 |

> **Convention:** Always append `--json` to every command. All examples
> below omit it for brevity — you must add it yourself.

## Prerequisites

- `vwave` / `vsignal` binary on PATH or at `<project>/build/bin/`
- `VERDI_HOME` + `LD_LIBRARY_PATH` 包含 `$VERDI_HOME/share/NPI/lib/linux64`（或通过 `module load` 设置）
- 环境检查: [check-tools.sh](./scripts/check-tools.sh)

## Workflow

Both tools follow the same lifecycle pattern:

```
open → query … → close
```

Daemon stays resident between queries — no reload overhead.

## Quick Reference

### vwave — 波形读取

```bash
vwave open <file.fsdb>                   # 启动 daemon，加载 FSDB
vwave info                               # 文件元信息（时间范围、精度）
vwave scopes [path]                      # 层级浏览
vwave signals <scope>                    # 信号列表
vwave get-value -s <sig> -t <time>       # 读取信号值
vwave get-value -s <sig> -b <t0> -e <t1> # 范围读取（变化列表）
vwave close                              # 关闭 daemon
```

详细命令格式、选项、响应示例 → [vwave reference](./references/vwave.md)

### vsignal — 信号追踪

```bash
vsignal open -dbdir simv.daidir          # 加载 KDB 数据库
vsignal open design.v top.v              # 或 RTL 源文件
vsignal driver <signal>                  # 驱动源追踪
vsignal load   <signal>                  # 负载追踪
vsignal fanin  <signal>                  # FanIn 寄存器连接
vsignal fanout <signal>                  # FanOut 寄存器连接
vsignal trace  <from> <to>              # 信号间路径
vsignal conn   <instance>               # 实例端口连接
vsignal close                            # 关闭 daemon
```

详细命令格式、选项、响应示例 → [vsignal reference](./references/vsignal.md)

## Common Options

| 选项 | 说明 |
|------|------|
| `--json` | JSON 输出（必须使用） |
| `--run-dir <path>` | 覆盖运行时目录 |
| `-h, --help` | 帮助信息 |

## Runtime Directory

```
<cwd>/.vtool/
├── wave_run/       # vwave: PID, socket, log, source_info
└── vsignal_run/    # vsignal: PID, socket, log, source_info
```

Auto-detected upward from CWD. Override: `--run-dir <path>`.

## Error Handling

- Exit **0** = success, **1** = error
- Error response: `{"id":1,"status":"error","error":{"code":"...","message":"..."}}`
- vwave codes: `INVALID_PARAMS`, `INTERNAL_ERROR`, `SIGNAL_NOT_FOUND`, `FSDB_OPEN_FAILED`, `SCOPE_NOT_FOUND`, `INVALID_TIME`, `FILE_READ_ERROR`
- vsignal codes: `INVALID_PARAMS`, `INTERNAL_ERROR`, `SIGNAL_NOT_FOUND`, `DESIGN_LOAD_FAILED`, `INSTANCE_NOT_FOUND`, `NO_PATH`

## Agent Tips

1. Check `status` first — reuse running daemon when possible
2. **vwave**: Explore hierarchy `scopes` → `signals` → `get-value`; batch reads with `-s` multiple times or `-f` for signal list file
3. **vsignal**: Start with `driver`/`load`, then `fanin`/`fanout` for register-level; use `--assign-cell` for real designs
4. Both servers auto-close after 1h idle; call `close` to free resources earlier
5. vwave client timeout: 30s; vsignal client timeout: 60s (large designs may be slow)
