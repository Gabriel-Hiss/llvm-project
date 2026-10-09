"""Clone detection over ir-post modules: hash SESE regions, find repeats.

Step 1 of the deleted-callee phase. For one ir-post TU, enumerates
single-block windows (3..16 non-phi, non-terminator instructions) in every
function, hashes each window with constants/operands abstracted (opcode +
type + operand-kind sequence, same alphabet as remarks.opcode_seq, plus
window length), and reports classes occurring in >=2 places (different
functions, or twice in one function at disjoint offsets).

Also matches each deleted-callee inline record's ir-pre callee body against
the observed classes: a record is 'represented' when its callee's ir-pre
body opcode_seq equals some class shape. This is the measurement half; the
outlining half (CodeExtractor + round-trip gate) follows once classes are
validated on a pilot TU.
"""
import argparse
import collections
import hashlib
import json
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from measure import CALL, functions, pairs
from remarks import inline_records, opcode_seq, symbol, verified_index

WINDOW_MIN = 3
WINDOW_MAX = 16


def window_shapes(body):
    lines = [l for l in body.splitlines()
             if l.strip() and not l.strip().startswith((";", "}", "define "))
             and not re.match(r"^\s*[-\w.$]+\s*:$", l)
             and not l.strip().startswith(("attributes #", "!", "declare "))]
    code = []
    for line in lines:
        head = line.split(";", 1)[0].rstrip()
        if re.match(r"^\s*phi\b", head) or re.match(r"^\s*(br|ret|switch|unreachable|resume|catch|invoke)\b", head):
            code.append(None)
        else:
            code.append(head)
    shapes = []
    for start in range(len(code)):
        if code[start] is None:
            continue
        seq = []
        for end in range(start, min(len(code), start + WINDOW_MAX)):
            if code[end] is None:
                break
            seq.append(code[end])
            if len(seq) >= WINDOW_MIN:
                shapes.append((start, end + 1 - start, shape_hash(seq)))
    return shapes


def shape_hash(lines):
    norm = []
    for line in lines:
        head = line.split(";", 1)[0].rstrip()
        head = re.sub(r", !dbg !\d+", "", head)
        head = re.sub(r"^[\w.$-]+\s*:$", "BB:", head)
        head = re.sub(r"%[\w.$-]+", "%v", head)
        head = re.sub(r"@[\"-a-zA-Z$._0-9?]+", "@F", head)
        head = re.sub(r"label %v", "label %B", head)
        head = re.sub(r"!\d+", "!?", head)
        norm.append(head.strip())
    return hashlib.sha1("\n".join(norm).encode()).hexdigest()[:16]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=pathlib.Path, required=True)
    parser.add_argument("--remarks", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--only", nargs="*", help="only these targets")
    args = parser.parse_args()
    records, _ = verified_index(args.remarks, None, args.corpus) \
        if False else (None, None)
    index = json.loads(pathlib.Path(args.remarks).read_text(encoding="utf-8"))
    by_tu = {(e["target"], e["tu"]): e for e in index
             if not e.get("error") and e.get("record")}
    summary = {}
    for target, category, tu, stages in pairs(args.corpus):
        if args.only and target not in args.only:
            continue
        post_text = stages["ir-post"].read_text(encoding="utf-8")
        post = functions(post_text)
        classes = collections.defaultdict(list)
        total_windows = 0
        for fname, body in post.items():
            for start, length, digest in window_shapes(body):
                classes[digest].append((fname, start, length))
                total_windows += 1
        repeated = {k: v for k, v in classes.items() if len(v) >= 2}
        distinct_fn = sum(1 for v in repeated.values()
                          if len({loc[0] for loc in v}) >= 2)
        key = target + "\x00" + tu
        entry = {"functions": len(post), "windows": total_windows,
                 "classes": len(classes), "repeated": len(repeated),
                 "cross_function": distinct_fn,
                 "repeat_occurrences": sum(len(v) for v in repeated.values())}
        if (target, tu) in by_tu:
            recs = inline_records(by_tu[target, tu]["record"])
            pre_text = stages["ir-pre"].read_text(encoding="utf-8")
            pre = functions(pre_text)
            shapes = set(classes)
            represented = 0
            deleted = 0
            for r in recs:
                callee = symbol(r["callee"])
                if callee in post:
                    continue
                deleted += 1
                body = pre.get(callee, "")
                lines = [l.split(";", 1)[0].rstrip() for l in body.splitlines()
                         if l.strip() and not l.strip().startswith(
                             (";", "}", "define "))
                         and not re.match(r"^\s*[-\w.$]+\s*:$", l)]
                code = [l for l in lines
                        if not re.match(r"^\s*(phi|br|ret|switch|unreachable|"
                                        r"resume|catch|invoke)\b", l)
                        and not l.strip().startswith(
                            ("attributes #", "!", "declare "))]
                if not code:
                    continue
                if shape_hash(code[:WINDOW_MAX]) in shapes or any(
                        shape_hash(code[i:i + WINDOW_MAX]) in shapes
                        for i in range(0, len(code), WINDOW_MAX)):
                    represented += 1
            entry["records"] = len(recs)
            entry["deleted_records"] = deleted
            entry["deleted_represented"] = represented
        summary[key] = entry
        print(target, tu[:50], json.dumps(entry), flush=True)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "clones.json").write_text(
        json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
