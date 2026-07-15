#!/bin/bash
# check-tools.sh — Verify vwave/vsignal availability for AI agents
# Usage: .github/skills/check-tools.sh

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

ok()   { echo -e "${GREEN}✓${NC} $1"; }
fail() { echo -e "${RED}✗${NC} $1"; }

errors=0

# Check vwave
if command -v vwave &>/dev/null; then
    ok "vwave found: $(command -v vwave)"
elif [[ -x "$(git rev-parse --show-toplevel 2>/dev/null)/build/bin/vwave" ]]; then
    ok "vwave found: $(git rev-parse --show-toplevel)/build/bin/vwave"
else
    fail "vwave not found on PATH or build/bin/"
    ((errors++))
fi

# Check vsignal
if command -v vsignal &>/dev/null; then
    ok "vsignal found: $(command -v vsignal)"
elif [[ -x "$(git rev-parse --show-toplevel 2>/dev/null)/build/bin/vsignal" ]]; then
    ok "vsignal found: $(git rev-parse --show-toplevel)/build/bin/vsignal"
else
    fail "vsignal not found on PATH or build/bin/"
    ((errors++))
fi

# Check VERDI_HOME
if [[ -n "${VERDI_HOME:-}" ]]; then
    ok "VERDI_HOME=$VERDI_HOME"
else
    fail "VERDI_HOME not set (Synopsys Verdi NPI required)"
    ((errors++))
fi

# Check LD_LIBRARY_PATH contains NPI lib dir
npi_lib="${VERDI_HOME:-}/share/NPI/lib/linux64"
if [[ -n "${VERDI_HOME:-}" && ":${LD_LIBRARY_PATH:-}:" == *":${npi_lib}:"* ]]; then
    ok "LD_LIBRARY_PATH contains $npi_lib"
elif [[ -n "${VERDI_HOME:-}" ]]; then
    fail "LD_LIBRARY_PATH missing $npi_lib (add: export LD_LIBRARY_PATH=\$VERDI_HOME/share/NPI/lib/linux64:\$LD_LIBRARY_PATH)"
    ((errors++))
fi

# Check running servers
if [[ -f .vtool/wave_run/wave_server.pid ]]; then
    pid=$(cat .vtool/wave_run/wave_server.pid)
    if kill -0 "$pid" 2>/dev/null; then
        ok "vwave server running (PID $pid)"
    else
        fail "vwave PID file exists but process $pid is dead"
    fi
else
    echo "  vwave server: not running"
fi

if [[ -f .vtool/vsignal_run/vsignal_server.pid ]]; then
    pid=$(cat .vtool/vsignal_run/vsignal_server.pid)
    if kill -0 "$pid" 2>/dev/null; then
        ok "vsignal server running (PID $pid)"
    else
        fail "vsignal PID file exists but process $pid is dead"
    fi
else
    echo "  vsignal server: not running"
fi

if ((errors > 0)); then
    echo ""
    echo "$errors issue(s) found. See above."
    exit 1
fi

echo ""
echo "All checks passed."
