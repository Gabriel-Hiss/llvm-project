#!/usr/bin/env python3
"""Count embedded function names and validate them against oracle symbols."""

import argparse
import json
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

TARGETS = ("c-o2", "cpp-o2", "rust-o2", "cpp-gcc-o2")
PUBLIC_NAME = re.compile(r"`([^`]+)`")
MAP_LINE = re.compile(r"\b[0-9A-Fa-f]{1,4}:[0-9A-Fa-f]{4,16}\s+(.+?)\s*$")


def copy_driver(exe: Path, directory: Path) -> Path:
    copied = directory / exe.name
    shutil.copy2(exe, copied)
    for dll in exe.parent.glob("*.dll"):
        shutil.copy2(dll, directory / dll.name)
    return copied


def oracle_names(binary_dir: Path, name: str, pdbutil: Path | None) -> set[str]:
    if name in ("c-o2", "cpp-o2"):
        pdb = binary_dir / "oracle" / f"{name}.pdb"
        if not pdbutil:
            raise ValueError("--pdbutil is required for c-o2 and cpp-o2")
        out = subprocess.run(
            [str(pdbutil), "dump", "-publics", str(pdb)],
            check=True, capture_output=True, text=True
        ).stdout
        return {match.group(1) for match in PUBLIC_NAME.finditer(out)}
    map_file = binary_dir / ("corpus_rust.map" if name == "rust-o2" else "cpp-gcc-o2.map")
    result = set()
    for line in map_file.read_text(encoding="utf-8", errors="replace").splitlines():
        match = MAP_LINE.search(line)
        if match:
            result.update(SYMBOL.findall(match.group(1)))
    return result


def binary_path(root: Path, name: str) -> Path:
    return root / name / ("corpus_rust.exe" if name == "rust-o2" else f"{name}.exe")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--pdbutil", type=Path)
    parser.add_argument("--targets", nargs="*", default=TARGETS)
    args = parser.parse_args()
    args.exe = args.exe.resolve()
    args.corpus = args.corpus.resolve()
    if args.pdbutil:
        args.pdbutil = args.pdbutil.resolve()

    with tempfile.TemporaryDirectory(prefix="revenant-meta-report-") as tmp:
        driver = copy_driver(args.exe, Path(tmp))
        report = []
        for name in args.targets:
            binary_dir = args.corpus / name
            binary = binary_path(args.corpus, name)
            output = subprocess.run(
                [str(driver), str(binary), "--meta=embedded", "--list"],
                check=True, capture_output=True, text=True
            ).stdout
            decompiler_names = [line.split("\t", 2)[1] for line in output.splitlines()
                                if "\t" in line]
            recovered = {n for n in decompiler_names
                         if n != "entry" and not n.startswith("sub_")}
            oracle = oracle_names(binary_dir, name, args.pdbutil)
            validated = recovered & oracle
            report.append({
                "binary": name,
                "functions": len(decompiler_names),
                "real_names": len(recovered),
                "oracle_name_matches": len(validated),
                "unmatched_real_names": sorted(recovered - oracle),
            })
        print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
