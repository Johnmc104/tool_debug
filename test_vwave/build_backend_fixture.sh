#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 2 ]]; then
    echo "Usage: $0 /path/to/vcs /path/to/verdi" >&2
    exit 1
fi
fixture_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fixture_vcs="$(realpath "$1")"
fixture_verdi="$(realpath "$2")"
fixture_version="$(basename "$fixture_verdi")"
fixture_out="$fixture_root/build/backend_fixtures/$fixture_version"
mkdir -p "$fixture_out"
cd "$fixture_out"
export VCS_HOME="$fixture_vcs" VERDI_HOME="$fixture_verdi"
unset NOVAS_HOME
fixture_pli=()
if [[ "$(basename "$fixture_vcs")" == T-2022* ]]; then
    fixture_pli=(-P "$fixture_verdi/share/PLI/VCS/LINUX64/novas.tab"
                    "$fixture_verdi/share/PLI/VCS/LINUX64/pli.a")
fi
"$fixture_vcs/bin/vcs" -full64 -sverilog -debug_access+all \
    "${fixture_pli[@]}" \
    "$fixture_root/test_vwave/fixtures/backend_fixture.sv" -o simv > build.log 2>&1
if [[ -f fixture.fsdb ]]; then mv fixture.fsdb fixture.previous.fsdb; fi
./simv +fsdb+glitch=0 > sim.log 2>&1
test -s fixture.fsdb
printf '%s\n' "$fixture_out/fixture.fsdb"
