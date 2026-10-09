"""Small reader for LLVM YAML optimization records; no YAML dependency needed."""
import hashlib
import json
import pathlib
import re
import subprocess


def scalar(text):
    text = text.strip()
    if text.startswith("'") and text.endswith("'"):
        return text[1:-1].replace("''", "'")
    if text.startswith('"') and text.endswith('"'):
        return json.loads(text)
    return text


def symbol(name):
    if name.startswith('"') and name.endswith('"'):
        name = name[1:-1]
        name = re.sub(r'\\([0-9A-Fa-f]{2})',
                      lambda m: chr(int(m[1], 16)), name)
    return name


def inline_records(path):
    text = pathlib.Path(path).read_text(encoding='utf-8')
    if pathlib.Path(path).suffix == '.txt':
        pattern = re.compile(r"^note: (.*):([0-9]+):([0-9]+) inline "
                             r"\(success\): '([^']+)' inlined into '([^']+)'",
                             re.M)
        return [{'file': m[1], 'line': int(m[2]), 'column': int(m[3]),
                 'callee': m[4], 'caller': m[5]} for m in pattern.finditer(text)]
    result = []
    for document in text.split('--- !'):
        if not re.search(r'^Name:\s+Inlined\s*$', document, re.M):
            continue
        fields = {}
        for key in ('Callee', 'Caller'):
            match = re.search(r'^  - ' + key + r':\s+(.+)$', document, re.M)
            if match:
                fields[key.lower()] = scalar(match[1])
        location = re.search(r'^DebugLoc:\s+\{(.*?)\}', document, re.M | re.S)
        if location:
            file = re.search(r'File:\s+(.*?),\s*Line:', location[1], re.S)
            line = re.search(r'Line:\s+(\d+)', location[1])
            column = re.search(r'Column:\s+(\d+)', location[1])
            fields['file'] = scalar(file[1]) if file else ''
            fields['line'] = int(line[1]) if line else 0
            fields['column'] = int(column[1]) if column else 0
        if 'callee' in fields and 'caller' in fields:
            result.append(fields)
    return result


def mask_ids(line):
    return re.sub(r'(!|#)\d+', r'\1?', line)


def body_lines(text):
    return [mask_ids(line) for line in text.splitlines()
            if not line.startswith(('!', 'attributes #', ';'))]


def meta_nodes(text):
    return sorted(mask_ids(line.split(' = ', 1)[1])
                  if ' = ' in line else mask_ids(line)
                  for line in text.splitlines()
                  if line.startswith(('!', 'attributes #')))


def semantic_equal(before, after):
    """Bodies equal after strip-debug, up to numbering.

    Line-table debug info changes which distinct scope nodes each compile
    emits, so metadata contents legitimately differ between two identical
    compiles of the same TU. What must agree exactly is every body line
    (with numbering masked): opcodes, operands, types, attributes and the
    metadata attachments on instructions.
    """
    return body_lines(before) == body_lines(after)


def normalized_ir(path, opt):
    run = subprocess.run([str(opt), '-strip-debug', '-passes=verify',
                          '-S', str(path)],
                         capture_output=True, text=True, timeout=120)
    if run.returncode:
        raise ValueError(run.stderr)
    return re.sub(r'^; ModuleID =.*\n', '', run.stdout)

def verified_index(path, opt, corpus):
    result, rejected, validation = {}, [], []
    for item in json.loads(pathlib.Path(path).read_text(encoding='utf-8')):
        item = dict(item)
        if item.get('exit_code') != 0 or item.get('error'):
            rejected.append(item)
            continue
        same = item.get('identical_ir_post') or item.get('identical_after_strip_debug')
        if not same:
            index = corpus / item['target'] / 'artifacts' / 'artifacts.json'
            files = json.loads(index.read_text(encoding='utf-8-sig'))['files']
            original = next(index.parent / f['file'] for f in files
                            if f['kind'] == 'ir-post' and f['tu'] == item['tu'])
            before = normalized_ir(original, opt)
            replay = normalized_ir(item['generated_ir'], opt)
            same = semantic_equal(before, after=replay)
            item['identical_after_strip_debug'] = same
            item['original_semantic_sha256'] = hashlib.sha256(before.encode()).hexdigest()
            item['replay_semantic_sha256'] = hashlib.sha256(replay.encode()).hexdigest()
        validation.append(item)
        if not same:
            rejected.append(item)
            continue
        result[item['target'], item['tu']] = inline_records(item['record'])
    validated = pathlib.Path(path).with_name('verified-index.json')
    validated.write_text(json.dumps(validation + [i for i in rejected
                          if i not in validation], indent=2) + '\n')
    return result, rejected
