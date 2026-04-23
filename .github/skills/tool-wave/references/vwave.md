# vwave — FSDB Waveform Reader Reference

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
vwave signals tb.intf          # Signals under tb.intf
vwave signal-info tb.intf.clk  # Single signal detail
```

Signals response:
```json
{"id":1,"status":"ok","data":{"path":"tb.intf","signals":[{"name":"clk","full_name":"tb.intf.clk","left":0,"right":0,"direction":"input"}]}}
```

### Read Values

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

### Get-value Options

| Flag | Long | Description |
|------|------|-------------|
| `-s` | `--signal` | Signal path (repeatable) |
| `-f` | `--signal-file` | Signal list file |
| `-t` | `--time` | Time point |
| `-b` | `--begin` | Range start (requires `-e`) |
| `-e` | `--end` | Range end (requires `-b`) |
| `-r` | `--radix` | `bin` (default) / `hex` / `oct` / `dec` |

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
