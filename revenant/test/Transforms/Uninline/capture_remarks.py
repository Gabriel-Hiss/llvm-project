"""Replay corpus compile-only IR recipes in scratch, never link or modify corpus."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess

from measure import pairs


def digest(path):
    text = path.read_text(encoding='utf-8')
    text = re.sub(r'^; ModuleID =.*\n', '', text)
    return hashlib.sha256(text.encode()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--corpus', type=pathlib.Path, required=True)
    parser.add_argument('--scratch', type=pathlib.Path, required=True)
    parser.add_argument('--rust-source', type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    results = []
    for target, category, tu, stages in pairs(args.corpus):
        scratch = (args.scratch / target / stages['ir-post'].stem).resolve()
        scratch.mkdir(parents=True, exist_ok=True)
        log = scratch / 'remarks.log'
        record = scratch / 'inline.yaml'
        if category.startswith('clang'):
            recipe = args.corpus / target / 'artifacts.cmd'
            if not recipe.is_file():
                results.append({'target': target, 'tu': tu,
                                'error': 'missing artifacts.cmd'})
                continue
            commands = recipe.read_text(encoding='utf-8').splitlines()
            # Select only /c -emit-llvm recipes for this TU, never move/link.
            source_leaf = pathlib.PurePosixPath(tu.replace('\\', '/')).name
            commands = [c for c in commands if '/clang:-emit-llvm' in c and
                        source_leaf.lower() in c.lower()]
            if len(commands) != 1:
                results.append({'target': target, 'tu': tu,
                                'error': 'ambiguous compile recipe'})
                continue
            vcvars = next(c[5:] for c in recipe.read_text().splitlines()
                          if c.lower().startswith('call '))
            command = commands[0] + ' /clang:-fsave-optimization-record ' + \
                '/clang:-Rpass=inline /clang:-foptimization-record-file=' + \
                '"' + str(record) + '"'
            script = scratch / 'capture.cmd'
            script.write_text('@echo off\ncall ' + vcvars + '\n' + command + '\n')
            run = subprocess.run(['cmd.exe', '/d', '/c', str(script)],
                                 cwd=scratch, capture_output=True, text=True,
                                 timeout=600)
            generated = scratch / (pathlib.Path(source_leaf).stem + '.ll')
        else:
            generated = scratch / 'corpus_rust.ll'
            command = ['rustc', str(args.rust_source), '--edition=2021',
                       '--crate-name', 'corpus_rust', '--target',
                       'x86_64-pc-windows-msvc', '-C', 'opt-level=3',
                       '-C', 'debuginfo=2', '-C', 'remark=inline',
                       '-C', 'llvm-args=-pass-remarks-output=' + str(record),
                       '--emit=llvm-ir=' + str(generated)]
            run = subprocess.run(command, cwd=scratch, capture_output=True,
                                 text=True, timeout=600)
        log.write_text(run.stdout + run.stderr, encoding='utf-8')
        result = {'target': target, 'tu': tu, 'exit_code': run.returncode,
                  'record': str(record), 'log': str(log)}
        if generated.is_file():
            result['identical_ir_post'] = digest(generated) == digest(stages['ir-post'])
            result['generated_ir'] = str(generated)
        if not record.is_file():
            result['error'] = 'compiler did not produce optimization record'
        results.append(result)
        print(json.dumps(result), flush=True)
    (args.scratch / 'index.json').write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
