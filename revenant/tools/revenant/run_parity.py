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
    Decoder, FlowControl, Formatter, FormatterSyntax, Mnemonic, OpKind,
    Register,
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


_RIPBR = re.compile(r"\[\s*rip\s*(?:\+|-)\s*(?:\.\.\.|0x[0-9a-fA-F]+|[0-9a-fA-F]+h)\s*\]")
_BAREABS = re.compile(r"\[\s*(?:0x[0-9a-fA-F]+|[0-9a-fA-F]+h)\s*\]")
_PAREN = re.compile(r"\s*\([^)]*\)\s*$")
_WS = re.compile(r"\s+")
_PTR = re.compile(
    r"\b(?:byte|word|dword|qword|tbyte|fword|xmmword|ymmword|zmmword|oword)\s+ptr\s+")
_SCALE = re.compile(r"(?<![a-z0-9_])(\d+)\s*\*\s*([a-z][a-z0-9]*)\b")
_NUMBER = re.compile(
    r"(?<![a-z0-9_#])(?:0x[0-9a-f]+|[0-9a-f]+h|\d+)(?![a-z0-9_])")
_REG = re.compile(
    r"\b(?:[er]?(?:ax|bx|cx|dx|si|di|bp|sp)|"
    r"(?:al|ah|bl|bh|cl|ch|dl|dh|spl|bpl|sil|dil)|"
    r"r(?:1[0-5]|[89])(?:[bwd])?)\b")
_IMMEDIATE_WIDTH = {
    "byte": 8, "word": 16, "dword": 32, "qword": 64,
}


def _number_value(token):
    if token.startswith("0x"):
        return int(token[2:], 16)
    if token.endswith("h"):
        return int(token[:-1], 16)
    return int(token, 10)


def _operand_width(text):
    ptr = re.search(r"\b(byte|word|dword|qword)\s+ptr\b", text)
    if ptr:
        return _IMMEDIATE_WIDTH[ptr.group(1)]
    widths = []
    for match in _REG.finditer(text):
        reg = match.group(0)
        if reg.endswith("b") or reg in {
                "al", "ah", "bl", "bh", "cl", "ch", "dl", "dh",
                "spl", "bpl", "sil", "dil"}:
            widths.append(8)
        elif reg.endswith("w") or reg in {
                "ax", "bx", "cx", "dx", "si", "di", "bp", "sp"}:
            widths.append(16)
        elif reg.startswith("e") or reg.endswith("d"):
            widths.append(32)
        else:
            widths.append(64)
    return max(widths, default=None)


def canon(text, near_branch=False, ip_rel_memory=False,
          symbol_memory=False, memory_index=None, memory_scale=1):
    """Normalize printer-only Intel differences and preserve operand identity."""
    t = _PAREN.sub("", text).strip().lower()
    if ip_rel_memory:
        t = _RIPBR.sub("[@]", t)
        # The oracle may contain an unrelocated object literal; the final
        # binary decode established that this bracket operand is RIP-relative.
        t = _BAREABS.sub("[@]", t)
    elif symbol_memory:
        expr = "[@"
        if memory_index is not None:
            expr += "+" + memory_index
            if memory_scale != 1:
                expr += "*" + str(memory_scale)
        expr += "]"
        t = re.sub(r"\[[^\]]*\]", expr, t, count=1)
    if near_branch:
        mnemonic = t.split(None, 1)[0] if t else ""
        return mnemonic + "@"
    string = re.match(
        r"^(rep(?:e|ne)?\s+)?(movs|cmps|scas|stos|lods)(b|w|d|q)\b", t)
    if string and (string.group(1) or not re.search(r"\b[xyz]mm\d", t)):
        return _WS.sub("", (string.group(1) or "") +
                        string.group(2) + string.group(3))
    if re.fullmatch(r"xchg\s+([a-z0-9]+)\s*,\s*\1", t):
        return "nop"
    width = _operand_width(t)
    def mask_negative_immediate(match):
        value = -_number_value(match.group(1))
        if width is not None:
            value %= 1 << width
        return ",#" + str(value)
    t = re.sub(r",\s*-(0x[0-9a-f]+|[0-9a-f]+h|\d+)\b",
               mask_negative_immediate, t)
    t = _PTR.sub("", t)
    t = _SCALE.sub(r"\2*\1", t)
    t = _NUMBER.sub(lambda m: f"#{_number_value(m.group(0))}", t)
    t = _WS.sub("", t)
    t = re.sub(r"^movabs", "mov", t)
    shift = re.fullmatch(r"(shl|shr|sal|sar|rol|ror|rcl|rcr)([^,]+)", t)
    if shift:
        t = f"{shift.group(1)}{shift.group(2)},#1"
    imul = re.fullmatch(r"imul([^,]+),([^,]+)", t)
    if imul:
        t = f"imul{imul.group(1)},{imul.group(1)},{imul.group(2)}"
    return t


def oracle_refs(ins):
    """Resolved direct-branch and RIP-relative memory target RVAs."""
    target = ins.near_branch_target if (
        ins.flow_control in (FlowControl.UNCONDITIONAL_BRANCH,
                             FlowControl.CONDITIONAL_BRANCH,
                             FlowControl.CALL)
        and ins.op0_kind == OpKind.NEAR_BRANCH64) else None
    mem = ins.ip_rel_memory_address if ins.is_ip_rel_memory_operand else None
    return target, mem


def register_family(register, formatter):
    if register == Register.NONE:
        return None
    name = formatter.format_register(register).lower()
    aliases = {
        "rax": ("rax", "eax", "ax", "al", "ah"),
        "rbx": ("rbx", "ebx", "bx", "bl", "bh"),
        "rcx": ("rcx", "ecx", "cx", "cl", "ch"),
        "rdx": ("rdx", "edx", "dx", "dl", "dh"),
        "rsi": ("rsi", "esi", "si", "sil"),
        "rdi": ("rdi", "edi", "di", "dil"),
        "rbp": ("rbp", "ebp", "bp", "bpl"),
        "rsp": ("rsp", "esp", "sp", "spl"),
    }
    for family, names in aliases.items():
        if name in names:
            return family
    match = re.fullmatch(r"(r(?:[89]|1[0-5]))(?:[bwd])?", name)
    return match.group(1) if match else None
def is_image_rva(sections, rva):
    return any(section.rva <= rva < section.rva + section.vsize
               for section in sections)


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
    formatter = Formatter(FormatterSyntax.INTEL)
    checked = mism = 0
    mismatches = []
    register_values = {}
    last_owner = None
    next_rva = None
    for key, rec in rev.items():
        oracle = meta.get(key)
        rva, length = key
        owner = oracle.get("owner") if oracle else None
        if rva != next_rva or (owner is not None and owner != last_owner):
            register_values.clear()
        off = rva_to_off(sections, rva)
        ins = (next(iter(Decoder(
            64, raw[off:off + length], ip=rva)), None) if off is not None else None)
        oracle_target = oracle_mem = None
        symbol_mem = None
        memory_index = None
        memory_scale = 1
        if ins is not None:
            oracle_target, oracle_mem = oracle_refs(ins)
            if oracle_mem is None:
                base = ins.memory_base
                index = ins.memory_index
                displacement = ins.memory_displacement
                displacement_size = ins.memory_displ_size
                if displacement_size:
                    bits = displacement_size * 8
                    if displacement & (1 << (bits - 1)):
                        displacement -= 1 << bits
                base_family = register_family(base, formatter)
                base_value = register_values.get(base_family)
                if base == Register.NONE and index == Register.NONE:
                    candidate = displacement
                elif base_value is not None:
                    candidate = base_value + displacement
                elif (displacement >= 0x1000 and
                      (index != Register.NONE or ins.mnemonic == Mnemonic.LEA)):
                    candidate = displacement
                else:
                    candidate = None
                if candidate is not None and is_image_rva(sections, candidate):
                    symbol_mem = candidate
                    oracle_mem = candidate
                    if index != Register.NONE:
                        memory_index = formatter.format_register(index).lower()
                        memory_scale = ins.memory_index_scale

        revenant_target = rec.get("target")
        revenant_mem = rec.get("mem")
        if revenant_mem is None:
            revenant_mem = symbol_mem
        if oracle:
            oracle_text = canon(
                oracle["asm"], near_branch=oracle_target is not None,
                ip_rel_memory=oracle_mem is not None and symbol_mem is None,
                symbol_memory=symbol_mem is not None,
                memory_index=memory_index, memory_scale=memory_scale)
            revenant_text = canon(
                rec.get("asm", ""), near_branch=revenant_target is not None,
                ip_rel_memory=revenant_mem is not None and symbol_mem is None,
                symbol_memory=symbol_mem is not None,
                memory_index=memory_index, memory_scale=memory_scale)
            ok = (revenant_text == oracle_text and
                  oracle_target == revenant_target and oracle_mem == revenant_mem)
            checked += 1
            if not ok:
                mism += 1
                mismatches.append({"rva": rva, "len": length,
                                   "revenant": rec.get("asm"),
                                   "oracle": oracle["asm"],
                                   "oracle_target": oracle_target,
                                   "oracle_mem": oracle_mem,
                                   "revenant_target": revenant_target,
                                   "revenant_mem": revenant_mem,
                                   "owner": oracle.get("owner", "?")})

        if ins is not None and ins.op0_kind == OpKind.REGISTER:
            destination = register_family(ins.op0_register, formatter)
            if destination:
                register_values.pop(destination, None)
                if (ins.mnemonic == Mnemonic.LEA and
                        ins.is_ip_rel_memory_operand):
                    register_values[destination] = ins.ip_rel_memory_address
                elif (ins.mnemonic == Mnemonic.MOV and
                      ins.op1_kind == OpKind.REGISTER):
                    source = register_family(ins.op1_register, formatter)
                    if source in register_values:
                        register_values[destination] = register_values[source]
        if ins is not None and ins.flow_control != FlowControl.NEXT:
            register_values.clear()
        next_rva = rva + length
        if owner is not None:
            last_owner = owner
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


def update_parity_doc(tag, summary):
    path = Path(__file__).resolve().parents[2] / "docs" / "parity.md"
    text = path.read_text()
    start = "<!-- parity-results:start -->"
    end = "<!-- parity-results:end -->"
    start_at = text.index(start) + len(start)
    end_at = text.index(end, start_at)
    lines = [
        f"Latest C/C++ scan (`{tag}`): {summary['binaries']} binaries, "
        f"{summary['clean']} clean; missing={summary['total_missing']}, "
        f"conflicts={summary['total_conflicts']}, "
        f"ASM mismatches={summary['total_asm_text_mismatch']}.",
        "",
        "| Binary | Status | Missing | Conflicts | Extra | ASM checked | ASM mismatch |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]

    def show(value):
        return "-" if value is None else f"{value:,}"

    for row in summary["rows"]:
        lines.append(
            f"| {row['id']} | {row['status'] or '-'} | "
            f"{show(row['missing'])} | {show(row['conflicts'])} | "
            f"{show(row['extra'])} | {show(row['asm_checked'])} | "
            f"{show(row['asm_text_mismatch'])} |")
    path.write_text(text[:start_at] + "\n" + "\n".join(lines) +
                    "\n" + text[end_at:])


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
    full_ccpp_scan = args.only is None and not args.all_kinds

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

    if full_ccpp_scan:
        update_parity_doc(args.tag, summary)
    if args.out:
        args.out.write_text(json.dumps(summary, indent=2))
        concise = {k: v for k, v in summary.items()
                   if k not in ("rows", "evidence_by_function_symbol")}
        print(json.dumps(concise, indent=2))
        print(f"full per-binary results and evidence: {args.out}")
    else:
        print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
