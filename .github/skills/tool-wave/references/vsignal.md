# vsignal — Netlist Signal Tracer Reference

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

### Trace Driver — What Drives a Signal

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

### Trace Load — What a Signal Drives

```bash
vsignal load top.data_in
vsignal load top.data_in --assign-cell
vsignal load top.data_in --pass-mod
```

Same format as driver, with `"loads"` array.

### FanIn — Source Registers

```bash
vsignal fanin top.q_reg
vsignal fanin top.q_reg --stop-at-pin
vsignal fanin top.q_reg --report-primary-port
vsignal fanin top.q_reg --scope top.u_sub
```
```json
{"id":1,"status":"ok","data":{"signal":"top.q_reg","count":3,"fanin":[...]}}
```

### FanOut — Destination Registers

```bash
vsignal fanout top.clk
vsignal fanout top.clk --stop-at-pin
vsignal fanout top.clk --report-primary-port
vsignal fanout top.clk --scope top.u_sub
```
```json
{"id":1,"status":"ok","data":{"signal":"top.clk","count":5,"fanout":[...]}}
```

### Trace Path Between Two Signals

```bash
vsignal trace top.data_in top.data_out
vsignal trace top.data_in top.data_out --assign-cell
```
```json
{"id":1,"status":"ok","data":{"from":"top.data_in","to":"top.data_out","count":4,"path":[...]}}
```

### Instance Port Connections

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

## Trace Options

| Option | Commands | Description |
|--------|----------|-------------|
| `--assign-cell` | driver, load, trace | Pass through assign cells |
| `--pass-mod` | driver, load | Pass through module boundaries |
| `--stop-at-pin` | fanin, fanout | Stop at pins |
| `--report-primary-port` | fanin, fanout | Report top-level ports |
| `--scope <name>` | fanin, fanout | Limit search scope |
| `--level high\|low` | conn | Connection detail level |

## Result Object Format

Each element in `drivers`, `loads`, `fanin`, `fanout`, `path` arrays:

| Field | Present | Description |
|-------|---------|-------------|
| `type` | always | `instance`, `port`, `inst_port`, `net`, `concat_net`, `slice_net`, `lib`, `cell`, `cell_pin`, `unknown` |
| `name` | always | Short name |
| `full_name` | always | Hierarchical full name |
| `def_name` | instance | Module/cell definition name |
| `direction` | port | `input` / `output` / `inout` / `none` |
| `size` | port/net | Bit width |

## Error Codes

| Code | Description |
|------|-------------|
| `INVALID_PARAMS` | Missing or invalid parameters |
| `INTERNAL_ERROR` | Server internal error |
| `SIGNAL_NOT_FOUND` | Signal path does not exist |
| `DESIGN_LOAD_FAILED` | Cannot load KDB/RTL design |
| `INSTANCE_NOT_FOUND` | Instance path does not exist |
| `NO_PATH` | No path found between signals |
