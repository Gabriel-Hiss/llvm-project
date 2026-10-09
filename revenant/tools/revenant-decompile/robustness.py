#!/usr/bin/env python3
"""Run all root-level corpus PEs from a private executable copy."""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import time


def pe_files(corpus):
    for directory in sorted(corpus.iterdir()):
        if not directory.is_dir():
            continue
        for path in sorted(directory.iterdir()):
            if not path.is_file():
                continue
            with path.open('rb') as stream:
                if stream.read(2) == b'MZ':
                    yield path


def source_language(target):
    cpp = ('cpp', 'json', 'yaml', 'pugi', 'fmt', 'spdlog', 'imgui',
           'box2d', 'tinyxml')
    return 'cpp' if any(word in target for word in cpp) else 'c'


def classify(log, status):
    if status == 'timeout':
        return 'timeout'
    assertion = re.search(r'Assertion failed: ([^\n]+)', log)
    if assertion:
        return 'assertion: ' + assertion.group(1)
    verifier = re.search(r'(?:IR verifier failed after|LLVM verification failed)[^\n]*', log)
    if verifier:
        return verifier.group(0)
    error = re.search(r'revenant-decompile: ([^\n]+)', log)
    if error:
        return error.group(1)
    return 'success' if status == 0 else 'process exit ' + str(status)


def run_one(exe, pe, kind, output, timeout, fingerprint):
    name = pe.parent.name + '-' + pe.name + '-' + kind
    result_path = output / (name + '.json')
    if result_path.exists():
        old = json.loads(result_path.read_text())
        if old.get('fingerprint') == fingerprint:
            return old
    base = output / name
    for suffix in ('.asm.txt', '.ir-raw.ll', '.ir-clean.ll', '.ir-final.ll',
                   '.source.c', '.source.cpp', '.manifest.txt'):
        Path(str(base) + suffix).unlink(missing_ok=True)
    command = [str(exe), str(pe), '--emit',
               'ir-final' if kind == 'ir-final' else 'source',
               '--lang', source_language(pe.parent.name), '--time', '-o', str(base)]
    started = time.monotonic()
    log_path = output / (name + '.log')
    with log_path.open('wb') as stream:
        try:
            status = subprocess.run(command, stdout=stream, stderr=stream,
                                    timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            status = 'timeout'
    log = log_path.read_text(errors='replace')
    failures = []
    manifest = Path(str(base) + '.manifest.txt')
    if manifest.exists():
        for line in manifest.read_text(errors='replace').splitlines():
            fields = line.split('\t')
            if len(fields) >= 3 and fields[1] == 'failed':
                failures.append({'stage': fields[0], 'message': fields[2]})
    final_ir = Path(str(base) + '.ir-final.ll')
    functions = None
    if final_ir.exists():
        functions = sum(line.startswith('define ') for line in
                        final_ir.read_text(errors='replace').splitlines())
    result = dict(target=pe.parent.name, pe=str(pe), kind=kind, status=status,
                  seconds=round(time.monotonic() - started, 2), log=str(log_path),
                  fingerprint=fingerprint, cluster=classify(log, status),
                  functions_ok=functions, failures=failures)
    result_path.write_text(json.dumps(result, indent=2))
    return result


def report(rows, destination, fingerprint, timeout):
    lines = ['# Pipeline robustness', '',
             'Executable SHA-256: `' + fingerprint + '`.', '',
             'Every immediate corpus child is inspected for PE files by its MZ header.',
             'Each PE runs ir-final and C/C++ source emission without a function filter.',
             'Each invocation has a ' + str(timeout) + ' second timeout. A private executable',
             'copy prevents corpus runs from locking the integration build.', '',
             '`unknown` means the process stopped before final IR was written.',
             'Function counts measure final definitions, not source compilability.',
             'Failure counts are per stage and may count one function more than once.', '',
             '| target / PE | emission | exit | functions ok | failed by stage | seconds |',
             '|---|---|---|---:|---|---:|']
    clusters = {}
    for row in sorted(rows, key=lambda r: (r['target'], r['pe'], r['kind'])):
        counts = {}
        for failure in row['failures']:
            stage = failure['stage']
            counts[stage] = counts.get(stage, 0) + 1
        failed = ', '.join(stage + '=' + str(count) for stage, count in sorted(counts.items()))
        if not failed:
            failed = '0' if row['status'] == 0 else 'unknown'
        ok = row['functions_ok'] if row['functions_ok'] is not None else 'unknown'
        lines.append('| ' + row['target'] + ' / ' + Path(row['pe']).name +
                     ' | ' + row['kind'] + ' | ' + str(row['status']) + ' | ' +
                     str(ok) + ' | ' + failed + ' | ' + str(row['seconds']) + ' |')
        if row['cluster'] != 'success':
            clusters.setdefault(row['cluster'], []).append(row['target'] + '/' + row['kind'])
    lines += ['', '## Process failure clusters', '']
    if not clusters:
        lines.append('No crashes, assertions, binary-level verifier errors or timeouts.')
    for message, targets in sorted(clusters.items()):
        lines += ['### ' + message.replace('|', '\\|'), '', ', '.join(targets), '']
    destination.write_text('\n'.join(lines) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--corpus', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--workers', type=int, default=3)
    args = parser.parse_args()
    if args.timeout <= 0 or args.workers <= 0:
        parser.error('timeout and workers must be positive')
    args.output.mkdir(parents=True, exist_ok=True)
    fingerprint = hashlib.sha256(args.exe.read_bytes()).hexdigest()
    private = args.output / ('bin-' + fingerprint[:16])
    private.mkdir(exist_ok=True)
    copied = private / args.exe.name
    if not copied.exists():
        shutil.copy2(args.exe, copied)
        for dll in args.exe.parent.glob('*.dll'):
            shutil.copy2(dll, private / dll.name)
    jobs = [(pe, kind) for pe in pe_files(args.corpus)
            for kind in ('ir-final', source_language(pe.parent.name))]
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(run_one, copied, pe, kind, args.output,
                               args.timeout, fingerprint) for pe, kind in jobs]
        for future in concurrent.futures.as_completed(futures):
            row = future.result()
            rows.append(row)
            print(row['target'], row['kind'], row['status'], flush=True)
            report(rows, args.report, fingerprint, args.timeout)
    (args.output / 'summary.json').write_text(json.dumps(rows, indent=2))
    failed = any(row['status'] != 0 for row in rows)
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
