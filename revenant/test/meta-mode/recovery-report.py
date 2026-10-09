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
RELEASE_SUFFIXES = (
    "-o1", "-o2", "-o3", "-ox", "-os", "-oz", "-o2lto", "-o2gl",
    "-o2avx2", "-o2hsw",
)
PUBLIC_NAME = re.compile(r"`([^`]+)`")
MAP_LINE = re.compile(r"\b[0-9A-Fa-f]{1,4}:[0-9A-Fa-f]{4,16}\s+(.+?)\s*$")
MAP_SYMBOL = re.compile(r"[?$A-Za-z_][?$A-Za-z_0-9@.$:?<>]*")


def release_targets(root: Path) -> list[str]:
    names = []
    for item in root.iterdir():
        if not item.is_dir():
            continue
        if not (item.name.endswith(RELEASE_SUFFIXES) or "pgo" in item.name.lower()):
            continue
        if binary_path(root, item.name).is_file():
            names.append(item.name)
    return sorted(names)


def copy_driver(exe: Path, directory: Path) -> Path:
    copied = directory / exe.name
    shutil.copy2(exe, copied)
    for dll in exe.parent.glob("*.dll"):
        shutil.copy2(dll, directory / dll.name)
    return copied


def oracle_names(binary_dir: Path, name: str, pdbutil: Path | None):
    Stem = "corpus_rust" if name.startswith("rust-") else name
    Oracle = binary_dir / "oracle"
    SearchDirs = (Oracle, binary_dir, binary_dir / "artifacts")
    Pdbs = [D / f"{Stem}.pdb" for D in SearchDirs]
    Maps = [D / f"{Stem}.map" for D in SearchDirs]
    if pdbutil:
        for Pdb in Pdbs:
            if Pdb.is_file():
                Text = subprocess.run(
                    [str(pdbutil), "dump", "-publics", str(Pdb)],
                    check=True, capture_output=True, text=True
                ).stdout
                return {M.group(1) for M in PUBLIC_NAME.finditer(Text)}, str(Pdb)
    for Map in Maps:
        if not Map.is_file():
            continue
        Names = set()
        for Line in Map.read_text(encoding="utf-8", errors="replace").splitlines():
            Match = MAP_LINE.search(Line)
            if Match:
                Names.update(MAP_SYMBOL.findall(Match.group(1)))
        return Names, str(Map)
    return set(), None


def binary_path(root: Path, name: str) -> Path:
    Binary = "corpus_rust.exe" if name.startswith("rust-") else f"{name}.exe"
    return root / name / Binary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--pdbutil", type=Path)
    parser.add_argument("--targets", nargs="*", default=TARGETS)
    parser.add_argument("--all-release", action="store_true")
    args = parser.parse_args()
    args.exe = args.exe.resolve()
    args.corpus = args.corpus.resolve()
    if args.pdbutil:
        args.pdbutil = args.pdbutil.resolve()

    with tempfile.TemporaryDirectory(prefix="revenant-meta-report-") as tmp:
        driver = copy_driver(args.exe, Path(tmp))
        report = []
        targets = release_targets(args.corpus) if args.all_release else args.targets
        for name in targets:
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
            oracle, oracle_file = oracle_names(binary_dir, name, args.pdbutil)
            validated = recovered & oracle
            report.append({
                "binary": name,
                "functions": len(decompiler_names),
                "real_names": len(recovered),
                "oracle_file": oracle_file,
                "oracle_name_matches": len(validated),
                "unmatched_real_names": sorted(recovered - oracle),
            })
        print(json.dumps(report, indent=2))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
