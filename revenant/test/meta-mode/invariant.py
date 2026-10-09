#!/usr/bin/env python3
"""Compare control-flow and statements with metadata disabled and enabled."""

import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

BINARIES = ("c-o2", "cpp-o2", "rust-o2", "cpp-gcc-o2")
IDENT = re.compile(r'([%@])(?:"(?:[^"\\]|\\.)*"|[-A-Za-z$._0-9]+)')
METADATA = re.compile(r",?\s*![-A-Za-z0-9_.]+\s+![0-9]+")
METADATA_DEF = re.compile(r"^![A-Za-z0-9_.-]+ *=.*$", re.MULTILINE)


def normalize(ir: str) -> str:
    # Debug and named metadata are presentation-only. Keep instructions,
    # constants, terminators, and their order intact.
    ir = METADATA_DEF.sub("", ir)
    ir = METADATA.sub("", ir)
    names = {}

    def neutralize(match):
        prefix, _ = match.groups()
        key = match.group(0)
        if key not in names:
            names[key] = f"{prefix}{len(names)}"
        return names[key]

    ir = IDENT.sub(neutralize, ir)
    return "\n".join(line.rstrip() for line in ir.splitlines() if line.strip())


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
    args = parser.parse_args()
    args.exe = args.exe.resolve()
    args.corpus = args.corpus.resolve()

    with tempfile.TemporaryDirectory(prefix="revenant-meta-") as tmp:
        scratch = Path(tmp)
        driver = copy_driver(args.exe, scratch)
        for name in BINARIES:
            binary = args.corpus / name
            binary = binary / ("corpus_rust.exe" if name == "rust-o2" else
                               "cpp-gcc-o2.exe" if name == "cpp-gcc-o2" else
                               f"{name}.exe")
            if not binary.is_file():
                raise FileNotFoundError(binary)
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
                    raise SystemExit("embedded is not the default metadata mode")
                pdb = subprocess.run(
                    [str(driver), str(binary), "--meta=pdb"],
                    check=False, capture_output=True, text=True
                )
                if pdb.returncode != 2 or "not implemented yet" not in pdb.stderr:
                    raise SystemExit("--meta=pdb did not report not implemented yet")
            output = scratch / name
            output.mkdir()
            none = normalize(run_mode(driver, binary, "none", output))
            embedded = normalize(run_mode(driver, binary, "embedded", output))
            if none != embedded:
                raise SystemExit(f"metadata changed statements or control flow for {name}")
            print(f"{name}: invariant")
    return 0
if __name__ == "__main__":
    raise SystemExit(main())
