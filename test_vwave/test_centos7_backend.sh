#!/usr/bin/env bash
# 构建完整轻量 CLI / FFR worker，再在无网络的纯 CentOS 7 环境验收。
set -euo pipefail
backend_project="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
backend_out="$backend_project/build/centos7_backend"
backend_sdk22=/opt/Synopsys/verdi/T-2022.06-SP2/share/FsdbReader
backend_sdk26=/opt/Synopsys/verdi/Y-2026.03-SP2/share/FsdbReader
backend_user="$(id -u):$(id -g)"
mkdir -p "$backend_out"
timeout 180 docker run --rm --network none --user "$backend_user" \
    -v "$backend_project:/src:ro" -v "$backend_out:/src/build/centos7_backend" \
    -v "$backend_sdk22:/sdk/share/FsdbReader:ro" -w /src \
    quay.io/pypa/manylinux2014_x86_64:latest /bin/bash -c '
        set -eu
        getconf GNU_LIBC_VERSION
        g++ --version | head -1
        make vwave-ffr FFR_SDK_HOME=/sdk OBJ_DIR=build/centos7_backend/obj \
            BIN_DIR=build/centos7_backend/bin
    ' > "$backend_out/build.log" 2>&1
for backend_binary in vwave vwave-ffr-worker; do
    readelf -d --version-info "$backend_out/bin/$backend_binary" > "$backend_out/$backend_binary.elf.txt"
done
timeout 90 docker run --rm --network none --user "$backend_user" \
    -v "$backend_out/bin:/app:ro" \
    -v "$backend_sdk22/linux64:/sdk2022/share/FsdbReader/linux64:ro" \
    -v "$backend_sdk26/linux64:/sdk2026/share/FsdbReader/linux64:ro" \
    -v "$backend_project/test_vwave/tb_top.fsdb:/sample.fsdb:ro" \
    -e SNPSLMD_LICENSE_FILE=1@127.0.0.1 -e LM_LICENSE_FILE=1@127.0.0.1 \
    centos:7 /bin/bash -c '
        set -eu
        getconf GNU_LIBC_VERSION
        /app/vwave open /sample.fsdb --backend ffr --verdi-home /sdk2022 --run-dir /tmp/session --json
        /app/vwave info --run-dir /tmp/session --json
        result=$(/app/vwave get -s tb.clk -t 25000 --run-dir /tmp/session --json)
        printf "%s\n" "$result"
        [[ "$result" == *"\"value\":\"1\""* ]]
        result=$(/app/vwave vc-count -s tb.clk --run-dir /tmp/session --json)
        printf "%s\n" "$result"
        [[ "$result" == *"\"count\":132"* ]]
        if /app/vwave open /sample.fsdb --backend ffr --verdi-home /sdk2026 --run-dir /tmp/session --json; then
            echo "ERROR: 2026 reader unexpectedly loaded on glibc 2.17" >&2
            exit 1
        fi
        # 加载预检失败应保留原来的可查询会话。
        /app/vwave status --run-dir /tmp/session --json
        /app/vwave close --run-dir /tmp/session --json
    ' > "$backend_out/runtime.stdout.txt" 2> "$backend_out/runtime.stderr.txt"
rg -q "GLIBC_2\.(25|27|28).*not found" "$backend_out/runtime.stderr.txt"
printf 'PASS: CentOS 7 / 2022 FFR; 2026 rejected before replacing session; logs: %s\n' "$backend_out"
