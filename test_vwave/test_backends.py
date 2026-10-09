#!/usr/bin/env python3
"""真实 SDK 的双后端差分、加载来源和会话切换验收（Python 3.6+）。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import shutil
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE_KEYS = {'backend', 'sdk_home', 'adapter', 'reader_library', 'support_library',
               'reader_sha256', 'support_sha256'}


def request(directory, cmd, params=None):
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(30)
        connection.connect(str(Path(directory) / 'wave_server.sock'))
        connection.sendall((json.dumps({'id': 1, 'cmd': cmd, 'params': params or {}}) + '\n').encode())
        chunks = bytearray()
        while not chunks.endswith(b'\n'):
            data = connection.recv(65536)
            if not data:
                break
            chunks.extend(data)
    return json.loads(chunks)


def corpus(fixture=False):
    if fixture:
        scope = 'fixture'
        signals = ['fixture.clk', 'fixture.stable', 'fixture.data', 'fixture.ascending',
                   'fixture.wide', 'fixture.mixed', 'fixture.signed_number', 'fixture.child.alias_data',
                   'fixture.memory[0]', 'fixture.memory[1]', 'fixture.\\escaped.name ']
        times = [0, 100, 999, 1000, 1001, 2000, 3000, 4000, 4001, 5000, 6000]
        ranges = [(0, 0), (100, 999), (0, 5000), (999, 2000), (1000, 2000),
                  (4000, 4000), (4001, 5000), (6000, 7000)]
    else:
        scope = 'tb'
        signals = ['tb.clk', 'tb.rstn', 'tb.dut.paddr', 'tb.dut.current_state']
        times = [0, 1000, 25000, 26000, 100000, 3275000, 9999999]
        ranges = [(0, 0), (1000, 1000), (0, 50000), (1000, 50000),
                  (25000, 50000), (0, 9999999), (9999998, 9999999)]
    yield 'file_info', {}
    for depth in [1, 3, 10]:
        yield 'list_scopes', {'depth': depth}
        yield 'list_scopes', {'path': scope, 'depth': depth, 'compact': True}
    for parent in [scope, scope + ('.child' if fixture else '.dut')]:
        yield 'list_signals', {'path': parent}
        yield 'list_signals', {'path': parent, 'compact': True}
    for pattern in ['*', '*clk*', '?l*', 'no_such_signal']:
        yield 'find_signals', {'pattern': pattern, 'scope': scope}
    for signal in signals:
        yield 'signal_info', {'signal': signal}
        for time in times:
            for radix in ['bin', 'hex', 'oct', 'dec']:
                yield 'get_value_at', {'signals': [signal], 'time': time, 'radix': radix}
            for direction in ['forward', 'backward']:
                for edge in ['any', 'rising', 'falling']:
                    yield 'next_edge', {'signal': signal, 'time': time, 'dir': direction, 'edge': edge}
        for begin, end in ranges:
            for radix in ['bin', 'hex', 'oct', 'dec']:
                yield 'get_value_between', {'signal': signal, 'begin': begin, 'end': end,
                                           'limit': 2, 'radix': radix}
            yield 'vc_count', {'signal': signal, 'begin': begin, 'end': end}
    yield 'get_value_at', {'signals': signals + ['missing.signal'], 'time': times[3]}
    yield 'list_scopes', {'path': 'missing.scope'}
    yield 'get_value_between', {'signal': signals[0], 'begin': 20, 'end': 10}
    yield 'signal_info', {'signal': 'missing.signal'}


def comparable(result):
    result = json.loads(json.dumps(result))
    if isinstance(result.get('data'), dict):
        for key in SOURCE_KEYS:
            result['data'].pop(key, None)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'release/bin/vwave')
    parser.add_argument('--sample', type=Path, default=ROOT / 'test_vwave/tb_top.fsdb')
    parser.add_argument('--fixture', action='store_true')
    parser.add_argument('--sdk', action='append', required=True, help='Verdi 安装，可重复')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/backend_test_results.json')
    args = parser.parse_args()
    binary, sample = str(args.binary.resolve()), str(args.sample.resolve())
    differences = []
    checks = 0
    with tempfile.TemporaryDirectory(prefix='vwave-backends-') as root:
        npi, ffr = Path(root) / 'npi', Path(root) / 'ffr with spaces'
        def cli(*argv, env=None, ok=True):
            p = subprocess.run([binary] + list(map(str, argv)) + ['--json'],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, timeout=70)
            if ok and p.returncode:
                raise AssertionError(p.stderr.decode())
            if not ok:
                assert p.returncode != 0, p.stdout.decode()
                return p
            return json.loads(p.stdout)
        def opened(backend, directory, sdk=None, env=None):
            argv = ['open', sample, '--backend', backend, '--run-dir', directory]
            if sdk:
                argv += ['--verdi-home', sdk]
            return cli(*argv, env=env)
        try:
            opened('npi', npi)
            queries = list(corpus(args.fixture))
            reference = [request(npi, cmd, params) for cmd, params in queries]
            last_pid = None
            for sdk in args.sdk:
                isolated_env = dict(os.environ, SNPSLMD_LICENSE_FILE='1@127.0.0.1', LM_LICENSE_FILE='1@127.0.0.1')
                isolated_env['VERDI_HOME'] = '/missing/environment-verdi'
                pid = opened('ffr', ffr, sdk, isolated_env)['pid']
                if last_pid is not None:
                    assert pid != last_pid, 'Switching SDK must restart worker'
                last_pid = pid
                assert opened('ffr', ffr, sdk, isolated_env)['pid'] == pid
                matching_env = dict(isolated_env, VERDI_HOME=sdk)
                assert cli('open', sample, '--backend', 'ffr', '--run-dir', ffr, env=matching_env)['pid'] == pid
                for (cmd, params), expected in zip(queries, reference):
                    actual = request(ffr, cmd, params)
                    checks += 1
                    if comparable(actual) != comparable(expected):
                        differences.append({'sdk': sdk, 'cmd': cmd, 'params': params,
                                            'npi': expected, 'ffr': actual})
                status = cli('status', '--run-dir', ffr)['data']
                assert status['backend'] == 'ffr'
                assert Path(status['sdk_home']).resolve() == Path(sdk).resolve()
                for prefix in ['reader', 'support']:
                    digest = hashlib.sha256(Path(status[prefix + '_library']).read_bytes()).hexdigest()
                    assert status[prefix + '_sha256'] == digest
                maps = Path('/proc/{}/maps'.format(pid)).read_text()
                assert 'libNPI' not in maps and 'libnpiL1' not in maps
                assert status['reader_library'] in maps and status['support_library'] in maps
                # 查询不解析当前环境中的安装，更不会切换活跃进程。
                changed_env = dict(os.environ, VERDI_HOME='/missing/verdi')
                assert cli('status', '--run-dir', ffr, env=changed_env)['data']['pid'] == pid
                cli('status', '--run-dir', ffr, '--backend', 'npi', ok=False)
                cli('open', sample, '--backend', 'ffr', '--verdi-home', '/missing/verdi', '--run-dir', ffr, ok=False)
                assert cli('status', '--run-dir', ffr)['data']['pid'] == pid
                # 库文件存在，但内容无效：必须预检失败并保留旧会话。
                invalid_home = Path(root) / 'invalid-sdk'
                invalid_lib = invalid_home / 'share/FsdbReader/linux64'
                invalid_lib.mkdir(parents=True, exist_ok=True)
                (invalid_lib / 'libnffr.so').write_bytes(b'not an ELF library')
                (invalid_lib / 'libnsys.so').write_bytes(b'not an ELF library')
                invalid_result = cli('open', sample, '--backend', 'ffr', '--verdi-home', invalid_home,
                                     '--run-dir', ffr, ok=False)
                assert b'dlopen' in invalid_result.stderr
                assert cli('status', '--run-dir', ffr)['data']['pid'] == pid
                if args.fixture:
                    for signal in ['fixture.analog', 'fixture.message', 'fixture.memory']:
                        unsupported = request(ffr, 'get_value_at', {'signals': [signal], 'time': 0})
                        assert unsupported['data']['values'][0]['error_code'] == 'UNSUPPORTED_SIGNAL_TYPE'
                checks += 12 + (3 if args.fixture else 0)
            # 安装路径不变但库内容更新：必须重新选择 reader。
            patched_home = Path(root) / 'patch sdk with spaces'
            patched_lib = patched_home / 'share/FsdbReader/linux64'
            patched_lib.mkdir(parents=True)
            for name in ['libnffr.so', 'libnsys.so']:
                shutil.copy2(str(Path(args.sdk[0]) / 'share/FsdbReader/linux64' / name), str(patched_lib / name))
            patch_pid = opened('ffr', ffr, patched_home)['pid']
            before = cli('status', '--run-dir', ffr)['data']['reader_sha256']
            # 追加 ELF 尾部测试字节，不修改原安装或已映射的代码段。
            with (patched_lib / 'libnffr.so').open('ab') as library:
                library.write(b'vwave-session-fingerprint-test')
            updated_pid = opened('ffr', ffr, patched_home)['pid']
            assert updated_pid != patch_pid
            assert cli('status', '--run-dir', ffr)['data']['reader_sha256'] != before
            assert cli('get', '-s', 'fixture.clk' if args.fixture else 'tb.clk', '-t', '0',
                       '--run-dir', ffr)['data']['values'][0]['value'] == '0'
            # ELF 可以加载，但缺少接口或版本未经验证：明确拒绝，保留活跃会话。
            shutil.copy2(str(Path(args.sdk[0]) / 'share/FsdbReader/linux64/libnsys.so'),
                         str(invalid_lib / 'libnsys.so'))
            for source, message in [
                    ('extern "C" int stub() { return 0; }', b'Missing reader entry'),
                    ('extern "C" const char* version() __asm__("_ZN9ffrObject13ffrGetVersionEv"); '
                     'extern "C" const char* version() { return "unvalidated-reader"; }', b'Unvalidated FsdbReader')]:
                subprocess.run(['g++', '-x', 'c++', '-shared', '-fPIC', '-', '-o', str(invalid_lib / 'libnffr.so')],
                               input=source.encode(), check=True, timeout=30)
                rejected = cli('open', sample, '--backend', 'ffr', '--verdi-home', invalid_home,
                               '--run-dir', ffr, ok=False)
                assert message in rejected.stderr
                assert cli('status', '--run-dir', ffr)['data']['pid'] == updated_pid
            broken_sample = Path(root) / 'broken.fsdb'
            broken_sample.write_bytes(b'not an FSDB file')
            broken_dir = Path(root) / 'broken'
            cli('open', broken_sample, '--backend', 'ffr', '--verdi-home', args.sdk[0],
                '--run-dir', broken_dir, ok=False)
            assert not (broken_dir / 'wave_server.pid').exists()
            assert not (broken_dir / 'session.json').exists()
            helper = subprocess.Popen([sys.executable, '-c', 'import signal; signal.pause()'])
            try:
                stale = Path(root) / 'stale'
                stale.mkdir()
                (stale / 'wave_server.pid').write_text(str(helper.pid))
                cli('open', sample, '--backend', 'ffr', '--verdi-home', args.sdk[0], '--run-dir', stale, ok=False)
                cli('close', '--run-dir', stale, ok=False)
                assert helper.poll() is None, 'Stale PID must never terminate an unrelated process'
            finally:
                helper.terminate()
                helper.wait(timeout=5)
            last_pid = updated_pid
            checks += 8
            # 同文件后端切换与默认 NPI。
            assert opened('npi', ffr)['pid'] != last_pid
            assert cli('status', '--run-dir', ffr)['data']['backend'] == 'npi'
            assert cli('open', sample, '--run-dir', ffr)['pid'] == cli('status', '--run-dir', ffr)['data']['pid']
            cli('open', sample, '--backend', 'bad', '--run-dir', ffr, ok=False)
            cli('open', sample, '--backend', ok=False)
            cli('info', '--verdi-home', args.sdk[0], '--run-dir', ffr, ok=False)
            checks += 6
        finally:
            for directory in [npi, ffr]:
                subprocess.run([binary, 'close', '--run-dir', str(directory), '--json'],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({'checks': checks, 'differences': differences}, indent=2) + '\n')
    print('{} checks; {} differences; {}'.format(checks, len(differences), args.output))
    if differences:
        for item in differences[:5]:
            print(json.dumps(item))
        raise SystemExit(1)


if __name__ == '__main__':
    main()
