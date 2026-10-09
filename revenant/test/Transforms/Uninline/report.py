"""Compare two verified per-site measurements without rerunning a pass."""
import argparse
import collections
import csv
import json
import pathlib


def load(directory):
    summary = json.loads((directory / 'summary.json').read_text(encoding='utf-8'))
    with (directory / 'sites.csv').open(encoding='utf-8', newline='') as f:
        rows = list(csv.DictReader(f))
    keyed = {}
    for row in rows:
        key = tuple(row[k] for k in ('target', 'tu', 'caller', 'callee', 'ordinal'))
        if key in keyed:
            raise ValueError('duplicate site: ' + repr(key))
        keyed[key] = row
    return summary, keyed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=pathlib.Path, required=True)
    parser.add_argument('--after', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    before, before_rows = load(args.before)
    after, after_rows = load(args.after)
    if set(before_rows) != set(after_rows):
        raise ValueError('before/after site denominators differ')
    if before['denominator'] != 'verified compiler inline records' or \
            after['denominator'] != before['denominator']:
        raise ValueError('both measurements must use verified inline records')
    table = ['| Compiler | Modules | Inline events | Before full/partial/none | '
             'After full/partial/none | Unresolved before/after |',
             '|---|---:|---:|---|---|---|']
    for category in sorted(set(before['groups']) | set(after['groups'])):
        old = before['groups'][category]
        new = after['groups'][category]
        statuses = ('full', 'partial', 'none')
        old_counts = '/'.join(str(old.get(k, 0)) for k in statuses)
        new_counts = '/'.join(str(new.get(k, 0)) for k in statuses)
        events = sum(new.get(k, 0) for k in statuses + ('unresolved',))
        table.append('| ' + category + ' | ' + str(new['modules']) + ' | ' +
                     str(events) + ' | ' + old_counts + ' | ' + new_counts +
                     ' | ' + str(old.get('unresolved', 0)) + '/' +
                     str(new.get('unresolved', 0)) + ' |')
    changed = []
    transitions = collections.Counter()
    for key, old in before_rows.items():
        new = after_rows[key]
        transitions[old['status'] + ' -> ' + new['status']] += 1
        if old['status'] != new['status']:
            changed.append({'target': new['target'], 'caller': new['caller'],
                            'callee': new['callee'], 'file': new.get('file', ''),
                            'line': new.get('line', ''),
                            'before': old['status'], 'after': new['status']})
    report = {'groups_before': before['groups'], 'groups_after': after['groups'],
              'table': '\n'.join(table), 'transitions': dict(transitions),
              'changed_sites': changed, 'errors_before': before['errors'],
              'errors_after': after['errors'],
              'rejected_records': after['rejected_records']}
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(report['table'])
    print('Transitions:', dict(transitions))
    for example in changed[:20]:
        print(json.dumps(example))
    if before['errors'] or after['errors']:
        raise SystemExit('Some corpus pairs have no verified measurement; see report')


if __name__ == '__main__':
    main()
