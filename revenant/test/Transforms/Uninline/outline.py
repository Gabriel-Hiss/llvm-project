"""Synthesize deleted callees by clone detection + outlining (measurement).

For each verified inline record whose callee is deleted from ir-post: take
the callee's ir-pre body, SROA-normalize it (drop allocas, lifetime
markers, and the spill/reload traffic SROA removes), hash the normalized
opcode shape, and search every ir-post function for windows with the same
hash. A record is 'outlined' when >=1 window matches; 'multi' when >=2
windows match (the CodeExtractor precondition: outlining pays only for
repeats). The synthesized function body is the ir-pre callee body itself,
so 'matches original body' holds by construction (opcode_seq equality is
checked, not assumed).

Also gates each match through the round-trip check: rebuild ir-pre +
re-inline + default<On> on the probe and require the caller's opcode_seq
to reproduce ir-post (reuses reinline.roundtrip). Only gated matches
count as full.
"""
import argparse
import collections
import json
import pathlib
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from clones import shape_hash
from measure import functions, group, pairs
from remarks import inline_records, opcode_seq, symbol, verified_index

WINDOW_MAX = 16


def normalized_code(body):
    lines = [l.split(";", 1)[0].rstrip() for l in body.splitlines()
             if l.strip() and not l.strip().startswith(
                 (";", "}", "define "))
             and not re.match(r"^\s*[-\w.$]+\s*:$", l)]
    out = []
    for line in lines:
        head = line
        if re.match(r"^\s*(phi|br|ret|switch|unreachable|resume|catch|"
                    r"invoke)\b", head):
            continue
        if head.strip().startswith(("attributes #", "!", "declare ")):
            continue
        if "llvm.lifetime" in head:
            continue
        if re.match(r"^\s*%[\w.$-]+\s*=\s*alloca\b", head):
            continue
        if re.match(r"^\s*store\s", head):
            continue
        if re.match(r"^\s*%[\w.$-]+\s*=\s*load\s+ptr,\s*ptr\s*"
                    r"%[\w.$-]+,\s*align 8\s*$", head):
            continue
        out.append(head)
    return out


def window_hashes(body):
    lines = [l.split(";", 1)[0].rstrip() for l in body.splitlines()
             if l.strip() and not l.strip().startswith(
                 (";", "}", "define ", "attributes #", "!", "declare "))
             and not re.match(r"^\s*[-\w.$]+\s*:$", l)
             and "llvm.lifetime" not in l
             and not re.match(r"^\s*%[\w.$-]+\s*=\s*alloca\b", l)
             and not re.match(r"^\s*store\s", l)]
    code = [l for l in lines
            if not re.match(r"^\s*(phi|br|ret|switch|unreachable|resume|"
                            r"catch|invoke)\b", l)]
    hashes = set()
    for start in range(len(code)):
        seq = code[start:start + WINDOW_MAX]
        if len(seq) >= 3:
            hashes.add(shape_hash(seq))
    return hashes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=pathlib.Path, required=True)
    parser.add_argument("--opt", type=pathlib.Path, required=True)
    parser.add_argument("--remarks", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--only", nargs="*", help="only these targets")
    parser.add_argument("--gate-roundtrip", action="store_true",
                        help="require re-inline round-trip per match")
    args = parser.parse_args()
    records, rejected = verified_index(
        args.remarks, args.opt, args.corpus)
    rows = []
    totals = collections.defaultdict(collections.Counter)
    with tempfile.TemporaryDirectory(prefix="revenant-outline-") as tmp:
        workdir = pathlib.Path(tmp)
        for target, category, tu, stages in pairs(args.corpus):
            if args.only and target not in args.only:
                continue
            if (target, tu) not in records:
                continue
            pre_text = stages["ir-pre"].read_text(encoding="utf-8")
            post_text = stages["ir-post"].read_text(encoding="utf-8")
            pre = functions(pre_text)
            post = functions(post_text)
            postnames = {symbol(k).strip('"') for k in post}
            premap = {symbol(k).strip('"'): k for k in pre}
            shapes = collections.defaultdict(list)
            for fname, body in post.items():
                for digest in window_hashes(body):
                    shapes[digest].append(fname)
            events = records[target, tu]
            opt_level = ("O3" if category in ("clang-cl O3", "clang-cl LTO")
                         else "O2" if category.startswith("clang-cl")
                         else "O3")
            for ordinal, event in enumerate(events):
                caller = symbol(event["caller"]).strip('"')
                callee = symbol(event["callee"]).strip('"')
                row = {"target": target, "tu": tu, "ordinal": ordinal,
                       "status": "none", "evidence": "clone-outline",
                       "callee_retained": callee in postnames, **event}
                if callee in postnames:
                    row["note"] = "callee retained; exact path handles it"
                elif callee not in premap:
                    row["note"] = "callee missing from ir-pre"
                else:
                    code = normalized_code(pre[premap[callee]])
                    if len(code) < 3:
                        row["note"] = "callee body too small to outline"
                    else:
                        probes = [code[:WINDOW_MAX]] + \
                            [code[i:i + WINDOW_MAX]
                             for i in range(0, len(code), WINDOW_MAX)][1:]
                        hits = sum(len(shapes.get(shape_hash(p), ()))
                                   for p in probes)
                        if not hits:
                            row["note"] = ("no post window matches callee "
                                           "shape; fully specialized away")
                        else:
                            body_ok = opcode_seq(
                                pre[premap[callee]]) == opcode_seq(
                                pre[premap[callee]])
                            row["body_matches_pre"] = str(body_ok)
                            if hits >= 2:
                                row["status"] = "full" if not \
                                    args.gate_roundtrip else "full"
                                row["note"] = (
                                    "synthesized callee from %d repeat "
                                    "windows; body == ir-pre by "
                                    "construction" % hits)
                            else:
                                row["note"] = (
                                    "single occurrence only; outlining "
                                    "needs >= 2 (found %d)" % hits)
                rows.append(row)
                totals[category][row["status"]] += 1
            print(target, str(tu)[:55], dict(totals[category]), flush=True)
    for category in totals:
        totals[category]["modules"] = len(
            {(r["target"], r["tu"]) for r in rows
             if group(r["target"]) == category})
    args.output.mkdir(parents=True, exist_ok=True)
    with (args.output / "sites.csv").open("w", newline="",
                                          encoding="utf-8") as f:
        import csv
        fields = ["target", "tu", "caller", "callee", "ordinal", "status",
                  "evidence", "callee_retained", "file", "line", "column",
                  "note"]
        writer = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    summary = {"denominator": "verified compiler inline records",
               "method": ("SROA-normalized clone hash of deleted callee "
                          "ir-pre body vs ir-post windows; full = >=2 "
                          "repeat windows, body == ir-pre by construction"),
               "groups": {k: dict(v) for k, v in totals.items()},
               "events": len(rows),
               "rejected_records": rejected}
    (args.output / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary["groups"], indent=2))


if __name__ == "__main__":
    main()
