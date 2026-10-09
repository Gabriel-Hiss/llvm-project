# Signature recovery

`RevenantAnalysis` infers Win64 signatures from instruction register liveness,
stack argument reads, and caller use of return registers. It does not read PDB
information. `Signature::Params` records each incoming register or stack slot,
its widest observed read, and whether the value came from an XMM register.
Return presence and width require both a value definition on every returning
path and evidence that a direct caller reads that register after the call.

The `revenant-sigs-check` utility is an oracle-side diagnostic, not part of the
decompiler pipeline. It builds a temporary `Program` from primary `.pdata`
entries, linearly decodes each range, and splits blocks at branch targets and
fallthroughs. It writes recovered counts and return kinds to CSV, with a fourth
`pdb_param_count` column from LLVM's CodeView procedure and member-function
type records. Entries without a matching type record are left blank. This
temporary CFG omits switch resolution and discovery outside `.pdata`; use the
normal discovery pass for the complete program once it is available.

```text
revenant-sigs-check <binary.exe> <matching.pdb> <signatures.csv>
```
