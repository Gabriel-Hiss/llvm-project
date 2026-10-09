#!/usr/bin/env python3
"""Compare control-flow and statements with metadata disabled and enabled."""

import argparse
import re
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
    # Debug and named metadata are presentation-only. Keep instructions,
    # constants, terminators, and their order intact.
    ir = METADATA_DEF.sub("", ir)
    ir = METADATA.sub("", ir)
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


def run_mode(exe: Path, binary: Path, mode: str, output: Path) -> str:
    base = output / mode
    subprocess.run(
        [str(exe), str(binary), "--meta=" + mode, "--emit=ir-final", "-o", str(base)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return Path(str(base) + ".ir-final.ll").read_text(encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--targets", nargs="*", default=BINARIES)
    parser.add_argument("--all-release", action="store_true")
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
            output = scratch / name
            output.mkdir()
            try:
                none = normalize(run_mode(driver, binary, "none", output))
                embedded = normalize(run_mode(driver, binary, "embedded", output))
            except subprocess.CalledProcessError as Error:
                Details = [
                    Line for Line in Error.stderr.splitlines()
                    if "Revenant stage=" in Line or "Exception Code:" in Line
                ]
                print(f"{name}: FAIL process exit {Error.returncode} "
                      f"({' ; '.join(Details)})", file=sys.stderr)
                failures.append(name)
                continue
            if none != embedded:
                print(f"{name}: FAIL metadata changed statements or control flow",
                      file=sys.stderr)
                failures.append(name)
                continue
            print(f"{name}: PASS invariant")
    return 1 if failures else 0
if __name__ == "__main__":
    raise SystemExit(main())
