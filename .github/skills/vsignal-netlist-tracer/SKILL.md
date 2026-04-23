---
name: vsignal-netlist-tracer
description: Trace signal drivers, loads, fanin/fanout, and paths in Verilog netlists using the vsignal CLI tool. Use this skill when the user asks to trace signal connectivity, find drivers or loads of a signal, analyze fanin/fanout register connections, trace paths between signals, or inspect instance port connections. Requires a compiled vsignal binary, Synopsys Verdi NPI, and a VCS KDB database or RTL source files.
---

# vsignal — Netlist Signal Tracer

vsignal performs static signal tracing on VCS-compiled netlists (KDB) or RTL
source files via Synopsys NPI L1 APIs, through a fork-based daemon (UDS communication).

> **Convention:** Always append `--json` to every vsignal command. All examples
> below omit it for brevity — you must add it yourself.

## Prerequisites

- `vsignal` binary on PATH or at `<project>/build/bin/vsignal`
- `VERDI_HOME` environment variable pointing to Synopsys Verdi installation
  (the tool auto-configures `LD_LIBRARY_PATH` and `NOVAS_HOME` at startup)
- Design source: VCS KDB database (`vcs -kdb`) or RTL Verilog files

## Workflow

```
open → trace (driver / load / fanin / fanout / trace / conn) → close
```

Design loading is expensive; the daemon stays resident for fast subsequent queries.

## Commands

### Lifecycle

```bash
vsignal open -dbdir simv.daidir          # From KDB database
vsignal open design.v top.v              # From RTL source files
vsignal status                           # Check daemon
vsignal info                             # Design info
vsignal close                            # Stop daemon
```

Open response:
```json
{"status":"ok","message":"Design loaded","pid":12345,"design":"/path/to/source"}
```

Parent waits up to **30s** for init (KDB loading can be slow). Check exit code.

### Trace driver — what drives a signal

```bash
vsignal driver top.data_out
vsignal driver top.data_out --assign-cell            # Pass through assigns
vsignal driver top.data_out --pass-mod               # Pass through modules
vsignal driver top.data_out --assign-cell --pass-mod # Both
```
```json
{"id":1,"status":"ok","data":{"signal":"top.data_out","count":2,"drivers":[
  {"type":"port","name":"data_out","full_name":"top.u_sub.data_out","direction":"output","size":8},
  {"type":"instance","name":"u_sub","full_name":"top.u_sub","def_name":"sub_mod"}
]}}
```

### Trace load — what a signal drives

```bash
vsignal load top.data_in
vsignal load top.data_in --assign-cell
vsignal load top.data_in --pass-mod
```

Same format as driver, with `"loads"` array.

### FanIn — source registers

```bash
vsignal fanin top.q_reg
vsignal fanin top.q_reg --stop-at-pin
vsignal fanin top.q_reg --report-primary-port
vsignal fanin top.q_reg --scope top.u_sub
```
```json
{"id":1,"status":"ok","data":{"signal":"top.q_reg","count":3,"fanin":[...]}}
```

### FanOut — destination registers

```bash
vsignal fanout top.clk
vsignal fanout top.clk --stop-at-pin
vsignal fanout top.clk --report-primary-port
vsignal fanout top.clk --scope top.u_sub
```
```json
{"id":1,"status":"ok","data":{"signal":"top.clk","count":5,"fanout":[...]}}
```

### Trace path between two signals

```bash
vsignal trace top.data_in top.data_out
vsignal trace top.data_in top.data_out --assign-cell
```
```json
{"id":1,"status":"ok","data":{"from":"top.data_in","to":"top.data_out","count":4,"path":[...]}}
```

### Instance port connections

```bash
vsignal conn top.u_sub                       # High-level (default)
vsignal conn top.u_sub --level low           # Low-level
```
```json
{"id":1,"status":"ok","data":{"instance":"top.u_sub","level":"high","count":3,"connections":[
  {"port":"top.u_sub.clk","signals":["top.clk"]},
  {"port":"top.u_sub.din","signals":["top.data_in"]}
]}}
```

## Trace options

| Option | Commands | Description |
|--------|----------|-------------|
| `--assign-cell` | driver, load, trace | Pass through assign cells |
| `--pass-mod` | driver, load | Pass through module boundaries |
| `--stop-at-pin` | fanin, fanout | Stop at pins |
| `--report-primary-port` | fanin, fanout | Report top-level ports |
| `--scope <name>` | fanin, fanout | Limit search scope |
| `--level high\|low` | conn | Connection detail level |

## Result object format

Each element in `drivers`, `loads`, `fanin`, `fanout`, `path` arrays:

| Field | Present | Description |
|-------|---------|-------------|
| `type` | always | `instance`, `port`, `inst_port`, `net`, `concat_net`, `slice_net`, `lib`, `cell`, `cell_pin`, `unknown` |
| `name` | always | Short name |
| `full_name` | always | Hierarchical full name |
| `def_name` | instance | Module/cell definition name |
| `direction` | port | `input` / `output` / `inout` / `none` |
| `size` | port/net | Bit width |

## Error handling

- Exit **0** = success, **1** = error
- Error response: `{"id":1,"status":"error","error":{"code":"SIGNAL_NOT_FOUND","message":"..."}}`
- Codes: `INVALID_PARAMS`, `INTERNAL_ERROR`, `SIGNAL_NOT_FOUND`, `DESIGN_LOAD_FAILED`, `INSTANCE_NOT_FOUND`, `NO_PATH`

## Runtime directory

`.vtool/vsignal_run/` under CWD — contains PID, socket, log, source_info.
Auto-detected upward from CWD. Override: `--run-dir <path>`.

## Agent tips

1. Check `vsignal status` first — reuse running daemon when possible
2. Start with `driver`/`load` for basic connectivity, then `fanin`/`fanout` for register-level
3. Use `--assign-cell` to see through assigns (usually needed for real designs)
4. Use `--pass-mod` to trace across module boundaries
5. `trace` finds paths between signals — useful for timing/connectivity
6. `conn` shows all ports of an instance — good for interface understanding
7. Client timeout is 60s — large designs may be slow
8. Server auto-closes after 12h idle; call `close` to free resources earlier
