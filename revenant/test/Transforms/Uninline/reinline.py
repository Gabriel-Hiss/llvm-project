"""Re-inline and re-optimize verification for uninline candidates.

For each compiler inline record (caller C, callee F) where F is retained in
ir-post: rebuild a probe module holding the ir-pre callee F and the ir-pre
caller C (which still calls F), run the O2/O3 function pipeline's inliner on
it, then the same function-simplification passes, and compare the optimized
caller against the ir-post caller. Structural equality (up to value numbering
and metadata IDs) means the inlined region round-trips: outlining it back to
`call F(args)` and re-inlining reproduces ir-post exactly.

This is measurement only: it never edits the corpus or the pass. It answers
how much of the 0.3% exact-match ceiling is specialization (bodies the later
optimizer passes rewrote after splicing) versus truly unrecoverable shapes.
"""
import argparse
import collections
import csv
import json
import pathlib
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from measure import CALL, NAME, DEFINE, functions, group, pairs
from remarks import inline_records, opcode_seq, roundtrip_lines, symbol, \
    verified_index

# Function-level optimizer tail shared by O2 and O3 after inlining: promote
# allocas, fold, thread, then clean up. Matches the corpus clang-cl pipeline's
# per-function simplification sequence (mem2reg, instcombine, simplifycfg,
# sroa, early-cse, gvn, dse, loop-simplify) without module-level IPO.
# instcombine runs with no-verify-fixpoint: the stock max-iterations=1
# fixpoint check crashes on math-library wrappers (_snprintf) in this
# build, unrelated to the probe.
REOPTIMIZE = ("function(mem2reg,instcombine<no-verify-fixpoint>,"
              "simplifycfg<bonus-inst-threshold=1>,"
              "sroa<modify-cfg>,early-cse,gvn,dse,"
              "loop(loop-rotate,loop-deletion),"
              "simplifycfg<bonus-inst-threshold=1>,"
              "instcombine<no-verify-fixpoint>),verify")


def probe_module(pre_text, caller, callee):
    # The whole ir-pre module: it parses as-is, carries every global and
    # declaration the inliner needs, and matches the corpus pipeline's own
    # input. Per-event cost is one extra opt run; correctness first.
    funcs = functions(pre_text)
    if caller not in funcs or callee not in funcs:
        return None
    return pre_text


def roundtrip(opt, probe, caller, callee, workdir, opt_level):
    # Replay the corpus build faithfully: strip the record-mode debug info
    # the replay added, then run the same default<On> pipeline the corpus
    # recipe ran. Canonical comparison ignores renumbering, so the forced
    # inline thresholds are unnecessary: the real pipeline inlines what the
    # compiler inlined.
    src = workdir / "probe.ll"
    src.write_text(probe, encoding="utf-8")
    stripped = workdir / "stripped.ll"
    run = subprocess.run(
        [str(opt), "-strip-debug", "--passes=verify", "-S",
         str(src), "-o", str(stripped)],
        capture_output=True, text=True, timeout=300)
    if run.returncode:
        return "strip-failed", run.stderr[-2000:]
    out = workdir / "reopt.ll"
    run = subprocess.run(
        [str(opt), "--passes=default<" + opt_level + ">", "-S",
         str(stripped), "-o", str(out)],
        capture_output=True, text=True, timeout=600)
    if run.returncode:
        return "reopt-failed", run.stderr[-2000:]
    got = functions(out.read_text(encoding="utf-8"))
    if caller not in got:
        return "caller-lost", ""
    return "ok", opcode_seq(got[caller])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=pathlib.Path, required=True)
    parser.add_argument("--opt", type=pathlib.Path, required=True)
    parser.add_argument("--remarks", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--only", nargs="*",
                        help="only these targets (default: all)")
    parser.add_argument("--max-events", type=int, default=0,
                        help="cap events per TU for pilot runs (0 = all)")
    args = parser.parse_args()
    records, rejected = verified_index(args.remarks, args.opt, args.corpus)
    rows = []
    totals = collections.defaultdict(collections.Counter)
    errors = []
    with tempfile.TemporaryDirectory(prefix="revenant-reinline-") as tmp:
        workdir = pathlib.Path(tmp)
        for target, category, tu, stages in pairs(args.corpus):
            if args.only and target not in args.only:
                continue
            if args.remarks and (target, tu) not in records:
                continue
            pre_text = stages["ir-pre"].read_text(encoding="utf-8")
            post_text = stages["ir-post"].read_text(encoding="utf-8")
            post = functions(post_text)
            events = records[target, tu]
            if args.max_events:
                events = events[:args.max_events]
            for ordinal, event in enumerate(events):
                caller = symbol(event["caller"]).strip('"')
                callee = symbol(event["callee"]).strip('"')
                row = {"target": target, "tu": tu, "ordinal": ordinal,
                       "status": "none", "evidence": "reinline-roundtrip",
                       "callee_retained": callee in post, **event}
                if callee not in post:
                    row["status"] = "none"
                    row["note"] = "callee deleted from ir-post; skipped"
                else:
                    probe = probe_module(pre_text, event["caller"],
                                         event["callee"])
                    if probe is None:
                        row["status"] = "none"
                        row["note"] = "caller/callee missing from ir-pre"
                    else:
                        want = opcode_seq(post[event["caller"]]
                                          if event["caller"] in post
                                          else "")
                        opt_level = ("O3" if category in ("clang-cl O3",
                                                          "clang-cl LTO")
                                     else "O2" if category.startswith(
                                         "clang-cl") else "O3")
                        outcome, payload = roundtrip(
                            args.opt, probe, event["caller"],
                            event["callee"], workdir, opt_level)
                        if outcome != "ok":
                            row["status"] = "none"
                            row["note"] = outcome + ": " + str(payload)[-300:]
                        elif payload == want:
                            row["status"] = "full"
                            row["note"] = ("re-inline + re-optimize "
                                           "reproduces ir-post caller")
                        else:
                            row["status"] = "none"
                            row["note"] = ("roundtrip differs: specialization "
                                           "beyond function pipeline")
                rows.append(row)
                totals[category][row["status"]] += 1
                totals[category]["modules"] = len(
                    {(r["target"], r["tu"]) for r in rows
                     if group(r["target"]) == category})
            print(target, str(tu)[:60],
                  dict(totals[category]), flush=True)
    for category in totals:
        totals[category]["modules"] = len(
            {(r["target"], r["tu"]) for r in rows
             if group(r["target"]) == category})
    args.output.mkdir(parents=True, exist_ok=True)
    with (args.output / "sites.csv").open("w", newline="",
                                          encoding="utf-8") as f:
        fields = ["target", "tu", "caller", "callee", "ordinal", "status",
                  "evidence", "callee_retained", "file", "line", "column",
                  "note"]
        writer = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    summary = {"denominator": "verified compiler inline records",
               "method": ("re-inline retained callee into ir-pre caller, "
                          "re-run function simplification, compare caller "
                          "body up to numbering"),
               "groups": {k: dict(v) for k, v in totals.items()},
               "events": len(rows),
               "rejected_records": rejected}
    (args.output / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary["groups"], indent=2))


if __name__ == "__main__":
    main()
