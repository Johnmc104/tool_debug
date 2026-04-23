#!/usr/bin/env bash
# ───────────────────────────────────────────────────────────────────────────────
# run_test.sh — Comprehensive test suite for vwave
#
# Usage:
#   cd <project_root>
#   bash test_vwave/run_test.sh
# ───────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VWAVE="$PROJECT_DIR/build/bin/vwave"
FSDB="$PROJECT_DIR/test_vwave/tb_top.fsdb"
RUNDIR="$PROJECT_DIR/.vtool/wave_run"
TIMEOUT_CMD="timeout 10"

# Counters
PASS=0
FAIL=0
SKIP=0

# Colors
if [[ -t 1 ]]; then
    GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[0;33m'
    CYAN='\033[0;36m'; NC='\033[0m'
else
    GREEN=''; RED=''; YELLOW=''; CYAN=''; NC=''
fi

log_section() { echo -e "\n${CYAN}═══ $1 ═══${NC}"; }
log_test()    { printf "  %-55s " "$1"; }
pass()        { echo -e "${GREEN}PASS${NC}"; PASS=$((PASS + 1)); }
fail()        { echo -e "${RED}FAIL${NC}: $1"; FAIL=$((FAIL + 1)); }

run_vwave() { $TIMEOUT_CMD "$VWAVE" "$@" --json 2>/dev/null || true; }
assert_has() { echo "$1" | grep -qF "$2"; }

# ─── Cleanup any old server ──────────────────────────────────────────────────

cleanup_server() {
    if [[ -f "$RUNDIR/wave_server.pid" ]]; then
        local pid
        pid=$(cat "$RUNDIR/wave_server.pid" 2>/dev/null) || true
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            $TIMEOUT_CMD "$VWAVE" close --fsdb "$FSDB" --json >/dev/null 2>&1 || true
            sleep 1
            kill -9 "$pid" 2>/dev/null || true
        fi
    fi
    rm -rf "$RUNDIR"
    rmdir "$PROJECT_DIR/.vtool" 2>/dev/null || true
}

cleanup_server

# ─── Build ────────────────────────────────────────────────────────────────────

log_section "Build"
log_test "make"
cd "$PROJECT_DIR"
if make > /dev/null 2>&1; then pass; else fail "build failed"; exit 1; fi

# ─── Pre-checks ──────────────────────────────────────────────────────────────

log_section "Pre-checks"

log_test "FSDB file exists"
if [[ -f "$FSDB" ]]; then pass; else fail "not found"; exit 1; fi

log_test "vwave binary exists"
if [[ -x "$VWAVE" ]]; then pass; else fail "not found"; exit 1; fi

# ─── T1: Open waveform ───────────────────────────────────────────────────────

log_section "T1: Open Waveform"

OUT=$(run_vwave open "$FSDB")

log_test "open returns ok"
if assert_has "$OUT" '"status":"ok"'; then pass; else fail "$OUT"; fi

log_test "open reports pid"
if assert_has "$OUT" '"pid":'; then pass; else fail "$OUT"; fi

log_test ".vtool/wave_run directory created"
if [[ -d "$RUNDIR" ]]; then pass; else fail "not found"; fi

log_test "PID file created"
if [[ -f "$RUNDIR/wave_server.pid" ]]; then pass; else fail "not found"; fi

log_test "source_info file created"
if [[ -f "$RUNDIR/source_info" ]]; then pass; else fail "not found"; fi

log_test "log file created"
if [[ -f "$RUNDIR/wave_server.log" ]]; then pass; else fail "not found"; fi

log_test "socket file created"
if [[ -S "$RUNDIR/wave_server.sock" ]]; then pass; else fail "not found"; fi

log_test "server process alive"
PID=$(cat "$RUNDIR/wave_server.pid")
if kill -0 "$PID" 2>/dev/null; then pass; else fail "PID $PID not running"; fi

# ─── T2: Duplicate open (idempotent) ─────────────────────────────────────────

log_section "T2: Duplicate Open"

OUT=$(run_vwave open "$FSDB")

log_test "duplicate open: no error"
if assert_has "$OUT" '"status":"ok"'; then pass; else fail "$OUT"; fi

log_test "duplicate open: same PID"
if assert_has "$OUT" "\"pid\":$PID"; then pass; else fail "$OUT"; fi

# ─── T3: Auto-detect (no --fsdb needed) ──────────────────────────────────────

log_section "T3: Auto-detect"

# Run from test_vwave/ dir (parent of .vtool/wave_run)
cd "$PROJECT_DIR/test_vwave"

OUT=$($TIMEOUT_CMD "$VWAVE" status --json 2>/dev/null || true)

log_test "status via auto-detect"
if assert_has "$OUT" '"status":"ok"'; then pass; else fail "$OUT"; fi

log_test "auto-detect finds correct fsdb"
if assert_has "$OUT" '"fsdb_file"'; then pass; else fail "$OUT"; fi

cd "$PROJECT_DIR"

# ─── T4: Status ──────────────────────────────────────────────────────────────

log_section "T4: Status"

OUT=$(run_vwave --fsdb "$FSDB" status)

log_test "status returns ok"
if assert_has "$OUT" '"status":"ok"'; then pass; else fail "$OUT"; fi

log_test "status has uptime_seconds"
if assert_has "$OUT" '"uptime_seconds"'; then pass; else fail "$OUT"; fi

log_test "status has pid"
if assert_has "$OUT" '"pid"'; then pass; else fail "$OUT"; fi

# ─── T5: File Info ────────────────────────────────────────────────────────────

log_section "T5: File Info"

OUT=$(run_vwave --fsdb "$FSDB" info)

log_test "info returns ok"
if assert_has "$OUT" '"status":"ok"'; then pass; else fail "$OUT"; fi

log_test "min_time = 0"
if assert_has "$OUT" '"min_time":0'; then pass; else fail "$OUT"; fi

log_test "max_time = 3275000"
if assert_has "$OUT" '"max_time":3275000'; then pass; else fail "$OUT"; fi

log_test "scale_unit = 1ps"
if assert_has "$OUT" '"scale_unit":"1ps"'; then pass; else fail "$OUT"; fi

log_test "version = 6.0"
if assert_has "$OUT" '"version":"6.0"'; then pass; else fail "$OUT"; fi

log_test "is_completed = 1"
if assert_has "$OUT" '"is_completed":1'; then pass; else fail "$OUT"; fi

# ─── T6: Scopes ──────────────────────────────────────────────────────────────

log_section "T6: Scopes"

OUT=$(run_vwave --fsdb "$FSDB" scopes)

log_test "top-level scopes contain 'tb'"
if assert_has "$OUT" '"tb"'; then pass; else fail "$OUT"; fi

log_test "top-level scopes contain 'uvm_pkg'"
if assert_has "$OUT" '"uvm_pkg"'; then pass; else fail "$OUT"; fi

log_test "top-level scope count >= 5"
CNT=$(echo "$OUT" | grep -oP '"count":\K[0-9]+' || echo 0)
if [[ "$CNT" -ge 5 ]]; then pass; else fail "count=$CNT"; fi

# Sub-scope
OUT=$(run_vwave --fsdb "$FSDB" scopes tb)

log_test "tb child scopes: tb.intf, tb.dut"
if assert_has "$OUT" '"tb.intf"' && assert_has "$OUT" '"tb.dut"'; then
    pass
else
    fail "$OUT"
fi

log_test "tb child count = 2"
if assert_has "$OUT" '"count":2'; then pass; else fail "$OUT"; fi

# ─── T7: Signals ─────────────────────────────────────────────────────────────

log_section "T7: Signals"

OUT=$(run_vwave --fsdb "$FSDB" signals tb.intf)

log_test "tb.intf signals contain 'clk'"
if assert_has "$OUT" '"tb.intf.clk"'; then pass; else fail "$OUT"; fi

log_test "tb.intf signals contain 'paddr'"
if assert_has "$OUT" '"tb.intf.paddr"'; then pass; else fail "$OUT"; fi

log_test "paddr has left=31, right=0"
if assert_has "$OUT" '"left":31' && assert_has "$OUT" '"right":0'; then
    pass
else
    fail "$OUT"
fi

# ─── T8: Signal Info ─────────────────────────────────────────────────────────

log_section "T8: Signal Info"

OUT=$(run_vwave --fsdb "$FSDB" signal-info tb.intf.paddr)

log_test "signal-info returns ok"
if assert_has "$OUT" '"status":"ok"'; then pass; else fail "$OUT"; fi

log_test "name = tb.intf.paddr"
if assert_has "$OUT" '"name":"tb.intf.paddr"'; then pass; else fail "$OUT"; fi

log_test "left=31, right=0"
if assert_has "$OUT" '"left":31' && assert_has "$OUT" '"right":0'; then
    pass
else
    fail "$OUT"
fi

# Non-existent
OUT=$(run_vwave --fsdb "$FSDB" signal-info tb.intf.nonexistent)
log_test "nonexistent signal returns error"
if assert_has "$OUT" '"status":"error"'; then pass; else fail "$OUT"; fi

# ─── T9: Get Value At ────────────────────────────────────────────────────────

log_section "T9: Get Value At"

OUT=$(run_vwave --fsdb "$FSDB" get-value -s tb.intf.clk -t 500000)

log_test "clk@500000 = 0"
if assert_has "$OUT" '"value":"0"'; then pass; else fail "$OUT"; fi

OUT=$(run_vwave --fsdb "$FSDB" get-value -s tb.intf.clk -t 525000)

log_test "clk@525000 = 1"
if assert_has "$OUT" '"value":"1"'; then pass; else fail "$OUT"; fi

OUT=$(run_vwave --fsdb "$FSDB" get-value -s tb.intf.paddr -t 1500000 -r hex)

log_test "paddr@1500000 (hex) = 00000004"
if assert_has "$OUT" '"value":"00000004"'; then pass; else fail "$OUT"; fi

OUT=$(run_vwave --fsdb "$FSDB" get-value -s tb.intf.paddr -t 0 -r hex)

log_test "paddr@0 (hex) = XXXXXXXX"
if assert_has "$OUT" '"value":"XXXXXXXX"'; then pass; else fail "$OUT"; fi

# ─── T10: Multi-Signal ───────────────────────────────────────────────────────

log_section "T10: Multi-Signal"

OUT=$(run_vwave --fsdb "$FSDB" get-value -s tb.intf.clk -s tb.intf.rstn -t 500000)

log_test "multi: clk=0"
if assert_has "$OUT" '"signal":"tb.intf.clk"' && assert_has "$OUT" '"value":"0"'; then
    pass
else
    fail "$OUT"
fi

log_test "multi: rstn=1"
if assert_has "$OUT" '"signal":"tb.intf.rstn"' && assert_has "$OUT" '"value":"1"'; then
    pass
else
    fail "$OUT"
fi

# ─── T11: Signal File Input ──────────────────────────────────────────────────

log_section "T11: Signal File Input"

SIGFILE="$PROJECT_DIR/test_vwave/test_signals.txt"
cat > "$SIGFILE" << 'EOF'
# Test signal list
tb.intf.clk
tb.intf.rstn
tb.intf.paddr
EOF

OUT=$(run_vwave --fsdb "$FSDB" get-value -f "$SIGFILE" -t 500000 -r hex)

log_test "signal-file: 3 values returned"
SIG_COUNT=$(echo "$OUT" | grep -oF '"signal":' | wc -l)
if [[ "$SIG_COUNT" -eq 3 ]]; then pass; else fail "expected 3, got $SIG_COUNT"; fi

rm -f "$SIGFILE"

# ─── T12: Value Between (range) ──────────────────────────────────────────────

log_section "T12: Value Between"

OUT=$(run_vwave --fsdb "$FSDB" get-value -s tb.intf.clk -b 0 -e 100000)

log_test "clk 0~100000: has changes"
if assert_has "$OUT" '"changes":['; then pass; else fail "$OUT"; fi

log_test "clk 0~100000: t=0 val=0"
if assert_has "$OUT" '"time":0,"value":"0"'; then pass; else fail "$OUT"; fi

log_test "clk 0~100000: t=25000 val=1"
if assert_has "$OUT" '"time":25000,"value":"1"'; then pass; else fail "$OUT"; fi

log_test "clk 0~100000: >= 5 changes"
CC=$(echo "$OUT" | grep -oF '"time":' | wc -l)
if [[ "$CC" -ge 5 ]]; then pass; else fail "only $CC"; fi

OUT=$(run_vwave --fsdb "$FSDB" get-value -s tb.intf.paddr -b 0 -e 3275000 -r hex)

log_test "paddr full range: 3 changes"
CC=$(echo "$OUT" | grep -oF '"time":' | wc -l)
if [[ "$CC" -eq 3 ]]; then pass; else fail "expected 3, got $CC"; fi

log_test "paddr: t=0 XXXXXXXX"
if assert_has "$OUT" '"time":0,"value":"XXXXXXXX"'; then pass; else fail "$OUT"; fi

log_test "paddr: t=10000 00000000"
if assert_has "$OUT" '"time":10000,"value":"00000000"'; then pass; else fail "$OUT"; fi

log_test "paddr: t=1026000 00000004"
if assert_has "$OUT" '"time":1026000,"value":"00000004"'; then pass; else fail "$OUT"; fi

# ─── T13: Radix Formats ──────────────────────────────────────────────────────

log_section "T13: Radix Formats"

OUT=$(run_vwave --fsdb "$FSDB" get-value -s tb.intf.paddr -t 1500000)
log_test "bin: ...100"
if echo "$OUT" | grep -qE '"value":"0+100"'; then pass; else fail "$OUT"; fi

OUT=$(run_vwave --fsdb "$FSDB" get-value -s tb.intf.paddr -t 1500000 -r dec)
log_test "dec: 4"
if assert_has "$OUT" '"value":"4"'; then pass; else fail "$OUT"; fi

OUT=$(run_vwave --fsdb "$FSDB" get-value -s tb.intf.paddr -t 1500000 -r hex)
log_test "hex: 00000004"
if assert_has "$OUT" '"value":"00000004"'; then pass; else fail "$OUT"; fi

# ─── T14: Error Handling ─────────────────────────────────────────────────────

log_section "T14: Error Handling"

OUT=$(run_vwave --fsdb "$FSDB" get-value -s tb.intf.nonexistent -t 500000)
log_test "nonexistent signal"
if assert_has "$OUT" '"error"' || assert_has "$OUT" '"not found"'; then
    pass
else
    fail "$OUT"
fi

OUT=$(run_vwave --fsdb "$FSDB" scopes nonexistent.scope)
log_test "invalid scope"
if assert_has "$OUT" '"status":"error"'; then pass; else fail "$OUT"; fi

# ─── T15: Close + Restart ────────────────────────────────────────────────────

log_section "T15: Close + Restart"

OUT=$(run_vwave --fsdb "$FSDB" status)
log_test "status ok before close"
if assert_has "$OUT" '"status":"ok"'; then pass; else fail "$OUT"; fi

OUT=$(run_vwave --fsdb "$FSDB" close)
log_test "close returns ok"
if assert_has "$OUT" '"status":"ok"'; then pass; else fail "$OUT"; fi

sleep 2

log_test "server process stopped"
if [[ -f "$RUNDIR/wave_server.pid" ]]; then
    PID=$(cat "$RUNDIR/wave_server.pid")
    if kill -0 "$PID" 2>/dev/null; then fail "still running"; else pass; fi
else
    pass
fi

# Re-open
OUT=$(run_vwave open "$FSDB")
log_test "re-open after close"
if assert_has "$OUT" '"status":"ok"'; then pass; else fail "$OUT"; fi

OUT=$(run_vwave --fsdb "$FSDB" get-value -s tb.intf.clk -t 500000)
log_test "value read after restart"
if assert_has "$OUT" '"value":"0"'; then pass; else fail "$OUT"; fi

# ─── T16: Waveform Switch ────────────────────────────────────────────────────

log_section "T16: Waveform Switch"

# Re-open the same file (should be idempotent, treated as already running)
OLD_PID=$(cat "$RUNDIR/wave_server.pid" 2>/dev/null || echo "0")
OUT=$(run_vwave open "$FSDB")
log_test "switch: re-open same file (idempotent)"
if assert_has "$OUT" '"status":"ok"'; then pass; else fail "$OUT"; fi

# Final cleanup
$TIMEOUT_CMD "$VWAVE" close --fsdb "$FSDB" --json >/dev/null 2>&1 || true
sleep 1
rm -rf "$RUNDIR"
rmdir "$PROJECT_DIR/.vtool" 2>/dev/null || true

# ─── Summary ─────────────────────────────────────────────────────────────────

log_section "Summary"
TOTAL=$((PASS + FAIL + SKIP))
echo -e "  Total:  ${TOTAL}"
echo -e "  ${GREEN}Pass:   ${PASS}${NC}"
echo -e "  ${RED}Fail:   ${FAIL}${NC}"
echo -e "  ${YELLOW}Skip:   ${SKIP}${NC}"

if [[ "$FAIL" -eq 0 ]]; then
    echo -e "\n${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "\n${RED}Some tests failed.${NC}"
    exit 1
fi
