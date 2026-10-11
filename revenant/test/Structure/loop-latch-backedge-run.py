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

def wrapper_ir(function: str, arguments: list[int], expected: int,
               return_type: str) -> str:
    operands = ", ".join(f"i32 {value}" for value in arguments)
    if return_type == "float":
        return (
            "define i32 @main() {\n"
            "entry:\n"
            f"  %unused = call float @{function}({operands})\n"
            "  ret i32 0\n"
            "}\n"
        )
    return (
        "define i32 @main() {\n"
        "entry:\n"
        f"  %result = call i32 @{function}({operands})\n"
        f"  %good = icmp eq i32 %result, {expected}\n"
        "  %status = select i1 %good, i32 0, i32 1\n"
        "  ret i32 %status\n"
        "}\n"
    )


def wrapper_c(function: str, arguments: list[int], expected: int,
              return_type: str) -> str:
    parameters = ", ".join("int" for _ in arguments) or "void"
    arguments_text = ", ".join(str(value) for value in arguments)
    if return_type == "float":
        return (
            f"float {function}({parameters});\n"
            f"int main(void) {{ (void){function}({arguments_text}); return 0; }}\n"
        )
    return (
        f"int {function}({parameters});\n"
        f"int main(void) {{ return {function}({arguments_text}) == {expected} ? 0 : 1; }}\n"
    )


def run(command: list[str], *, timeout: int = 30) -> subprocess.CompletedProcess[bytes]:
    try:
        return subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=timeout, check=False)
    except subprocess.TimeoutExpired as exc:
        raise SystemExit(f"timed out after {timeout}s: {' '.join(command)}") from exc


def main() -> int:
    if len(sys.argv) < 3:
        raise SystemExit("usage: loop-latch-backedge-run.py input.ll function ...")
    ir = Path(sys.argv[1]).resolve()
    entry_mode = sys.argv[2] == "--entry"
    arguments: list[int] = []
    expected = 0
    return_type = "i32"
    if entry_mode:
        if len(sys.argv) != 3:
            raise SystemExit("--entry does not take function arguments")
    else:
        if len(sys.argv) < 4:
            raise SystemExit(
                "usage: loop-latch-backedge-run.py input.ll function [float] [argument ...] expected")
        function = sys.argv[2]
        values_text = sys.argv[3:]
        if values_text and values_text[0] == "float":
            return_type = values_text.pop(0)
        if not values_text:
            raise SystemExit("expected result is required")
        try:
            values = [int(value) for value in values_text]
        except ValueError as exc:
            raise SystemExit("arguments and expected result must be integers") from exc
        arguments, expected = values[:-1], values[-1]
    revenant_clang = os.environ["REVENANT_CLANG"]
    clang_cl = os.environ["RV_CLANG_CL"]
    lli = os.environ["RV_LLI"]
    ir_text = ir.read_text(encoding="utf-8")
    metadata = "!llvm.module.flags ="
    if not entry_mode and metadata not in ir_text:
        raise SystemExit("loop fixture has no module-flags insertion point")
    with tempfile.TemporaryDirectory(prefix="rv-loop-latch-") as td:
        root = Path(td)
        source = root / "emitted.c"
        driver = root / "driver.c"
        binary = root / "emitted.exe"
        lli_input = root / "lli-input.ll"
        if entry_mode:
            lli_input.write_text(ir_text, encoding="utf-8")
        else:
            main_ir = wrapper_ir(function, arguments, expected, return_type)
            lli_input.write_text(
                ir_text.replace(metadata, main_ir + "\n" + metadata, 1),
                encoding="utf-8")
            driver.write_text(
                wrapper_c(function, arguments, expected, return_type),
                encoding="utf-8")
        emitted = run([revenant_clang, str(ir), "--lang=c", "-o", str(source)])
        if emitted.returncode:
            raise SystemExit(emitted.stderr.decode(errors="replace"))
        compile_command = [clang_cl, "/nologo", "/O0", "/Fe:" + str(binary),
                           str(source)]
        if not entry_mode:
            compile_command.append(str(driver))
        compiled = run(compile_command)
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
