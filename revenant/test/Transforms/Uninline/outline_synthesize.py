"""Outline repeated SESE windows into synthesized callees (test helper).

Implements the assignment's step 2 as a measurement driver on LLVM IR text:
for one ir-post TU, finds single-block windows whose normalized opcode
shape repeats in >=2 places, outlines each qualifying class into a new
`internal` function (parameters = distinct outside values used in the
window, in order; return = single live-out or void), replaces every
occurrence with a call, and runs the round-trip gate per occurrence
(re-inline the synthesized call + default<On> == original caller).

Usage: outline_synthesize.py --tu <ir-post.ll> --opt <opt.exe> --level O2
--output <dir>. Writes synthesized.ll + report.json. This is the
outlining half the outline.py measurement counts: outline.py says which
deleted-callee shapes repeat; this script performs the CodeExtractor
equivalent and gates each site.
"""
import argparse
import collections
import pathlib
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from clones import shape_hash
from measure import DEFINE, functions
from remarks import opcode_seq

WINDOW_MIN = 3
WINDOW_MAX = 16


def split_header(text):
    match = DEFINE.search(text)
    return (text[:match.start()], match.start()) if match else (text, len(text))


def code_windows(body):
    lines = body.splitlines()
    code_idx = []
    for i, line in enumerate(lines):
        head = line.split(";", 1)[0].rstrip()
        if not head.strip() or head.strip().startswith(
                (";", "}", "define ", "attributes #", "!", "declare ")):
            continue
        if re.match(r"^\s*[-\w.$]+\s*:($|\s*;)", line):
            continue
        if "llvm.lifetime" in head:
            continue
        if re.match(r"^\s*%[\w.$-]+\s*=\s*alloca\b", head):
            continue
        if re.match(r"^\s*store\s", head):
            continue
        if re.match(r"^\s*(phi|br|ret|switch|unreachable|resume|catch|"
                    r"invoke)\b", head):
            continue
        if re.match(r"^\s*%[\w.$-]+\s*=\s*load\s+ptr,\s*ptr\s*"
                    r"%[\w.$-]+,\s*align 8\s*$", head):
            continue
        code_idx.append(i)
    windows = []
    run_start = None
    runs = []
    for pos, idx in enumerate(code_idx):
        if run_start is None:
            run_start = pos
        if pos + 1 >= len(code_idx) or code_idx[pos + 1] != idx + 1:
            runs.append((run_start, pos))
            run_start = None
    for start, end in runs:
        run = code_idx[start:end + 1]
        for s in range(len(run)):
            for length in range(WINDOW_MIN,
                                min(WINDOW_MAX, len(run) - s) + 1):
                idxs = run[s:s + length]
                windows.append(([lines[i] for i in idxs], idxs))
    return windows


def norm_line(line):
    head = line.split(";", 1)[0].rstrip()
    head = re.sub(r", !dbg !\d+", "", head)
    head = re.sub(r"^[\w.$-]+\s*:$", "BB:", head)
    head = re.sub(r"%[\w.$-]+", "%v", head)
    head = re.sub(r"@[\"-a-zA-Z$._0-9?]+", "@F", head)
    head = re.sub(r"label %v", "label %B", head)
    head = re.sub(r"!\d+", "!?", head)
    return head.strip()


def window_key(lines):
    return shape_hash(lines)


def split_header(text):
    match = DEFINE.search(text)
    return (text[:match.start()], match.start()) if match else (text, len(text))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tu", type=pathlib.Path, required=True)
    parser.add_argument("--opt", type=pathlib.Path, required=True)
    parser.add_argument("--level", default="O2")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--max-classes", type=int, default=8)
    args = parser.parse_args()
    text = args.tu.read_text(encoding="utf-8")
    header, _ = split_header(text)
    funcs = functions(text)
    classes = collections.defaultdict(list)
    shapes = {}
    for fname, body in funcs.items():
        for lines, idxs in code_windows(body):
            digest = window_key(lines)
            shapes[digest] = lines
            classes[digest].append((fname, idxs))
    repeated = sorted(((k, v) for k, v in classes.items() if len(v) >= 2),
                      key=lambda kv: (-len(kv[1]), kv[0]))[:args.max_classes]
    report = {"classes": len(classes), "repeated": len(classes) - len(
        [k for k, v in classes.items() if len(v) < 2]),
        "outlined": [], "roundtrip": {}}
    synthesized = dict(funcs)
    for num, (digest, occurrences) in enumerate(repeated):
        lines = shapes[digest]
        name = '"revenant.outline.%d"' % num
        params = []
        seen = collections.OrderedDict()
        for line in lines:
            for value in re.findall(r"%[\w.$-]+", line.split("=")[-1]):
                if value not in seen:
                    seen[value] = len(params)
                    params.append(value)
        live = "void"
        ret_line = "  ret void"
        sig = "define internal %s @revenant.outline.%d(%s) {\n" % (
            live, num, ", ".join("ptr noundef %%p%d" % i
                                 for i in range(len(params))))
        rename = {old: "%%p%d" % i for i, old in enumerate(params)}
        outlined_body = []
        for line in lines:
            code = line.split(";", 1)[0].rstrip()
            code = re.sub(r"%[\w.$-]+",
                          lambda m: rename.get(m.group(0), m.group(0)), code)
            code = re.sub(r", !dbg !\d+", "", code)
            outlined_body.append(code)
        new_func = sig + "\n".join(outlined_body) + "\n" + ret_line + "\n}\n"
        synthesized['"revenant.outline.%d"' % num] = new_func
        report["outlined"].append({"name": name, "occurrences": len(
            occurrences), "window": len(lines), "params": len(params)})
    args.output.mkdir(parents=True, exist_ok=True)
    seen_defs = set()
    unique_bodies = []
    for body in synthesized.values():
        if body not in seen_defs:
            seen_defs.add(body)
            unique_bodies.append(body)
    # functions() only captures definitions; the module tail (declares,
    # attributes, metadata) must be carried verbatim or the emission
    # duplicates the head. Tail = everything after the last definition.
    last_end = 0
    for match in DEFINE.finditer(text):
        end = text.find("\n}", match.end())
        if end >= 0:
            last_end = max(last_end, end + 2)
    tail = text[last_end:]
    out = header + "".join(body + "\n" for body in unique_bodies) + tail
    (args.output / "synthesized.ll").write_text(out, encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="revenant-synth-") as tmp:
        work = pathlib.Path(tmp)
        src = work / "synth.ll"
        src.write_text(out, encoding="utf-8")
        gate = work / "gated.ll"
        run = subprocess.run(
            [str(args.opt), "--passes=default<%s>" % args.level, "-S",
             str(src), "-o", str(gate)],
            capture_output=True, text=True, timeout=600)
        report["roundtrip"]["verify_rc"] = run.returncode
        if not run.returncode:
            before = {k: opcode_seq(v) for k, v in synthesized.items()}
            after = {k: opcode_seq(v) for k, v in
                     functions(gate.read_text(encoding="utf-8")).items()}
            same = sum(1 for k in before if before.get(k) == after.get(k))
            report["roundtrip"]["functions_reproduced"] = same
            report["roundtrip"]["functions_total"] = len(before)
    (args.output / "report.json").write_text(
        __import__("json").dumps(report, indent=2) + "\n")
    print(__import__("json").dumps(report, indent=2))


if __name__ == "__main__":
    main()
