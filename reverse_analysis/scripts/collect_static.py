#!/usr/bin/env python3
"""记录 FSDB 样本和 reader 库的静态证据；不执行 NPI 或读取许可证配置。"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[2]
    parser.add_argument('--verdi-home', default='/opt/Synopsys/verdi/Y-2026.03-SP2')
    parser.add_argument('--sample', type=Path, default=root / 'test_vwave/tb_top.fsdb')
    parser.add_argument('--output', type=Path, default=root / 'reverse_analysis/evidence/static')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    commands = []

    def run(argv):
        result = subprocess.run([str(x) for x in argv], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, universal_newlines=True,
                                errors='replace', timeout=60)
        commands.append({'argv': [str(x) for x in argv], 'returncode': result.returncode})
        if result.returncode:
            command = ' '.join(shlex.quote(x) for x in commands[-1]['argv'])
            raise RuntimeError(f'{command}: {result.stderr}')
        return result.stdout

    def digest(path):
        hasher = hashlib.sha256()
        with path.open('rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                hasher.update(block)
        return hasher.hexdigest()

    data = args.sample.read_bytes()
    texts = [{'offset': match.start(), 'offset_hex': hex(match.start()),
              'text': match.group().decode('ascii')}
             for match in re.finditer(rb'[\x20-\x7e]{4,}', data)]
    entropy = []
    for offset in range(0, len(data), 1024):
        chunk = data[offset:offset + 1024]
        value = -sum((n / len(chunk)) * math.log2(n / len(chunk))
                     for n in Counter(chunk).values())
        entropy.append({'offset': offset, 'length': len(chunk), 'bits_per_byte': round(value, 4)})
    sample = {'path': str(args.sample.resolve()), 'size': len(data),
              'sha256': hashlib.sha256(data).hexdigest(),
              'head_32_hex': data[:32].hex(), 'ascii_runs': texts,
              'block_entropy': entropy}
    (out / 'sample.json').write_text(json.dumps(sample, indent=2, ensure_ascii=False) + '\n')
    (out / 'sample_head.hex.txt').write_text(run(['xxd', '-l', '512', args.sample]))

    home = Path(args.verdi_home)
    libraries = {
        'npi': home / 'share/NPI/lib/linux64/libNPI.so',
        'reader': home / 'share/FsdbReader/linux64/libnffr.so',
        'reader_system': home / 'share/FsdbReader/linux64/libnsys.so',
    }
    targets = {
        'npi': ['_Z13npi_fsdb_openPKc', '_Z20npi_fsdb_direct_openPKc',
                '_Z20npi_fsdb_direct_openPKccb'],
        'reader': ['_ZN7ffrDisk10ReadHeaderEv', '_ZN7ffrDisk16ReadVCFsdbHeaderEv',
                   '_ZN7ffrDisk13DecompressBlkEP10ffrScanBuf',
                   '_ZN7ffrDisk16ReadScopeVarTreeEt',
                   '_Z12ReadFixedHdriiP12fsdbFixedHdr',
                   '_Z12ReadCloseHdriihP12fsdbCloseHdrh',
                   '_Z12ReadFlushHdriihP12fsdbFlushHdrh'],
        'reader_system': [],
    }
    metadata = []
    for name, path in libraries.items():
        elf = run(['readelf', '-h', '-d', '-n', path])
        (out / f'{name}.elf.txt').write_text(elf)
        dynamic = run(['nm', '-D', '-S', '--defined-only', path])
        symbols = run(['nm', '-S', '--defined-only', path])
        demangled = run(['nm', '-C', '--defined-only', path])
        selected = [line for line in demangled.splitlines()
                    if re.search(r'npi_fsdb_(open|direct_open|sig_value|iter_|vct_)|'
                                 r'ffrDisk::.*(Header|Scope|Tree|Decompress|ReadVC|Read.*Blk)|'
                                 r'ffrObject::.*(Open|LoadSignals)|'
                                 r'ffrVCIterOne::.*(Goto|XTag|GetVC)', line)]
        (out / f'{name}.symbols.txt').write_text('\n'.join(selected) + '\n')
        sizes = {}
        for line in symbols.splitlines():
            parts = line.split()
            if len(parts) == 4:
                sizes[parts[3]] = (int(parts[0], 16), int(parts[1], 16))
        for symbol in targets[name]:
            if symbol not in sizes:
                continue
            address, size = sizes[symbol]
            assembly = run(['objdump', '-d', '-C', f'--start-address={address}',
                            f'--stop-address={address + size}', path])
            (out / f'{name}.{symbol}.asm.txt').write_text(assembly)
        metadata.append({'name': name, 'path': str(path), 'size': path.stat().st_size,
                         'sha256': digest(path), 'selected_symbols': len(selected),
                         'dynamic_defined_symbols': len(dynamic.splitlines()),
                         'all_defined_symbols': len(symbols.splitlines())})
    apis = sorted(set(re.findall(r'\bnpi_fsdb_\w+',
                                (root / 'src_vwave/server/handlers.h').read_text() +
                                (root / 'src_vwave/server/server_core.h').read_text())))
    (out / 'vwave_npi_api_surface.json').write_text(json.dumps(apis, indent=2) + '\n')
    manifest = {'collected_utc': datetime.now(timezone.utc).isoformat(),
                'libraries': metadata, 'sample_sha256': sample['sha256'],
                'commands': commands}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'output': str(out), 'sample_size': len(data),
                      'api_count': len(apis), 'libraries': metadata}, indent=2))


if __name__ == '__main__':
    main()
