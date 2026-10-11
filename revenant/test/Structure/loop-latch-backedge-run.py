#!/usr/bin/env python3
"""Compile structured C and compare its result with lli on the input IR."""
from __future__ import annotations

import ctypes
import os
import subprocess
import sys
import tempfile
from pathlib import Path

if os.name == "nt":
    ctypes.windll.kernel32.SetErrorMode(0x8003)

MAIN_IR = r"""
define i32 @main() {
entry:
  %result = call i32 @sub_140001210(i32 10)
  %good = icmp eq i32 %result, 27
  %status = select i1 %good, i32 0, i32 1
  ret i32 %status
}
"""
MAIN_C = r"""
int sub_140001210(int);
int main(void) { return sub_140001210(10) == 27 ? 0 : 1; }
"""


def run(command: list[str], *, timeout: int = 30) -> subprocess.CompletedProcess[bytes]:
    try:
        return subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=timeout, check=False)
    except subprocess.TimeoutExpired as exc:
        raise SystemExit(f"timed out after {timeout}s: {' '.join(command)}") from exc


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: loop-latch-backedge-run.py input.ll")
    ir = Path(sys.argv[1]).resolve()
    revenant_clang = os.environ["REVENANT_CLANG"]
    clang_cl = os.environ["RV_CLANG_CL"]
    lli = os.environ["RV_LLI"]
    ir_text = ir.read_text(encoding="utf-8")
    metadata = "!llvm.module.flags ="
    if metadata not in ir_text:
        raise SystemExit("loop fixture has no module-flags insertion point")
    with tempfile.TemporaryDirectory(prefix="rv-loop-latch-") as td:
        root = Path(td)
        source = root / "emitted.c"
        driver = root / "driver.c"
        binary = root / "emitted.exe"
        lli_input = root / "lli-input.ll"
        lli_input.write_text(ir_text.replace(metadata, MAIN_IR + "\n" + metadata, 1),
                             encoding="utf-8")
        driver.write_text(MAIN_C, encoding="utf-8")
        emitted = run([revenant_clang, str(ir), "--lang=c", "-o", str(source)])
        if emitted.returncode:
            raise SystemExit(emitted.stderr.decode(errors="replace"))
        compiled = run([clang_cl, "/nologo", "/O0", "/Fe:" + str(binary),
                        str(source), str(driver)])
        if compiled.returncode:
            raise SystemExit(compiled.stderr.decode(errors="replace"))
        original = run([lli, str(lli_input)])
        candidate = run([str(binary)])
        if (original.returncode != candidate.returncode or
                original.stdout != candidate.stdout or
                original.stderr != candidate.stderr):
            raise SystemExit(
                "structured C differs from lli: "
                f"lli rc={original.returncode} stdout={original.stdout!r} "
                f"stderr={original.stderr!r}; candidate rc={candidate.returncode} "
                f"stdout={candidate.stdout!r} stderr={candidate.stderr!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
