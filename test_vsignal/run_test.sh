#!/bin/bash
# test_vsignal/run_test.sh — Build KDB, run vsignal commands, verify results.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VSIGNAL="$ROOT_DIR/build/bin/vsignal"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

PASS=0
FAIL=0
TOTAL=0

check() {
    local desc="$1"
    local result="$2"
    local expect="$3"
    TOTAL=$((TOTAL + 1))
    if echo "$result" | grep -q "$expect"; then
        PASS=$((PASS + 1))
        echo -e "  ${GREEN}PASS${NC} [$TOTAL] $desc"
    else
        FAIL=$((FAIL + 1))
        echo -e "  ${RED}FAIL${NC} [$TOTAL] $desc"
        echo "    expected pattern: $expect"
        echo "    got: $result"
    fi
}

check_rc() {
    local desc="$1"
    local rc="$2"
    local expect_rc="$3"
    TOTAL=$((TOTAL + 1))
    if [ "$rc" -eq "$expect_rc" ]; then
        PASS=$((PASS + 1))
        echo -e "  ${GREEN}PASS${NC} [$TOTAL] $desc"
    else
        FAIL=$((FAIL + 1))
        echo -e "  ${RED}FAIL${NC} [$TOTAL] $desc"
        echo "    expected rc=$expect_rc, got rc=$rc"
    fi
}

# ══════════════════════════════════════════════════════════════════════════════
echo "═══════════════════════════════════════════════════════"
echo " vsignal Test Suite"
echo "═══════════════════════════════════════════════════════"

# ── Step 0: Check binary exists ──────────────────────────────────────────────
if [ ! -x "$VSIGNAL" ]; then
    echo -e "${RED}ERROR: $VSIGNAL not found. Run 'make vsignal' first.${NC}"
    exit 1
fi

# ── Step 1: Build KDB ────────────────────────────────────────────────────────
echo ""
echo "── Step 1: Build KDB database ──"
cd "$SCRIPT_DIR"

# Clean previous artifacts
rm -rf simv simv.daidir csrc .vsignal_run

echo "  Running: vcs -sverilog -kdb -q example.v"
if ! vcs -sverilog -kdb -q example.v -o simv 2>&1 | tail -3; then
    echo -e "${RED}ERROR: VCS compilation failed${NC}"
    exit 1
fi

if [ -d "simv.daidir/kdb.elab++" ]; then
    echo -e "  ${GREEN}KDB generated: simv.daidir/kdb.elab++${NC}"
else
    echo -e "${RED}ERROR: KDB not generated${NC}"
    exit 1
fi

# ── Step 2: Open design ──────────────────────────────────────────────────────
echo ""
echo "── Step 2: Open design (KDB mode) ──"

RESULT=$("$VSIGNAL" open -dbdir simv.daidir --json 2>/dev/null || true)
check "open -dbdir returns ok" "$RESULT" '"status":"ok"'
check "open -dbdir reports pid" "$RESULT" '"pid":'

# Give server a moment to fully initialize
sleep 1

# ── Step 3: Status ────────────────────────────────────────────────────────────
echo ""
echo "── Step 3: Status check ──"

RESULT=$("$VSIGNAL" status --json 2>/dev/null || true)
check "status returns ok" "$RESULT" '"status":"ok"'
check "status shows design_source" "$RESULT" 'design_source'
check "status shows pid" "$RESULT" '"pid":'

# ── Step 4: Info ──────────────────────────────────────────────────────────────
echo ""
echo "── Step 4: Design info ──"

RESULT=$("$VSIGNAL" info --json 2>/dev/null || true)
check "info returns ok" "$RESULT" '"status":"ok"'

# ── Step 5: Driver trace ─────────────────────────────────────────────────────
echo ""
echo "── Step 5: Trace drivers ──"

RESULT=$("$VSIGNAL" driver top.data_out --json 2>/dev/null || true)
check "driver returns ok" "$RESULT" '"status":"ok"'
check "driver shows count" "$RESULT" '"count":'
check "driver shows drivers array" "$RESULT" '"drivers":'

# ── Step 6: Load trace ───────────────────────────────────────────────────────
echo ""
echo "── Step 6: Trace loads ──"

RESULT=$("$VSIGNAL" load top.data_in --json 2>/dev/null || true)
check "load returns ok" "$RESULT" '"status":"ok"'
check "load shows count" "$RESULT" '"count":'
check "load shows loads array" "$RESULT" '"loads":'

# ── Step 7: FanIn register ───────────────────────────────────────────────────
echo ""
echo "── Step 7: FanIn register connections ──"

RESULT=$("$VSIGNAL" fanin top.data_out --json 2>/dev/null || true)
check "fanin returns ok" "$RESULT" '"status":"ok"'
check "fanin shows count" "$RESULT" '"count":'
check "fanin shows fanin array" "$RESULT" '"fanin":'

# ── Step 8: FanOut register ──────────────────────────────────────────────────
echo ""
echo "── Step 8: FanOut register connections ──"

RESULT=$("$VSIGNAL" fanout top.clk --json 2>/dev/null || true)
check "fanout returns ok" "$RESULT" '"status":"ok"'
check "fanout shows count" "$RESULT" '"count":'
check "fanout shows fanout array" "$RESULT" '"fanout":'

# ── Step 9: Instance connections ─────────────────────────────────────────────
echo ""
echo "── Step 9: Instance connections ──"

RESULT=$("$VSIGNAL" conn top.u_sub --json 2>/dev/null || true)
check "conn returns ok" "$RESULT" '"status":"ok"'
check "conn shows connections" "$RESULT" '"connections":'

# ── Step 10: Signal-to-signal trace ──────────────────────────────────────────
echo ""
echo "── Step 10: Signal-to-signal path trace ──"

RESULT=$("$VSIGNAL" trace top.data_in top.data_out --json 2>/dev/null || true)
check "trace returns ok or no_path" "$RESULT" '"status":'
check "trace shows from/to" "$RESULT" 'from'

# ── Step 11: Error handling ──────────────────────────────────────────────────
echo ""
echo "── Step 11: Error handling ──"

RESULT=$("$VSIGNAL" driver nonexistent.signal --json 2>/dev/null || true)
check "invalid signal returns response" "$RESULT" '"status":'

# ── Step 12: Re-open (idempotent) ────────────────────────────────────────────
echo ""
echo "── Step 12: Re-open same design (idempotent) ──"

RESULT=$("$VSIGNAL" open -dbdir simv.daidir --json 2>/dev/null || true)
check "re-open returns ok" "$RESULT" '"status":"ok"'
check "re-open says already running" "$RESULT" 'already running\|Design loaded'

# ── Step 13: Close ────────────────────────────────────────────────────────────
echo ""
echo "── Step 13: Close design ──"

"$VSIGNAL" close 2>/dev/null
RC=$?
check_rc "close exits 0" $RC 0

# Verify server is gone
sleep 1
RESULT=$("$VSIGNAL" status --json 2>/dev/null || echo "no_server")
check "server stopped after close" "$RESULT" 'No active design\|no_server\|error'

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "═══════════════════════════════════════════════════════"
echo -e " Results: ${GREEN}$PASS passed${NC}, ${RED}$FAIL failed${NC}, $TOTAL total"
echo "═══════════════════════════════════════════════════════"

# Cleanup
rm -rf .vsignal_run

if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}Some tests failed.${NC}"
    exit 1
fi
