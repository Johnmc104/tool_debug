#!/usr/bin/env python3
"""采集 reader ABI 要求与 NPI 内置 reader 调用证据，兼容 Python 3.6。"""
import hashlib
import json
from pathlib import Path
import re
import subprocess


def run(args):
    result = subprocess.run([str(x) for x in args], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, universal_newlines=True,
                            errors='replace', timeout=60)
    if result.returncode:
        raise RuntimeError(result.stderr)
    return result.stdout


def version_key(value):
    return tuple(int(x) for x in value.split('_', 1)[1].split('.'))


def main():
    root = Path(__file__).resolve().parents[1]
    out = root / 'evidence/compatibility'
    out.mkdir(parents=True, exist_ok=True)
    versions = ['T-2022.06-SP2', 'Y-2026.03-SP2']
    rows = []
    for version in versions:
        sdk = Path('/opt/Synopsys/verdi') / version / 'share/FsdbReader'
        for library in ['libnffr.so', 'libnsys.so']:
            path = sdk / 'linux64' / library
            prefix = version + '.' + library
            dynamic = run(['readelf', '-d', path])
            requirements = run(['readelf', '--version-info', path])
            symbols = run(['readelf', '--dyn-syms', '--wide', path])
            (out / (prefix + '.dynamic.txt')).write_text(dynamic)
            (out / (prefix + '.version.txt')).write_text(requirements)
            undefined = [line.strip() for line in symbols.splitlines() if ' UND ' in line]
            (out / (prefix + '.undefined.txt')).write_text('\n'.join(undefined) + '\n')
            required = {}
            for namespace in ['GLIBC', 'GLIBCXX', 'CXXABI']:
                found = sorted(set(re.findall(r'\b' + namespace + r'_\d+(?:\.\d+)+',
                                             '\n'.join(undefined))), key=version_key)
                required[namespace] = found
            newer = [line for line in undefined for tag in re.findall(r'GLIBC_\d+(?:\.\d+)+', line)
                     if version_key(tag) > (2, 17)]
            sha = hashlib.sha256()
            with path.open('rb') as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    sha.update(block)
            rows.append({'version': version, 'library': library, 'path': str(path),
                         'size': path.stat().st_size, 'sha256': sha.hexdigest(),
                         'required_versions': required,
                         'max_glibc': required['GLIBC'][-1],
                         'incompatible_with_glibc_2_17': sorted(set(newer)),
                         'needed': re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic)})

    npi = Path('/opt/Synopsys/verdi/Y-2026.03-SP2/share/NPI/lib/linux64/libNPI.so')
    (out / 'npi.dynamic.txt').write_text(run(['readelf', '-d', npi]))
    symbols = run(['nm', '-S', '--defined-only', npi])
    table = {}
    for line in symbols.splitlines():
        parts = line.split()
        if len(parts) == 4:
            table[parts[3]] = {'address': int(parts[0], 16), 'size': int(parts[1], 16),
                               'symbol_type': parts[2]}
    targets = {
        'open_file': '_ZN14fda_file_mgr_t9open_fileEPKccc',
        'open_file_with_ffr': '_ZN14fda_file_mgr_t18open_file_with_ffrEPccc',
        'ffr_open3': '_ZN9ffrObject8ffrOpen3EPc',
        'ffr_open_impl': '_ZN9ffrObject12ffrOpen_implEPcPFc14fsdbTreeCBTypePvS2_ES2_13ffrOpenOptionP15ffrCmprsFileArgP11IJoinedFile',
        'reader_fixed_header': '_Z12ReadFixedHdriiP12fsdbFixedHdr',
        'reader_header': '_ZN7ffrDisk10ReadHeaderEv',
    }
    selected = {}
    for label, symbol in targets.items():
        entry = table[symbol]
        assembly = run(['objdump', '-d', '-C', '--start-address=' + str(entry['address']),
                        '--stop-address=' + str(entry['address'] + entry['size']), npi])
        (out / ('npi.' + label + '.asm.txt')).write_text(assembly)
        selected[label] = dict(entry, symbol=symbol)
    result = {'centos7_glibc': '2.17', 'libraries': rows,
              'npi_internal_functions': selected}
    (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
    for row in rows:
        print('{} {}: {}, incompatible symbols={}'.format(row['version'], row['library'],
              row['max_glibc'], len(row['incompatible_with_glibc_2_17'])))
    print('NPI 内置 reader 调用证据已保存：' + str(out))


if __name__ == '__main__':
    main()
