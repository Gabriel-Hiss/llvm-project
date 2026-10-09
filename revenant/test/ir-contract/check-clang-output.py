#!/usr/bin/env python3
"""Compile every reference module and Structure shape emitted by revenant-clang.

Usage: python check-clang-output.py <revenant-clang.exe> <clang-cl.exe>
"""

import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    frontend = pathlib.Path(sys.argv[1])
    clang_cl = pathlib.Path(sys.argv[2])
    test_dir = pathlib.Path(__file__).resolve().parent
    structure_dir = test_dir.parent / "Structure"
    modules = sorted(test_dir.glob("*.ll")) + sorted(structure_dir.glob("*.ll"))
    failed = False

    with tempfile.TemporaryDirectory(prefix="revenant-clang-acceptance-") as tmp:
        out_dir = pathlib.Path(tmp)
        for ir_path in modules:
            module_name = f"{ir_path.parent.name}/{ir_path.name}"
            ir_text = ir_path.read_text(encoding="utf-8")
            is_cpp = '"revenant.lang", !"cpp"' in ir_text
            source_ext = ".cpp" if is_cpp else ".c"
            source_path = out_dir / (ir_path.stem + source_ext)
            obj_path = out_dir / (ir_path.stem + ".obj")

            emit = subprocess.run(
                [str(frontend), str(ir_path), "-o", str(source_path)],
                text=True,
                capture_output=True,
            )
            if emit.returncode:
                failed = True
                print(f"FAIL {module_name}: revenant-clang exited {emit.returncode}")
                print(emit.stderr, end="")
                continue

            mode = "/TP" if is_cpp else "/TC"
            compile_result = subprocess.run(
                [str(clang_cl), "/nologo", "/c", mode, str(source_path),
                 f"/Fo{obj_path}"],
                text=True,
                capture_output=True,
            )
            if compile_result.returncode:
                failed = True
                print(f"FAIL {module_name}: clang-cl rejected generated {source_ext}")
                print(compile_result.stdout, end="")
                print(compile_result.stderr, end="")
            else:
                print(f"PASS {module_name}: emitted and compiled ({source_ext})")

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
