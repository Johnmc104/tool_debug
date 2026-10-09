#!/usr/bin/env python3
"""对照 NPI/FFR 功能缺口，并采集相关 FDA 调用链；兼容 Python 3.6。"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'test_vwave'))
from test_backends import comparable, request


def capture_calls(library, output):
    entries = []
    listing = subprocess.check_output(['nm', '-S', '--defined-only', str(library)],
                                      universal_newlines=True)
    for line in listing.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[2] in ('t', 'T'):
            entries.append((parts[3], int(parts[0], 16), int(parts[1], 16)))
    names = subprocess.check_output(['c++filt'], input='\n'.join(e[0] for e in entries),
                                    universal_newlines=True).splitlines()
    prefixes = [
        'npi_fsdb_sig_by_name(', 'npi_fsdb_goto_time(', 'npi_fsdb_vct_value(',
        'fda_file_t::get_sig_by_name(', 'fda_get_sig_by_name(',
        'fda_scope_t::find_sig(', 'fda_vct_value_skip_check(',
        'fda_vch_leaf_t::goto_time(', 'fda_vch_partial_t::goto_time(',
        'fda_vch_partial_t::goto_next_vc(', 'fda_vch_composite_t::goto_time(',
        'fda_val_real_t::get_value_str_bin(', 'fda_double_to_char(',
        'fda_val_string_t::get_value_string(',
        'fda_read_scope_tree(', 'fda_create_packed_struct_field(',
        'fda_create_unpacked_struct_field(',
    ]
    selected = []
    for (symbol, address, size), name in zip(entries, names):
        if not any(name.startswith(prefix) for prefix in prefixes):
            continue
        label = name.split('(')[0].replace('::', '.')
        assembly = subprocess.check_output([
            'objdump', '-d', '-C', '--start-address=' + str(address),
            '--stop-address=' + str(address + size), str(library)], universal_newlines=True)
        (output / (label + '.asm.txt')).write_text(assembly)
        selected.append({'name': name, 'symbol': symbol, 'address': hex(address), 'size': size,
                         'calls': [line.strip() for line in assembly.splitlines() if 'call' in line]})
    return {'library': str(library), 'sha256': hashlib.sha256(library.read_bytes()).hexdigest(),
            'functions': selected}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--support-fixture', action='store_true')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/ffr_support_analysis/queries.json')
    parser.add_argument('--assembly-output', type=Path, default=ROOT / 'reverse_analysis/evidence/ffr_support')
    args = parser.parse_args()
    if args.support_fixture:
        scope = 'support_fixture'
        signals = ['packet', 'packet.payload', 'packet.tag', 'packet[7:0]',
                   'pair_value', 'pair_value.a', 'matrix', 'matrix[1]', 'matrix[1][2]',
                   'matrix[1][2][3]', 'two_state', 'two_state[0]', 'delayed', 'analog', 'text_value']
    else:
        scope = 'fixture'
        signals = ['data[3]', 'data[7:4]', 'ascending[0]', 'memory', 'memory[1]',
                   'memory[1][3]', 'analog', 'message']
    queries = [('list_signals', {'path': scope}), ('list_scopes', {'path': scope, 'depth': 3})]
    for signal in signals:
        path = scope + '.' + signal
        queries.append(('signal_info', {'signal': path}))
        for time in (0, 1000, 3000, 4000, 6000):
            for radix in ('bin', 'hex', 'oct', 'dec'):
                queries.append(('get_value_at', {'signals': [path], 'time': time, 'radix': radix}))
        queries.append(('get_value_between', {'signal': path, 'begin': 0, 'end': 6000}))
        queries.append(('vc_count', {'signal': path, 'begin': 0, 'end': 6000}))
        for direction in ('forward', 'backward'):
            queries.append(('next_edge', {'signal': path, 'time': 1000, 'dir': direction}))
    records = []
    binary = str(ROOT / 'release/bin/vwave')
    with tempfile.TemporaryDirectory(prefix='vwave-support-') as temporary:
        directories = {b: Path(temporary) / b for b in ('npi', 'ffr')}
        try:
            for backend, directory in directories.items():
                command = [binary, 'open', str(args.sample.resolve()), '--backend', backend,
                           '--run-dir', str(directory), '--json']
                if backend == 'ffr':
                    command += ['--verdi-home', str(args.sdk.resolve())]
                opened = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=70)
                if opened.returncode:
                    raise RuntimeError(opened.stderr.decode(errors='replace'))
            for cmd, params in queries:
                row = {'cmd': cmd, 'params': params}
                row.update({b: request(d, cmd, params) for b, d in directories.items()})
                row['equal'] = comparable(row['npi']) == comparable(row['ffr'])
                records.append(row)
        finally:
            for directory in directories.values():
                subprocess.run([binary, 'close', '--run-dir', str(directory), '--json'],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
    result = {'sample': str(args.sample.resolve()), 'sdk': str(args.sdk.resolve()),
              'npi_reference': 'worker compiled with its own SDK; see worker --describe',
              'checks': len(records), 'different': sum(not r['equal'] for r in records), 'records': records}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    args.assembly_output.mkdir(parents=True, exist_ok=True)
    library = args.sdk / 'share/NPI/lib/linux64/libNPI.so'
    calls = capture_calls(library, args.assembly_output)
    (args.assembly_output / 'npi_calls.json').write_text(json.dumps(calls, indent=2) + '\n')
    print('{} checks; {} different; {}'.format(result['checks'], result['different'], args.output))


if __name__ == '__main__':
    main()
