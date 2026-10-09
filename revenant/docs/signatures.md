# Signature recovery

`RevenantAnalysis` infers Win64 signatures from instruction register liveness,
stack argument reads, and caller use of return registers. It does not read PDB
information. `Signature::Params` records each incoming register or stack slot,
its widest observed read, and whether the value came from an XMM register.
Return presence and width require both a value definition on every returning
path and evidence that a direct caller reads that register after the call.

`revenant-sigs-check` is an oracle-side diagnostic, not part of the
decompiler pipeline. It runs the same `Program::analyze` discovery path used
by the driver, then compares recovered parameter counts with LLVM CodeView
procedure and member-function records. It writes per-function results to CSV.
Entries without a matching type record are left blank.

When a static-library database is supplied, exact relocation-masked matches
replace inferred names and prototypes. MSVC member prototypes include an
explicit leading `this` pointer, matching the parameter counts stored in PDB
member-function records.

Varargs detection requires all four Win64 register-home spills plus evidence
that a pointer to the home area is consumed through a `va_list`, such as an
indirect read or use as a register or stack argument to a call. Ordinary
argument homing is not sufficient. Straight-line direct tail jumps propagate
target parameters when register moves map them to incoming Win64 argument
registers; caller-prefix evidence is used separately for direct callers.

```text
revenant-sigs-check <binary.exe> <matching.pdb> <signatures.csv>
```
