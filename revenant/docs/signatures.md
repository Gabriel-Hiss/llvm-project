# Signature recovery

`RevenantAnalysis` infers Win64 signatures from instruction register liveness,
stack argument reads, and caller use of return registers. It does not read PDB
information. `Signature::Params` records each incoming register or stack slot,
its widest observed read, and whether the value came from an XMM register.
Return presence and width require both a value definition on every returning
path and evidence that a direct caller reads that register after the call.

The `revenant-sigs-check` utility is an oracle-side diagnostic, not part of the
decompiler pipeline. It builds a temporary `Program` from primary `.pdata`
entries, linearly decodes each entry, runs recovery, writes
`function_va,param_count,return_kind` CSV, and compares parameter counts with
LLVM's native PDB reader. This temporary `.pdata` input has no recovered CFG,
so treat its agreement rate as a diagnostic until the normal discovery pass
feeds complete `Program` records.

```text
revenant-sigs-check <binary.exe> <matching.pdb> <signatures.csv>
```
