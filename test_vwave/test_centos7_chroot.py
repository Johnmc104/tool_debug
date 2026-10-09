#!/usr/bin/env python3
"""使用已解压的 CentOS 7 运行库，验证完整 CLI/worker 启动流程，无需 mount。"""
import argparse
import json
import os
import re
from pathlib import Path
import shutil
import subprocess

PROJECT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rootfs', type=Path, required=True, help='包含 CentOS 7 usr/lib64 和 lib64 的目录')
    parser.add_argument('--binary-dir', type=Path, default=PROJECT / 'release/bin')
    args = parser.parse_args()
    root = args.rootfs.resolve()
    assert '7.9.2009' in (root / 'etc/centos-release').read_text()
    (root / 'app').mkdir(exist_ok=True)
    (root / 'tmp').mkdir(exist_ok=True)
    for name in ['vwave', 'vwave-ffr-worker']:
        shutil.copy2(str(args.binary_dir / name), str(root / 'app' / name))
    shutil.copy2(str(PROJECT / 'test_vwave/tb_top.fsdb'), str(root / 'sample.fsdb'))
    for version, target in [('T-2022.06-SP2', 'sdk2022'), ('Y-2026.03-SP2', 'sdk2026')]:
        directory = root / target / 'share/FsdbReader/linux64'
        directory.mkdir(parents=True, exist_ok=True)
        for library in ['libnsys.so', 'libnffr.so']:
            shutil.copy2('/opt/Synopsys/verdi/' + version + '/share/FsdbReader/linux64/' + library,
                         str(directory / library))
    env = dict(os.environ, SNPSLMD_LICENSE_FILE='1@127.0.0.1', LM_LICENSE_FILE='1@127.0.0.1')
    # 测试用户空间基线，避免宿主机预加载项进入隔离环境。
    env.pop('LD_PRELOAD', None)
    env.pop('LD_LIBRARY_PATH', None)
    records = []
    def run(*command, ok=True):
        p = subprocess.run(['unshare', '-Ur', 'chroot', str(root)] + list(command), env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        records.append({'command': list(command), 'exit': p.returncode,
                        'stdout': p.stdout.decode(), 'stderr': p.stderr.decode()})
        if ok and p.returncode:
            raise AssertionError(p.stderr.decode())
        if not ok:
            assert p.returncode != 0
        return p
    def cli(*command, ok=True):
        return run('/app/vwave', *command, '--run-dir', '/tmp/session', '--json', ok=ok)
    try:
        runtime = run('/lib64/libc.so.6')
        assert b'stable release version 2.17' in runtime.stdout
        pid = json.loads(cli('open', '/sample.fsdb', '--backend', 'ffr', '--verdi-home', '/sdk2022').stdout)['pid']
        info = json.loads(cli('info').stdout)['data']
        assert info['backend'] == 'ffr' and info['version'] == '6.0'
        point = json.loads(cli('get', '-s', 'tb.clk', '-t', '25000').stdout)['data']['values'][0]
        assert point['value'] == '1'
        count = json.loads(cli('vc-count', '-s', 'tb.clk').stdout)['data']['count']
        assert count == 132
        rejected = cli('open', '/sample.fsdb', '--backend', 'ffr', '--verdi-home', '/sdk2026', ok=False)
        missing = re.search(br'GLIBC_2\.(\d+).*not found', rejected.stderr)
        assert missing and int(missing.group(1)) > 17
        assert json.loads(cli('status').stdout)['data']['pid'] == pid
        assert json.loads(cli('open', '/sample.fsdb', '--backend', 'ffr', '--verdi-home', '/sdk2022').stdout)['pid'] == pid
    finally:
        cli('close')
        output = root.parent / 'chroot_validation.json'
        output.write_text(json.dumps({'runtime': 'CentOS 7.9 / glibc 2.17',
                                      'method': 'unshare -Ur chroot; no mounts; full CLI fork/exec path',
                                      'network_isolation': False,
                                      'license_env': '1@127.0.0.1', 'records': records}, indent=2) + '\n')
    assert not (root / 'tmp/session/wave_server.pid').exists()
    assert not (root / 'tmp/session/session.json').exists()
    print('PASS: CentOS 7.9 / glibc 2.17 CLI + FFR worker; open/get/count/switch failure/close')


if __name__ == '__main__':
    main()
