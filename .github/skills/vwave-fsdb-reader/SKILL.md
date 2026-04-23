---
name: vwave-fsdb-reader
description: Read FSDB waveform files using the vwave CLI tool. Use this skill when the user asks to read signal values from FSDB waveform files, query waveform hierarchy, list signals or scopes, or inspect FSDB file metadata. Requires a compiled vwave binary and Synopsys Verdi NPI environment.
---

# vwave — FSDB Waveform Reader

vwave reads Synopsys FSDB waveform files via a fork-based daemon (UDS communication).

> **Convention:** Always append `--json` to every vwave command. All examples
> below omit it for brevity — you must add it yourself.

## Prerequisites

- `vwave` binary on PATH or at `<project>/build/bin/vwave`
- `VERDI_HOME` environment variable pointing to Synopsys Verdi installation
  (the tool auto-configures `LD_LIBRARY_PATH` and `NOVAS_HOME` at startup)

## Workflow

```
open → query (scopes / signals / get-value …) → close
```

The daemon stays resident — no need to reload FSDB between queries.

## Commands

### Lifecycle

```bash
vwave open <file.fsdb>         # Start daemon, load FSDB
vwave status                   # Check if daemon is running
vwave close                    # Stop daemon
```

Open response:
```json
{"status":"ok","message":"Waveform loaded","pid":12345,"fsdb":"/abs/path/file.fsdb"}
```

Already running → `{"status":"ok","message":"Server already running","pid":12345}`

Parent waits up to 10s for init. Check exit code on large files.

### File info

```bash
vwave info
```
```json
{"id":1,"status":"ok","data":{"file":"...","min_time":0,"max_time":1000000,"scale_unit":"ps","version":"...","is_completed":1}}
```

### Hierarchy

```bash
vwave scopes                   # Top-level scopes
vwave scopes tb.intf           # Sub-scopes under tb.intf
vwave signals tb.intf          # Signals under tb.intf
vwave signal-info tb.intf.clk  # Single signal detail
```

Signals response:
```json
{"id":1,"status":"ok","data":{"path":"tb.intf","signals":[{"name":"clk","full_name":"tb.intf.clk","left":0,"right":0,"direction":"input"}]}}
```

### Read values

**At a time point** (supports multiple signals):
```bash
vwave get-value -s tb.intf.clk -t 500000
vwave get-value -s tb.intf.clk -s tb.intf.paddr -t 500000 -r hex
vwave get-value -f signals.txt -t 1000 -r hex     # From file (one signal/line, # = comment)
```
```json
{"id":1,"status":"ok","data":{"time":500000,"values":[{"signal":"tb.intf.clk","value":"1","actual_time":500000}]}}
```

Signal not found → `{"signal":"tb.intf.xxx","error":"not found"}` in values array.

**Over a time range** (single signal only):
```bash
vwave get-value -s tb.intf.clk -b 0 -e 100000
```
```json
{"id":1,"status":"ok","data":{"signal":"tb.intf.clk","begin":0,"end":100000,"changes":[{"time":0,"value":"0"},{"time":50000,"value":"1"}]}}
```

### Get-value options

| Flag | Long | Description |
|------|------|-------------|
| `-s` | `--signal` | Signal path (repeatable) |
| `-f` | `--signal-file` | Signal list file |
| `-t` | `--time` | Time point |
| `-b` | `--begin` | Range start (requires `-e`) |
| `-e` | `--end` | Range end (requires `-b`) |
| `-r` | `--radix` | `bin` (default) / `hex` / `oct` / `dec` |

## Error handling

- Exit **0** = success, **1** = error
- Error response: `{"id":1,"status":"error","error":{"code":"SIGNAL_NOT_FOUND","message":"..."}}`
- Codes: `INVALID_PARAMS`, `INTERNAL_ERROR`, `SIGNAL_NOT_FOUND`, `FSDB_OPEN_FAILED`, `SCOPE_NOT_FOUND`, `INVALID_TIME`, `FILE_READ_ERROR`

## Runtime directory

`.vtool/wave_run/` under CWD — contains PID, socket, log, source_info.
Auto-detected upward from CWD. Override: `--run-dir <path>`.

## Agent tips

1. Check `vwave status` first — reuse running daemon when possible
2. Explore hierarchy: `scopes` → `signals` → `get-value`
3. Batch reads: `-s` multiple times or `-f` for many signals at once
4. Range mode (`-b`/`-e`) supports only one signal
5. Server auto-closes after 12h idle; call `close` to free resources earlier
