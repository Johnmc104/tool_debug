# tool_wave v1.4 Assessment — Usability, Performance, Optimization Directions

Date: 2026-08-14
Test environment: M0V1 SoC (Cortex-M0), sim_pre directory
EDA versions tested: VCS/Verdi Y-2026.03 (compile-time), W-2024.09 (cross-version)

---

## 1. Cross-Version Adaptability

### Current State

Binary compiled with Verdi Y-2026.03, RUNPATH hardcoded to that version's NPI lib.
RUNPATH (not RPATH) used via `--enable-new-dtags`, so `LD_LIBRARY_PATH` can override at runtime.

| Scenario | vwave | vsignal |
|----------|-------|---------|
| Binary=Y-2026.03, Data=Y-2026.03 | OK | OK |
| Binary=Y-2026.03, Data=W-2024.09 | OK | OK (needs `LD_LIBRARY_PATH`) |
| FSDB version compatibility | v6.1 ↔ v6.4 both work | N/A |

### Issues Found

- **vsignal requires extra `LD_LIBRARY_PATH`**: `npi_load_design` dynamically loads `liblpinstrumentdb.so` from `$VERDI_HOME/platform/linux64/bin/`, which is NOT in the RUNPATH. vwave does not need this because `npi_fsdb_open` only uses libs from `NPI/lib/`.
- **User friction**: `module load synopsys/verdi/X` sets `LD_LIBRARY_PATH` to `NPI/lib/LINUXAMD64`, not `NPI/lib/linux64` or `platform/linux64/bin`. User must manually add paths.

### Optimization Direction

- [ ] Add `$VERDI_HOME/platform/linux64/bin` to RUNPATH at link time (covers transitive deps)
- [ ] Or: vsignal startup could prepend `$VERDI_HOME`-derived paths to `LD_LIBRARY_PATH` before fork, similar to the old `npi_env` re-exec approach (removed in b630951)
- [ ] Document the `LD_LIBRARY_PATH` requirement clearly in `--help` and error messages

---

## 2. Usability Assessment

### What Works Well

1. **Daemon model**: open-once-query-many avoids repeated design load (10+ seconds for KDB)
2. **Auto-detect**: server found by searching upward from CWD for `.vtool/` — no manual socket paths
3. **`--json` mode**: structured output for programmatic use, all commands support it
4. **`--compact` mode** (vwave): reduces `signals` output from 2362 → 296 chars (87% reduction)
5. **Design switch detection**: correctly detects and restarts when opening a different file

### Pain Points

1. **No batch trace in vsignal**: each `driver`/`load`/`fanin` call is one signal. Tracing 10 signals = 10 separate invocations (10 round-trips, 10 process forks for client).
2. **Range query single-signal limit** (vwave): `get -b -e` only supports one signal. Multi-signal time-range dump requires N separate calls.
3. **No pagination/cursor** for large results: `find "*gpio*"` returns 175 matches in one 14 KB response. No way to request page 2 or stream incrementally.
4. **Signal path verbosity**: full hierarchical paths like `tb_top.top_inst.u_digit_top.u_system.u_cortexm0integration.HCLK` are repeated in every response field, inflating output significantly.

---

## 3. Response Size Analysis

### vwave

| Command | Typical Size | Notes |
|---------|-------------|-------|
| status | 137 chars | Minimal |
| info | 184 chars | Minimal |
| get (1 signal, 1 time) | 128 chars | Compact |
| get (5 signals, 1 time) | 424 chars | Linear growth with signal count |
| get (1 signal, time range, limit=1000) | 29 KB | Dominated by change array |
| get (1 signal, time range, limit=10) | ~500 chars | `--limit` effective |
| edge | 145 chars | Minimal |
| find (broad pattern) | 14 KB / 175 matches | No pagination |
| signals (normal) | 2362 chars | Full metadata |
| signals (compact) | 296 chars | 87% reduction |

### vsignal

| Command | Typical Size | Notes |
|---------|-------------|-------|
| driver | 266 chars | Minimal |
| driver --pass-mod | 918 chars | Grows with cross-module depth |
| load --pass-mod --assign-cell | ~5 KB / 56 loads | No limit mechanism |
| fanout | 42 KB / 173 registers | **Large**, no truncation |
| conn | 4.5 KB / 57 ports | Moderate |
| trace | 439 chars | Path usually short |

### Key Observation

vsignal has **no `--limit` or `--compact` mechanism**. A fanout query returning 173 registers produces 42 KB of JSON with full hierarchical paths. For AI agent context windows, this is significant.

---

## 4. Optimization Directions

### P0 — High Impact, Required for AI Agent Use

#### 4.1 vsignal: Add `--compact` and `--limit`

vsignal lacks both features that vwave already has. For large fanout/fanin results:
- `--compact`: return short names (leaf only) instead of `full_name`
- `--limit N`: cap result count, report `total` vs `returned` (like vwave's range query)

Expected savings: 42 KB fanout → ~5 KB with compact + limit=20.

#### 4.2 Batch Query Support

Add multi-signal batch for common vsignal operations:

```
vsignal driver -s sig1 -s sig2 -s sig3 --json
vsignal driver -f signal_list.txt --json
```

Reduces N round-trips to 1. Matches vwave's multi-signal `get -s ... -s ...` pattern.

Similarly for vwave, extend range query to support multiple signals:

```
vwave get -s sig1 -s sig2 -b T0 -e T1 --json
```

### P1 — Usability Improvements

#### 4.3 Error Messages with Recovery Hints

When vsignal fails due to missing `LD_LIBRARY_PATH`, the error is:
```
Error: Server process exited unexpectedly.
Check log: .vtool/vsignal_run/vsignal_server.log
```

Should parse the log and surface the root cause:
```
Error: Design load failed — liblpinstrumentdb.so not found.
Hint: export LD_LIBRARY_PATH=$VERDI_HOME/platform/linux64/bin:$LD_LIBRARY_PATH
```

#### 4.4 Signal Path Shortening in Output

For repeated paths sharing a common prefix, consider:
- A `prefix` field at the response level, with signals using relative paths
- Or `--compact` mode using leaf names only

Example (current):
```json
{"full_name": "tb_top.top_inst.u_digit_top.u_system.u_cortexm0integration.HCLK"}
{"full_name": "tb_top.top_inst.u_digit_top.u_system.u_cortexm0integration.u_top.hclk"}
```

With prefix extraction:
```json
{"prefix": "tb_top.top_inst.u_digit_top.u_system.u_cortexm0integration",
 "signals": [{"name": "HCLK"}, {"name": "u_top.hclk"}]}
```

### P2 — Completeness Gaps

#### 4.5 vwave: Multi-Signal Range Query

Current limitation: `get -b -e` accepts only one signal. For waveform comparison workflows (e.g. comparing clock vs reset over a window), the agent must make N calls.

#### 4.6 vsignal: Design Info Enrichment

`vsignal info` currently returns only `design_source` and `pid`. Could include:
- Top module name
- Total instance/net/port counts
- Design hierarchy depth

This helps AI agents understand the design scope without exploratory queries.

#### 4.7 vwave: Scope-Filtered Signal Listing

`vwave find` searches globally. For large designs, a scope-filtered listing with metadata would reduce the need for multi-step discovery:

```
vwave signals tb_top.u_cpu --recursive --compact --json
```

### P3 — Future Considerations

#### 4.8 Streaming/Pagination for Large Results

For results > 10 KB, consider:
- `--offset N --limit M` pagination
- Or a cursor-based approach for iterative exploration

#### 4.9 Multi-Command Pipeline

A single request containing multiple queries, returning results in one response:

```json
{"commands": [
  {"cmd": "get", "signal": "clk", "time": 1000},
  {"cmd": "get", "signal": "rst", "time": 1000},
  {"cmd": "edge", "signal": "clk", "time": 1000, "edge": "rising"}
]}
```

This would eliminate per-command process spawn overhead (currently each CLI invocation forks a client process, connects to socket, sends request, receives response).

---

## 5. Test Matrix Summary

### vwave (18/18 passed)

| Category | Tests | Status |
|----------|-------|--------|
| Lifecycle (open/close/status) | 4 | PASS |
| Hierarchy (scopes/signals/find) | 5 | PASS |
| Value queries (get single/multi/range/file) | 4 | PASS |
| Analysis (edge/vc-count/signal-info) | 3 | PASS |
| Cross-version (W-2024.09 FSDB) | 2 | PASS |

### vsignal (12/12 passed)

| Category | Tests | Status |
|----------|-------|--------|
| Lifecycle (open/close/status/info) | 4 | PASS |
| Trace (driver/load/fanin/fanout/trace/conn) | 7 | PASS |
| Cross-version (W-2024.09 KDB) | 1 | PASS |

### Bugs Fixed During This Session

1. **Design-switch self-comparison** (both tools): `source()` compared with itself, switch path unreachable
2. **Relative path after chdir** (vsignal): `-dbdir simv.daidir` broken by chdir to run_dir
3. **NPI logs not contained** (vsignal): vsignalLog/ scattered in CWD

---

## 6. Iteration Plan

This is the first assessment (v1). Planned follow-up iterations:

- **v2**: Implement P0 items (--compact/--limit for vsignal, batch query), re-assess
- **v3**: Measure AI agent end-to-end workflow (Claude Code skill integration), identify remaining friction
- **v4**: Large design stress test (100K+ instances, deep hierarchy)
