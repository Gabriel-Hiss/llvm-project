"""Measure recovered call boundaries against read-only pre/post corpus pairs.

Absent direct calls are candidates, not proof of inlining: without optimization
remarks or inline debug frames, DCE and constant folding are indistinguishable.
The CSV keeps this distinction rather than claiming an exact inline denominator.
"""
import argparse
import collections
import csv
import json
import pathlib
import re
import subprocess
import tempfile

NAME = r'@("(?:[^"\\]|\\.)*"|[-a-zA-Z$._0-9]+)'
DEFINE = re.compile(r'^define\b.*?' + NAME + r'\(', re.M)
CALL = re.compile(r'\b(?:call|invoke)\b[^\n]*?' + NAME + r'\(')


def functions(text):
    result = {}
    for match in DEFINE.finditer(text):
        end = text.find('\n}', match.end())
        if end < 0:
            raise ValueError('unterminated definition ' + match[1])
        result[match[1]] = text[match.start():end + 2]
    return result


def calls(body):
    return collections.Counter(m[1] for m in CALL.finditer(body)
                               if not m[1].startswith('llvm.'))


def group(name):
    if re.search(r'clang-o[23]lto$', name):
        return 'clang-cl LTO'
    if re.search(r'clang-o[23]$', name):
        return 'clang-cl ' + name[-2:].upper()
    if re.fullmatch(r'rust-o[23]', name):
        return 'rustc ' + name[-2:].upper()
    return None


def pairs(root):
    for directory in sorted(root.iterdir()):
        category = group(directory.name)
        index = directory / 'artifacts' / 'artifacts.json'
        if not category or not index.is_file():
            continue
        files = json.loads(index.read_text(encoding='utf-8-sig'))['files']
        by_tu = collections.defaultdict(dict)
        for item in files:
            if item['kind'] in ('ir-pre', 'ir-post'):
                by_tu[item['tu']][item['kind']] = index.parent / item['file']
        for tu, stages in sorted(by_tu.items()):
            if set(stages) == {'ir-pre', 'ir-post'}:
                yield directory.name, category, tu, stages


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--corpus', type=pathlib.Path, required=True)
    parser.add_argument('--opt', type=pathlib.Path, required=True)
    parser.add_argument('--plugin', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    rows, modules, errors = [], [], []
    totals = collections.defaultdict(collections.Counter)
    with tempfile.TemporaryDirectory(prefix='revenant-uninline-') as temporary:
        output = pathlib.Path(temporary) / 'recovered.ll'
        for target, category, tu, stages in pairs(args.corpus):
            pre = functions(stages['ir-pre'].read_text(encoding='utf-8'))
            post = functions(stages['ir-post'].read_text(encoding='utf-8'))
            command = [str(args.opt), '-load-pass-plugin=' + str(args.plugin),
                       '-passes=revenant-uninline,verify', '-S',
                       str(stages['ir-post']), '-o', str(output)]
            run = subprocess.run(command, capture_output=True, text=True,
                                 timeout=300)
            if run.returncode:
                errors.append({'target': target, 'tu': tu, 'error': run.stderr})
                continue
            recovered = functions(output.read_text(encoding='utf-8'))
            count = collections.Counter()
            for caller, body in pre.items():
                if caller not in post:
                    continue  # Deleted callers do not contain observable sites.
                original = calls(body)
                optimized = calls(post[caller])
                restored = calls(recovered[caller])
                for callee, before in sorted(original.items()):
                    if callee not in pre or callee == caller:
                        continue
                    header = pre[callee].split('\n', 1)[0]
                    attr = re.search(r'#(\d+)', header)
                    attrs = ''
                    if attr:
                        text = stages['ir-pre'].read_text(encoding='utf-8')
                        found = re.search(r'^attributes #' + attr[1] +
                                          r' = (.*)$', text, re.M)
                        attrs = found[1] if found else ''
                    if 'noinline' in header + attrs:
                        continue
                    missing = max(0, before - optimized[callee])
                    recovered_count = max(0, restored[callee] - optimized[callee])
                    # Repeated identical targets cannot be assigned to original
                    # sites without debug info. Keep a stable ordinal and flag it.
                    for ordinal in range(missing):
                        status = 'full' if ordinal < recovered_count else 'none'
                        rows.append({'target': target, 'tu': tu, 'caller': caller,
                                     'callee': callee, 'ordinal': ordinal,
                                     'status': status,
                                     'evidence': 'removed-direct-call-candidate',
                                     'ambiguous_site': before > 1,
                                     'callee_retained': callee in post})
                        count[status] += 1
            modules.append({'target': target, 'tu': tu, **count})
            totals[category].update(count)
            totals[category]['modules'] += 1
            print(target, tu, dict(count), flush=True)
    args.output.mkdir(parents=True, exist_ok=True)
    with (args.output / 'sites.csv').open('w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=['target', 'tu', 'caller', 'callee',
                               'ordinal', 'status', 'evidence', 'ambiguous_site',
                               'callee_retained'])
        writer.writeheader()
        writer.writerows(rows)
    summary = {'classification': 'full = restored direct-call boundary; none = '
               'not restored; partial = not observed because a failed match '
               'does not mutate IR. This is not full pre-optimization IR equality.',
               'denominator': 'removed direct-call candidates, excluding '
               'noinline callees and deleted callers; inlining is not proven',
               'groups': dict(totals), 'modules': modules, 'errors': errors}
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary['groups'], indent=2))
    if errors:
        raise SystemExit(str(len(errors)) + ' module(s) failed; see summary.json')


if __name__ == '__main__':
    main()
