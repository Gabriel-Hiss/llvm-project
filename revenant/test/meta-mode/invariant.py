#!/usr/bin/env python3
"""Compare control-flow and statements with metadata disabled and enabled."""

import argparse
import re
import random

import shutil
import subprocess
import tempfile
import sys

from pathlib import Path

BINARIES = ("c-o2", "cpp-o2", "rust-o2", "cpp-gcc-o2")
IDENT = re.compile(r'([%@])(?:"(?:[^"\\]|\\.)*"|[-A-Za-z$._0-9]+)')
METADATA = re.compile(r",?\s*![-A-Za-z0-9_.]+\s+![0-9]+")
DEBUG_RECORD = re.compile(r"^\s*#dbg_[^\n]*$", re.MULTILINE)
RELEASE_SUFFIXES = (
    "-o1", "-o2", "-o3", "-ox", "-os", "-oz", "-o2lto", "-o2gl",
    "-o2avx2", "-o2hsw",
)
TYPE = re.compile(r"\b(?:void|ptr|i[0-9]+|half|float|double|label|metadata|token)\b")
BLOCK_COMMENT = re.compile(r"; preds =.*$", re.MULTILINE)
BLOCK_LABEL = re.compile(r"(?m)^([ \t]*)bb_[^:\n]*:")



def release_targets(root: Path) -> list[str]:
    names = []
    for item in root.iterdir():
        if not item.is_dir():
            continue
        if not (item.name.endswith(RELEASE_SUFFIXES) or "pgo" in item.name.lower()):
            continue
        binary = item / ("corpus_rust.exe" if item.name.startswith("rust-")
                         else f"{item.name}.exe")
        if binary.is_file():
            names.append(item.name)
    return sorted(names)

METADATA_DEF = re.compile(r"^![A-Za-z0-9_.-]+ *=.*$", re.MULTILINE)


def normalize(ir: str) -> str:
    # Debug and named metadata are presentation-only, and so are block
    # labels (`bb_<va>`, `bb_<va>.<va>_crit_edge`, LLVM's numeric
    # suffixes on duplicate labels). Keep instructions, constants,
    # terminators, and their order intact.
    ir = METADATA_DEF.sub("", ir)
    ir = METADATA.sub("", ir)
    ir = TYPE.sub("TY", ir)
    ir = BLOCK_COMMENT.sub("", ir)
    ir = BLOCK_LABEL.sub(r"\1BB:", ir)
    ir = DEBUG_RECORD.sub("", ir)
    globals = {}
    types = {}
    output = []
    local_names = {}
    inside_function = False

    for line in ir.splitlines():
        if line.lstrip().startswith("define "):
            local_names = {}
            inside_function = True

        def neutralize(match):
            nonlocal local_names
            prefix = match.group(1)
            key = match.group(0)
            if prefix == "@":
                return globals.setdefault(key, f"@g{len(globals)}")
            if inside_function:
                return local_names.setdefault(key, f"%v{len(local_names)}")
            return types.setdefault(key, f"%t{len(types)}")

        output.append(IDENT.sub(neutralize, line).rstrip())
        if line.strip() == "}":
            inside_function = False

    return "\n".join(line for line in output if line)


def copy_driver(exe: Path, directory: Path) -> Path:
    copied = directory / exe.name
    shutil.copy2(exe, copied)
    for dll in exe.parent.glob("*.dll"):
        shutil.copy2(dll, directory / dll.name)
    return copied


def run_mode(exe: Path, binary: Path, mode: str, targets: list[str],
             output: Path, serial: int) -> str | None:
    base = output / f"{serial}-{mode}"
    command = [str(exe), str(binary), "--meta=" + mode,
               "--emit=ir-final", "-o", str(base)]
    for target in targets:
        command.extend(["--func", target])
    try:
        subprocess.run(command, check=True, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, text=True)
    except subprocess.CalledProcessError:
        return None
    return normalize(Path(str(base) + ".ir-final.ll").read_text(encoding="utf-8"))

def run_whole(exe: Path, binary: Path, mode: str) -> str | None:
    try:
        result = subprocess.run(
            [str(exe), str(binary), "--meta=" + mode,
             "--emit=ir-final", "--no-artifacts"],
            check=True, capture_output=True, text=True
        )
    except subprocess.CalledProcessError:
        return None
    return normalize(result.stdout)


def selected_functions(exe: Path, binary: Path, sample: int, seed: int) -> list[str]:
    output = subprocess.run(
        [str(exe), str(binary), "--list"],
        check=True, capture_output=True, text=True
    ).stdout
    all_functions = [line.split("\t", 1)[0] for line in output.splitlines()
                     if "\t" in line]
    return random.Random(seed).sample(all_functions, min(sample, len(all_functions)))


def compare_group(exe: Path, binary: Path, targets: list[str], output: Path,
                  counts: dict[str, int], serial: list[int]) -> None:
    serial[0] += 1
    NoneIR = run_mode(exe, binary, "none", targets, output, serial[0])
    EmbeddedIR = run_mode(exe, binary, "embedded", targets, output, serial[0])
    if NoneIR is not None and EmbeddedIR is not None and NoneIR == EmbeddedIR:
        counts["passed"] += len(targets)
        return
    if len(targets) == 1:
        if NoneIR is None and EmbeddedIR is None:
            counts["crashed_in_both"] += 1
        elif NoneIR is None or EmbeddedIR is None:
            counts["one_mode_crash"] += 1
            print(f"    {targets[0]}: crash in one mode", file=sys.stderr)
        else:
            counts["diff"] += 1
            print(f"    {targets[0]}: normalized IR differs", file=sys.stderr)
        return
    Middle = len(targets) // 2
    compare_group(exe, binary, targets[:Middle], output, counts, serial)
    compare_group(exe, binary, targets[Middle:], output, counts, serial)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--targets", nargs="*", default=BINARIES)
    parser.add_argument("--all-release", action="store_true")
    parser.add_argument("--sample", type=int, default=300)
    parser.add_argument("--seed", type=int, default=211009)
    parser.add_argument("--whole", action="store_true")
    args = parser.parse_args()
    args.exe = args.exe.resolve()
    args.corpus = args.corpus.resolve()

    with tempfile.TemporaryDirectory(prefix="revenant-meta-") as tmp:
        scratch = Path(tmp)
        driver = copy_driver(args.exe, scratch)
        targets = release_targets(args.corpus) if args.all_release else args.targets
        failures = []
        for name in targets:
            binary = args.corpus / name
            binary = binary / ("corpus_rust.exe" if name.startswith("rust-") else
                               f"{name}.exe")
            if not binary.is_file():
                print(f"{name}: FAIL missing binary {binary}", file=sys.stderr)
                failures.append(name)
                continue
            if name == "c-o2":
                default = subprocess.run(
                    [str(driver), str(binary), "--list"],
                    check=True, capture_output=True, text=True
                ).stdout
                explicit = subprocess.run(
                    [str(driver), str(binary), "--meta=embedded", "--list"],
                    check=True, capture_output=True, text=True
                ).stdout
                if default != explicit:
                    print("c-o2: FAIL embedded is not the default", file=sys.stderr)
                    failures.append(name)
                pdb = subprocess.run(
                    [str(driver), str(binary), "--meta=pdb"],
                    check=False, capture_output=True, text=True
                )
                if pdb.returncode != 2 or "not implemented yet" not in pdb.stderr:
                    print("c-o2: FAIL --meta=pdb did not report not implemented yet",
                          file=sys.stderr)
                    failures.append(name)
            if args.whole:
                none = run_whole(driver, binary, "none")
                embedded = run_whole(driver, binary, "embedded")
                if none is not None and embedded is not None and none == embedded:
                    print(f"{name}: PASS whole-binary invariant")
                else:
                    print(f"{name}: FAIL whole-binary invariant "
                          f"(none_failed={none is None}, "
                          f"embedded_failed={embedded is None}, "
                          f"diff={none is not None and embedded is not None})")
                    failures.append(name)
                continue
            functions = selected_functions(driver, binary, args.sample, args.seed)
            output = scratch / name
            output.mkdir()
            counts = {"passed": 0, "crashed_in_both": 0,
                      "one_mode_crash": 0, "diff": 0}
            compare_group(driver, binary, functions, output, counts, [0])
            print(f"{name}: sampled={len(functions)} passed={counts['passed']} "
                  f"crashed_both={counts['crashed_in_both']} "
                  f"one_mode_crash={counts['one_mode_crash']} "
                  f"diff={counts['diff']}")
            if counts["one_mode_crash"] or counts["diff"]:
                failures.append(name)
    return 1 if failures else 0
if __name__ == "__main__":
    raise SystemExit(main())
