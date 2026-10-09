# Fork parity

The `--export-parity` driver runs fresh PE analysis without loading a PDB and writes the same contract files as the Rust parity exporter: sorted `revenant-items.bin`, a whole-file `file-spans.tsv` partition, and `meta.json`. With `REVENANT_PARITY_ASM=1`, it also writes the optional `revenant-asm.jsonl` sidecar used for the ASM-text metric.

## Full C/C++ corpus

Run from any shell with the parity virtual environment installed. The runner selects every oracle-backed `native` and `gnu` target (cl, clang-cl, and MinGW GCC across available optimization levels), enables the ASM sidecar in the exporter subprocess, invokes the unmodified `compare.py`, clusters failures, and updates the per-binary table below.

```bat
C:\Users\User\RustroverProjects\Revenant\target\parity-venv\Scripts\python.exe C:\Users\User\RustroverProjects\rv-wt\Swe2ParityDriver\revenant\tools\revenant\run_parity.py --revenant-dir C:\Users\User\RustroverProjects\rv-wt\Swe2ParityDriver\build-rv\tools\revenant --parity-root C:\Users\User\RustroverProjects\Revenant\target\parity --tag luna-final --out C:\Users\User\RustroverProjects\Revenant\target\parity\summary-luna-final.json
```

Use `--only c-o2` to compare a subset. `--asm-only` recalculates the text metric from the sidecars produced by an earlier comparison. The JSON report includes up to 50 representative records per function-symbol cluster and per-binary counts.

## Metrics

- **Missing / conflicts / extra** are the boundary results from `compare.py`. `extra_on_padding` distinguishes extra instructions that overlap oracle padding runs.
- **ASM text** compares normalized Intel text for matched `(rva, len)` instruction pairs. Register and operand spelling, optional `ptr` sizes, numeric formats, equivalent x86 aliases, and relocation-derived symbol operands are normalized. Near-branch and memory references are compared by their resolved RVA identity. Oracle RIP-relative text may contain an unrelocated object literal, so those targets are decoded from the final PE with iced-x86 before comparison.
- **Clean** requires zero missing, zero conflicts, zero ASM-text mismatches, an `ok` comparison, and a present ASM sidecar.

## Per-binary results

The table is refreshed by `run_parity.py` after each scan; `summary-<tag>.json` contains the full records and owner evidence.

<!-- parity-results:start -->
No full-corpus report has been generated in this worktree yet.
<!-- parity-results:end -->
