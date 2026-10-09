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

from remarks import symbol, verified_index

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
    parser.add_argument('--remarks', type=pathlib.Path,
                        help='capture_remarks.py index with identical-IR records')
    args = parser.parse_args()
    rows, modules, errors = [], [], []
    totals = collections.defaultdict(collections.Counter)
    records, rejected = verified_index(args.remarks, args.opt, args.corpus) \
        if args.remarks else ({}, [])
    with tempfile.TemporaryDirectory(prefix='revenant-uninline-') as temporary:
        output = pathlib.Path(temporary) / 'recovered.ll'
        for target, category, tu, stages in pairs(args.corpus):
            if args.remarks and (target, tu) not in records:
                errors.append({'target': target, 'tu': tu,
                               'error': 'no identical-IR inline record'})
                continue
            text = stages['ir-pre'].read_text(encoding='utf-8')
            pre = functions(text)
            post = functions(stages['ir-post'].read_text(encoding='utf-8'))
            command = [str(args.opt), '-load-pass-plugin=' + str(args.plugin),
                       '-passes=revenant-uninline,verify', '-S',
                       str(stages['ir-post']), '-o', str(output)]
            try:
                run = subprocess.run(command, capture_output=True, text=True,
                                     timeout=300)
            except subprocess.TimeoutExpired:
                errors.append({'target': target, 'tu': tu, 'error': 'opt timeout'})
                continue
            if run.returncode:
                errors.append({'target': target, 'tu': tu, 'error': run.stderr})
                continue
            recovered = functions(output.read_text(encoding='utf-8'))
            count = collections.Counter(full=0, partial=0, none=0, unresolved=0)
            if args.remarks:
                events = records[target, tu]
                post_names = {symbol(k): k for k in post}
                optimized_calls = {symbol(k): {symbol(c): n for c, n in
                                   calls(body).items()} for k, body in post.items()}
                restored_calls = {symbol(k): {symbol(c): n for c, n in
                                  calls(body).items()} for k, body in recovered.items()}
                event_counts = collections.Counter((e['caller'], e['callee'])
                                                   for e in events)
                for ordinal, event in enumerate(events):
                    caller, callee = event['caller'], event['callee']
                    delta = restored_calls.get(caller, {}).get(callee, 0)
                    delta -= optimized_calls.get(caller, {}).get(callee, 0)
                    pair_count = event_counts[caller, callee]
                    status = 'none' if delta <= 0 else (
                        'full' if delta >= pair_count else 'unresolved')
                    row = {'target': target, 'tu': tu, 'ordinal': ordinal,
                           'status': status, 'evidence': 'verified-inline-remark',
                           'ambiguous_site': status == 'unresolved',
                           'callee_retained': callee in post_names, **event}
                    rows.append(row)
                    count[status] += 1
            else:
                attributes = dict(re.findall(r'^attributes #(\\d+) = (.*)$',
                                             text, re.M))
                for caller, body in pre.items():
                    if caller not in post:
                        continue
                    original = calls(body)
                    optimized = calls(post[caller])
                    restored = calls(recovered[caller])
                    for callee, before in sorted(original.items()):
                        if callee not in pre or callee == caller:
                            continue
                        header = pre[callee].split('\\n', 1)[0]
                        attr = re.search(r'#(\\d+)', header)
                        attrs = attributes.get(attr[1], '') if attr else ''
                        if 'noinline' in header + attrs:
                            continue
                        missing = max(0, before - optimized[callee])
                        delta = max(0, restored[callee] - optimized[callee])
                        for ordinal in range(missing):
                            status = 'none' if delta == 0 else (
                                'full' if delta >= missing else 'unresolved')
                            rows.append({'target': target, 'tu': tu,
                                         'caller': caller, 'callee': callee,
                                         'ordinal': ordinal, 'status': status,
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
                               'callee_retained', 'file', 'line', 'column'])
        writer.writeheader()
        writer.writerows(rows)
    summary = {'classification': 'full = restored direct-call boundary; none = '
               'not restored; partial = 0 because rewrites are atomic. '
               'Unresolved means indistinguishable repeated source sites with '
               'only some call boundaries restored, not partial recovery.',
               'denominator': 'verified compiler inline records' if args.remarks
               else 'removed direct-call candidates; inlining is not proven',
               'groups': dict(totals), 'modules': modules, 'errors': errors,
               'rejected_records': rejected}
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary['groups'], indent=2))
    if errors:
        raise SystemExit(str(len(errors)) + ' module(s) failed; see summary.json')


if __name__ == '__main__':
    main()
