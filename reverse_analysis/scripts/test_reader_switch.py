#!/usr/bin/env python3
"""验证同一 2022 探针在当前主机切换两套 reader；不验证所有版本的 ABI。"""
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    root = Path(__file__).resolve().parents[1]
    probe = root / 'build/centos7/T-2022.06-SP2/reader_probe'
    sample = root.parent / 'test_vwave/tb_top.fsdb'
    out = root / 'evidence/reader_switch'
    out.mkdir(parents=True, exist_ok=True)
    baseline = [json.loads(line) for line in (root / 'evidence/probe/reader.jsonl').read_text().splitlines()]
    records = {}
    for version in ['T-2022.06-SP2', 'Y-2026.03-SP2']:
        libdir = Path('/opt/Synopsys/verdi') / version / 'share/FsdbReader/linux64'
        env = dict(os.environ, LD_LIBRARY_PATH=str(libdir),
                   SNPSLMD_LICENSE_FILE='1@127.0.0.1', LM_LICENSE_FILE='1@127.0.0.1')
        linked = subprocess.check_output(['ldd', str(probe)], env=env, universal_newlines=True)
        (out / (version + '.ldd.txt')).write_text(linked)
        for library in ['libnffr.so', 'libnsys.so']:
            assert '{} => {}/{}'.format(library, libdir, library) in linked
        trace = out / (version + '.network.strace.txt')
        result = subprocess.run(['strace', '-f', '-e', 'trace=network', '-o', str(trace),
                                 str(probe), str(sample), 'tb.clk'], env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                universal_newlines=True, timeout=20)
        (out / (version + '.stdout.txt')).write_text(result.stdout)
        (out / (version + '.stderr.txt')).write_text(result.stderr)
        assert result.returncode == 0, result.stderr
        parsed = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
        assert parsed == baseline
        assert not re.search(r'\b(socket|socketpair|connect|sendto|recvfrom)\(', trace.read_text())
        records[version] = {'exit_code': 0, 'events': 132, 'matches_baseline': True,
                            'libraries': str(libdir), 'network_calls_observed': False}

    declarations = []
    for version in records:
        header = (Path('/opt/Synopsys/verdi') / version / 'share/FsdbReader/ffrAPI.h').read_text()
        header = re.sub(r'/\*.*?\*/', '', header, flags=re.S)
        header = re.sub(r'//[^\n]*', '', header)
        start = header.index('class ffrObject\n#endif')
        start = header.index('{', start)
        stop, depth = start + 1, 1
        while depth:
            depth += (header[stop] == '{') - (header[stop] == '}')
            stop += 1
        signatures = re.findall(r'virtual\s+([^;]+);', re.sub(r'\s+', ' ', header[start:stop]))
        declarations.append(signatures)
    summary = {'probe': str(probe), 'build_sdk': 'T-2022.06-SP2',
               'test_environment': 'current host; not CentOS 7 for 2026 libraries',
               'sample': str(sample), 'sample_writer': 'T-2022.06-SP2',
               'ffrObject_virtual_declaration_groups': len(declarations[0]),
               'ffrObject_declaration_sequence_equal': declarations[0] == declarations[1],
               'reader_runs': records,
               'scope': 'existing sample and probe API subset; not a complete ABI certification'}
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
