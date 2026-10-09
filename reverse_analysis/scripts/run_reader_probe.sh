#!/usr/bin/env bash
# 将许可证地址设为本机无服务端口，观察 reader 是否仍可完成读取。
set -euo pipefail
task_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
reader_home="${1:-/opt/Synopsys/verdi/Y-2026.03-SP2}"
sample_path="${2:-$task_root/test_vwave/tb_top.fsdb}"
signal_path="${3:-tb.clk}"
probe_root="$task_root/reverse_analysis"
evidence_dir="$probe_root/evidence/probe"
reader_dir="$reader_home/share/FsdbReader"
mkdir -p "$probe_root/build" "$evidence_dir"
g++ -std=c++14 -Wall -Wextra -O2 -isystem "$reader_dir" \
    "$probe_root/probes/reader_probe.cpp" -L"$reader_dir/linux64" \
    -Wl,-rpath,"$reader_dir/linux64" -lnffr -lnsys -lz -ldl -lpthread \
    -o "$probe_root/build/reader_probe" >"$evidence_dir/build.log" 2>&1
readelf -d "$probe_root/build/reader_probe" >"$evidence_dir/probe.elf.txt"
cat >"$evidence_dir/test_conditions.txt" <<EOF
reader_home=$reader_home
sample_path=$sample_path
signal_path=$signal_path
SNPSLMD_LICENSE_FILE=1@127.0.0.1
LM_LICENSE_FILE=1@127.0.0.1
trace=network (all child processes)
timeout=20s
EOF
env SNPSLMD_LICENSE_FILE=1@127.0.0.1 LM_LICENSE_FILE=1@127.0.0.1 \
    strace -f -e trace=network -o "$evidence_dir/network.strace.txt" \
    timeout 20 "$probe_root/build/reader_probe" "$sample_path" "$signal_path" \
    >"$evidence_dir/reader.stdout.txt" 2>"$evidence_dir/reader.stderr.txt"
python3 - "$evidence_dir" <<'PY'
import json
from pathlib import Path
import sys
out = Path(sys.argv[1])
records = [json.loads(line) for line in (out / 'reader.stdout.txt').read_text().splitlines()
           if line.startswith('{')]
(out / 'reader.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in records))
print(json.dumps(records[0]))
print(json.dumps(records[-1]))
PY
printf '读取成功；证据保存至 %s\n' "$evidence_dir"
