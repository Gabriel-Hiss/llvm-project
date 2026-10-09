#!/usr/bin/env python3
"""Parity loop for the LLVM fork's `revenant --export-parity`.

Wraps the harness compare.py (boundary parity: missing / conflicts /
extra) and adds the ASM 1:1 metric on top of its records:

  asm_text_mismatch — a listed item whose normalized Intel text or its
                  resolved operand identity differs from the compiler's
                  disassembly. Operand identity is resolved on both
                  sides: the fork emits `target`/`mem` RVAs in
                  revenant-asm.jsonl; the oracle side is resolved by
                  re-decoding the final binary with iced-x86 (the same
                  decoder the oracle used), because oracle `asm` text
                  may carry unrelocated object literals in RIP-relative
                  brackets. Symbols never compare by name.

Normalization before compare: whitespace/case folded, `0xN` -> `Nh`,
zero padding stripped; resolved RIP-relative memory operands and near
branch operands become @. Their RVAs are compared separately by operand
kind. `(linker import thunk)` annotations are dropped. Everything else —
mnemonic, registers, sizes, displacements — compares literally.

Usage (absolute paths; compare.py subprocesses cwd into outdirs):

  python run_parity.py --revenant-dir <build>/tools/revenant
      --parity-root <repo>/target/parity --tag swe2pd [--only id ...]
      [--asm-only] [--out summary.json]

Without --only, every C/C++ corpus id (kind native or gnu) that has an
oracle is compared. Sets REVENANT_PARITY_ASM=1 for its own runs.
"""

import argparse
import json
import os
import re
import struct
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

HARNESS = Path(r"C:\Users\User\RustroverProjects\Revenant\.worktrees\harness\corpus\parity")
PY = Path(r"C:\Users\User\RustroverProjects\Revenant\target\parity-venv\Scripts\python.exe")
COMPARE = HARNESS / "compare.py"

sys.path.insert(0, str(HARNESS))
from paritylib import parse_pe, rva_to_off  # noqa: E402

from iced_x86 import (  # noqa: E402
    Decoder, Formatter, FormatterSyntax, FlowControl, OpKind,
)


def corpus_ids(parity_root: Path, ccpp_only=True):
    targets = []
    for frag in sorted((HARNESS / "corpus.d").glob("*.json")) + [HARNESS / "corpus.json"]:
        try:
            targets += json.loads(frag.read_text())["targets"]
        except Exception:
            continue
    ids = []
    for t in targets:
        if ccpp_only and t.get("kind") not in ("native", "gnu"):
            continue
        if (parity_root / t["id"] / "oracle" / "oracle-items.bin").exists():
            ids.append(t["id"])
    return sorted(set(ids))


def load_items(path):
    raw = Path(path).read_bytes()
    return [(r, n) for r, n in struct.iter_unpack("<II", raw)]


def load_meta(path):
    """(rva, len) -> oracle record; first wins on duplicate lines."""
    out = {}
    for line in Path(path).read_text().splitlines():
        rec = json.loads(line)
        out.setdefault((rec["rva"], rec["len"]), rec)
    return out


_HEX0X = re.compile(r"0x[0-9a-fA-F]+")
_HEXLIT = re.compile(r"\b0*([0-9a-fA-F]+)h\b")
_RIPBR = re.compile(r"\[\s*rip\s*\+\s*(?:\.\.\.|0x[0-9a-fA-F]+|[0-9a-fA-F]+h)\s*\]")
_BAREABS = re.compile(r"\[\s*(?:0x[0-9a-fA-F]+|[0-9a-fA-F]+h)\s*\]")
_PAREN = re.compile(r"\s*\([^)]*\)\s*$")
_WS = re.compile(r"\s+")


def canon(text, near_branch=False, ip_rel_memory=False):
    """Normalize formatting without hiding unrelated numeric operands."""
    t = _PAREN.sub("", text).strip().lower()
    if ip_rel_memory:
        t = _RIPBR.sub("[@]", t)
        # Some compiler records contain the unrelocated object literal
        # instead of a RIP expression. The final-binary decode identified
        # this as the RIP-relative memory operand.
        t = _BAREABS.sub("[@]", t)
    if near_branch:
        # A direct near branch has one destination operand. Its identity
        # is compared separately, so symbol spelling and address syntax
        # do not affect the text metric.
        t = re.sub(r"^(\S+)\s+.*$", r"\1@", t)
    t = _HEX0X.sub(lambda m: f"{int(m.group(0), 16)}h", t)
    t = _HEXLIT.sub(lambda m: f"{m.group(1).lower()}h", t)
    t = _WS.sub("", t)
    return t


def oracle_refs(ins):
    """Resolved target RVAs, kept separate for branch and RIP memory."""
    target = ins.near_branch_target if (
        ins.flow_control in (FlowControl.UNCONDITIONAL_BRANCH,
                             FlowControl.CONDITIONAL_BRANCH,
                             FlowControl.CALL)
        and ins.op0_kind == OpKind.NEAR_BRANCH64) else None
    mem = ins.ip_rel_memory_address if ins.is_ip_rel_memory_operand else None
    return target, mem


def asm_metric(bindir: Path, revdir: Path, binary: Path):
    """ASM 1:1: text + operand identity on shared rvas."""
    side = revdir / "revenant-asm.jsonl"
    meta = load_meta(bindir / "oracle" / "oracle-items.jsonl")
    if not side.exists():
        return {"asm_checked": 0, "asm_text_mismatch": 0, "asm_sidecar": "absent"}
    raw = binary.read_bytes()
    sections, _base, _pdb = parse_pe(raw)
    rev = {}
    for line in side.read_text().splitlines():
        rec = json.loads(line)
        rev[(rec["rva"], rec["len"])] = rec
    fmt = Formatter(FormatterSyntax.INTEL)
    checked = mism = 0
    mismatches = []
    for key, rec in rev.items():
        o = meta.get(key)
        if not o:
            continue  # boundary metric owns conflict/extra
        rva, ln = key
        o_target = o_mem = None
        if off is not None:
            ins = next(iter(Decoder(64, raw[off:off + ln], ip=rva)), None)
            if ins is not None:
                o_target, o_mem = oracle_refs(ins)
        r_target, r_mem = rec.get("target"), rec.get("mem")
        want = canon(o_text, near_branch=o_target is not None,
                     ip_rel_memory=o_mem is not None)
        got = canon(rec.get("asm", ""), near_branch=r_target is not None,
                    ip_rel_memory=r_mem is not None)
        ok = (got == want and o_target == r_target and o_mem == r_mem)
        checked += 1
        if not ok:
            mism += 1
            mismatches.append({"rva": rva, "len": ln,
                               "revenant": rec.get("asm"),
                               "oracle": o_text,
                               "o_refs": o_refs, "r_refs": r_refs,
                               "owner": o.get("owner", "?")})
    return {"asm_checked": checked, "asm_text_mismatch": mism,
            "asm_mismatches": mismatches[:200]}


def run_compare(parity_root, exporter, tag, only):
    env = dict(os.environ)
    env["REVENANT_PARITY_ASM"] = "1"
    cmd = [str(PY), str(COMPARE), str(parity_root),
           "--exporter", str(exporter), "--cli", str(exporter),
           "--tag", tag]
    if only:
        cmd += ["--only"] + only
    res = subprocess.run(cmd, capture_output=True, text=True, env=env)
    sys.stdout.write(res.stdout)
    sys.stderr.write(res.stderr)
    return res.returncode


def merge_asm(parity_root, tag, only):
    rows = []
    wanted = set(only)
    for pj in sorted(parity_root.glob(f"*/parity-{tag}.json")):
        rec = json.loads(pj.read_text())
        if rec.get("id") not in wanted:
            continue
        bindir = pj.parent
        manifest = json.loads((bindir / "manifest.json").read_text())
        rec.update(asm_metric(bindir, bindir / f"revenant-{tag}",
                              bindir / manifest["binary"]))
        pj.write_text(json.dumps(rec, indent=2))
        rows.append(rec)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--revenant-dir", required=True, type=Path,
                    help="dir containing revenant.exe + export-parity.cmd")
    ap.add_argument("--parity-root", required=True, type=Path)
    ap.add_argument("--tag", default="swe2pd")
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("--all-kinds", action="store_true")
    ap.add_argument("--asm-only", action="store_true",
                    help="skip compare.py; only (re)compute asm metrics")
    ap.add_argument("--out", type=Path)
    args = ap.parse_args()

    exporter = args.revenant_dir / "export-parity.cmd"
    if not exporter.exists() or not (args.revenant_dir / "revenant.exe").exists():
        sys.exit(f"exporter pair missing in {args.revenant_dir}")

    only = args.only
    if only is None:
        only = corpus_ids(args.parity_root, ccpp_only=not args.all_kinds)

    if not only:
        sys.exit("no oracle-backed corpus ids selected")

    if not args.asm_only:
        rc = run_compare(args.parity_root, exporter, args.tag, only)
        if rc != 0:
            sys.exit(f"compare.py exited {rc}")

    rows = merge_asm(args.parity_root, args.tag, only)
    missing_reports = sorted(set(only) - {r.get("id") for r in rows})
    if missing_reports:
        sys.exit(f"missing parity reports: {', '.join(missing_reports)}")
    compare_summary = args.parity_root / f"summary-{args.tag}.json"
    if not compare_summary.exists():
        sys.exit(f"missing compare summary: {compare_summary}")
    clusters = json.loads(compare_summary.read_text()).get(
        "failure_clusters", {})
    owner = Counter(dict(clusters.get("by_owner", [])))
    mnem = Counter(dict(clusters.get("by_mnemonic", [])))
    evidence = defaultdict(list)
    for r in rows:
        for rec in (r.get("records") or {}).get("missing", []):
            owner_name = rec.get("owner", "?")
            evidence[owner_name].append(
                {"id": r.get("id"), "kind": "missing", **rec})
        for rec in (r.get("records") or {}).get("conflicts", []):
            owner_name = rec.get("owner", "?")
            evidence[owner_name].append(
                {"id": r.get("id"), "kind": "conflict", **rec})
        for rec in r.get("asm_mismatches", []):
            owner_name = rec.get("owner", "?")
            owner[owner_name] += 1
            mnem[(rec.get("oracle") or "?").split()[0]] += 1
            evidence[owner_name].append(
                {"id": r.get("id"), "kind": "asm_text", **rec})
    summary = {"binaries": len(rows),
               "clean": sum(1 for r in rows
                            if r.get("status") == "ok"
                            and not (r.get("missing") or r.get("conflicts")
                                     or r.get("asm_text_mismatch"))
                            and r.get("asm_sidecar") != "absent"),
               "total_missing": sum(r.get("missing") or 0 for r in rows),
               "total_conflicts": sum(r.get("conflicts") or 0 for r in rows),
               "total_asm_text_mismatch": sum(r.get("asm_text_mismatch") or 0
                                              for r in rows),
               "clusters_by_function_symbol": owner.most_common(30),
               "clusters_by_mnemonic": mnem.most_common(30),
               "evidence_by_function_symbol": {
                   name: records[:50] for name, records in evidence.items()},
               "rows": [{"id": r.get("id"), "status": r.get("status"),
                         "missing": r.get("missing"),
                         "conflicts": r.get("conflicts"),
                         "extra": r.get("extra_revenant_items"),
                         "asm_checked": r.get("asm_checked"),
                         "asm_text_mismatch": r.get("asm_text_mismatch")}
                        for r in rows]}
    print(json.dumps({k: v for k, v in summary.items() if k != "rows"},
                     indent=2))
    if args.out:
        args.out.write_text(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
