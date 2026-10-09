#!/usr/bin/env bash
# 在 glibc 2.17 环境构建，再于纯 CentOS 7 容器验证 reader；不访问网络。
set -euo pipefail
probe_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
project_root="$(cd "$probe_root/.." && pwd)"
builder_image=quay.io/pypa/manylinux2014_x86_64:latest
runtime_image=centos:7
task_user="$(id -u):$(id -g)"
for reader_version in T-2022.06-SP2 Y-2026.03-SP2; do
    sdk_path="/opt/Synopsys/verdi/$reader_version/share/FsdbReader"
    build_dir="$probe_root/build/centos7/$reader_version"
    evidence_dir="$probe_root/evidence/centos7/$reader_version"
    mkdir -p "$build_dir/lib" "$evidence_dir"
    docker image inspect --format '{{.Id}}' "$builder_image" >"$evidence_dir/builder_image.txt"
    docker image inspect --format '{{.Id}}' "$runtime_image" >"$evidence_dir/runtime_image.txt"
    if [[ "$reader_version" == T-2022.06-SP2 ]]; then
    docker run --rm --network none --user "$task_user" \
        -v "$probe_root/probes:/src:ro" -v "$sdk_path:/sdk:ro" -v "$build_dir:/out" \
        "$builder_image" /bin/bash -c '
            set -eu
            cat /etc/redhat-release
            getconf GNU_LIBC_VERSION
            g++ --version | head -1
            g++ -std=c++14 -Wall -Wextra -O2 -static-libstdc++ -static-libgcc \
                -isystem /sdk /src/reader_probe.cpp -L/sdk/linux64 \
                -Wl,-rpath,"\$ORIGIN/lib" -lnffr -lnsys -lz -ldl -lpthread \
                -o /out/reader_probe
        ' >"$evidence_dir/build.log" 2>&1
    else
        # 用同一 glibc 2.17 探针加载新库，隔离动态库自身的兼容性问题。
        cp "$probe_root/build/centos7/T-2022.06-SP2/reader_probe" "$build_dir/reader_probe"
        printf 'Reuse probe built against T-2022.06-SP2; test loader only.\n' >"$evidence_dir/build.log"
    fi
    # 使用 $ORIGIN/lib，使运行时只需探针和相邻的 reader 库目录。
    readelf -d --version-info "$build_dir/reader_probe" >"$evidence_dir/probe.elf.txt"
    set +e
    docker run --rm --network none --user "$task_user" \
        -v "$build_dir:/app:ro" -v "$sdk_path/linux64:/app/lib:ro" \
        -v "$project_root/test_vwave/tb_top.fsdb:/sample.fsdb:ro" \
        -e SNPSLMD_LICENSE_FILE=1@127.0.0.1 -e LM_LICENSE_FILE=1@127.0.0.1 \
        "$runtime_image" /bin/bash -c '
            cat /etc/centos-release
            getconf GNU_LIBC_VERSION
            ldd /app/reader_probe
            exec /app/reader_probe /sample.fsdb tb.clk
        ' >"$evidence_dir/runtime.stdout.txt" 2>"$evidence_dir/runtime.stderr.txt"
    runtime_rc=$?
    set -e
    printf '%s\n' "$runtime_rc" >"$evidence_dir/runtime.exit_code.txt"
    printf '%s: CentOS 7 exit=%s\n' "$reader_version" "$runtime_rc"
    if [[ "$reader_version" == T-2022.06-SP2 && "$runtime_rc" != 0 ]]; then
        cat "$evidence_dir/runtime.stderr.txt"
        exit 1
    fi
    if [[ "$reader_version" == Y-2026.03-SP2 && "$runtime_rc" == 0 ]]; then
        printf '2026 版意外通过，请检查容器与库版本。\n' >&2
        exit 1
    fi
done
python3 - "$probe_root" <<'PY'
import json
from pathlib import Path
import sys
root = Path(sys.argv[1])
old = root / 'evidence/centos7/T-2022.06-SP2'
new = root / 'evidence/centos7/Y-2026.03-SP2'
records = [json.loads(line) for line in (old / 'runtime.stdout.txt').read_text().splitlines()
           if line.startswith('{')]
baseline = [json.loads(line) for line in (root / 'evidence/probe/reader.jsonl').read_text().splitlines()]
assert records == baseline, 'CentOS 7 / 2022 reader output differs from host / 2026 baseline'
assert (old / 'runtime.exit_code.txt').read_text().strip() == '0'
assert (new / 'runtime.exit_code.txt').read_text().strip() == '1'
assert "GLIBC_2.28' not found" in (new / 'runtime.stderr.txt').read_text()
conditions = {
    'runtime': 'CentOS Linux 7.9.2009, glibc 2.17',
    'network': 'Docker --network none',
    'license_env': 'SNPSLMD_LICENSE_FILE=LM_LICENSE_FILE=1@127.0.0.1',
    'vendor_mount': 'FsdbReader/linux64 only; no complete Verdi installation',
    'probe_build': 'manylinux2014 / devtoolset-10, static libstdc++ and libgcc',
    'baseline_equal': True,
    'events': sum(row['type'] == 'event' for row in records),
    '2022_exit': 0, '2026_exit': 1,
    '2026_probe': 'Same executable as 2022 run; loader compatibility test only',
}
(root / 'evidence/centos7/validation.json').write_text(json.dumps(conditions, indent=2) + '\n')
(old / 'reader.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in records))
print('CentOS 7 / 2022 reader 与已有 2026 reader 输出完全一致，2026 库加载失败符合预期。')
PY
