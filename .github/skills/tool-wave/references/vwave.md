# vwave — FSDB Waveform Reader Reference

## Commands

### Lifecycle

```bash
vwave open <file.fsdb> [--timeout <sec>]  # Start daemon, load FSDB (default timeout: 30s)
vwave status                              # Check if daemon is running
vwave close                               # Stop daemon
```

Open response:
```json
{"status":"ok","message":"Waveform loaded","pid":12345,"fsdb":"/abs/path/file.fsdb"}
```

Already running → `{"status":"ok","message":"Server already running","pid":12345}`

Exit code 2 = server still loading (not a hard failure, retry with `vwave status`).

### File Info

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
vwave scopes tb.intf -c        # Compact: short names only
vwave signals tb.intf          # Signals under tb.intf
vwave signals tb.intf -c       # Compact: "name[L:R] dir" strings
vwave signal-info tb.intf.clk  # Single signal detail
```

Compact signals response (88% smaller):
```json
{"path":"tb.intf","signals":["clk i","rst_n i","paddr[31:0] o","pwdata[31:0] o"],"count":4}
```

Full signals response:
```json
{"id":1,"status":"ok","data":{"path":"tb.intf","signals":[{"name":"clk","full_name":"tb.intf.clk","left":0,"right":0,"direction":"input"}]}}
```

### Find (Wildcard Search)

```bash
vwave find "*clk*" --scope tb.top_inst    # Recursive wildcard in subtree
vwave find "*paddr*" --scope tb.dut       # Substring match
vwave find "HRESETn" --scope tb.dut       # Exact short name match
vwave find "*gpio*out*"                   # Global search (all top scopes)
```
```json
{"pattern":"*clk*","scope":"tb.top_inst","signals":["tb.top_inst.dut.clk","tb.top_inst.dut.u_clkctrl.FCLK"],"count":2}
```

Notes:
- Pattern matches signal **short name** (not full path) using `*` (any) and `?` (single char)
- Without `*`, pattern is exact match — use `*name*` for substring search
- Search is recursive through all child scopes
- Results capped at 200 signals (`"truncated":1` if hit)
- Use `--scope` to narrow search and reduce noise



**At a time point** (supports multiple signals):
```bash
vwave get -s tb.intf.clk -t 500000
vwave get -s tb.intf.clk -s tb.intf.paddr -t 500000 -r hex
vwave get -f signals.txt -t 1000 -r hex     # From file (one signal/line, # = comment)
```
```json
{"id":1,"status":"ok","data":{"time":500000,"values":[{"signal":"tb.intf.clk","value":"1","actual_time":500000}]}}
```

Signal not found → `{"signal":"tb.intf.xxx","error":"not found"}` in values array.

**Over a time range** (single signal only):
```bash
vwave get -s tb.intf.clk -b 0 -e 100000
```
```json
{"id":1,"status":"ok","data":{"signal":"tb.intf.clk","begin":0,"end":100000,"changes":[{"time":0,"value":"0"},{"time":50000,"value":"1"}]}}
```

### Get Options

| Flag | Long | Description |
|------|------|-------------|
| `-s` | `--signal` | Signal path (repeatable) |
| `-f` | `--signal-file` | Signal list file |
| `-t` | `--time` | Time point |
| `-b` | `--begin` | Range start (requires `-e`) |
| `-e` | `--end` | Range end (requires `-b`) |
| `-r` | `--radix` | `bin` (default) / `hex` / `oct` / `dec` |

### Global Options

| Flag | Description |
|------|-------------|
| `--compact`, `-c` | Compact output (short names, fewer tokens) |
| `--json` | Full JSON-RPC envelope output |
| `--fsdb <path>` | Explicit FSDB path (skip auto-detect) |
| `--run-dir <path>` | Override runtime directory |
| `--timeout <sec>` | Server start timeout (default: 30, open only) |

## Error Codes

| Code | Description |
|------|-------------|
| `INVALID_PARAMS` | Missing or invalid parameters |
| `INTERNAL_ERROR` | Server internal error |
| `SIGNAL_NOT_FOUND` | Signal path does not exist |
| `FSDB_OPEN_FAILED` | Cannot open FSDB file |
| `SCOPE_NOT_FOUND` | Scope path does not exist |
| `INVALID_TIME` | Time value out of range |
| `FILE_READ_ERROR` | Cannot read signal list file |
