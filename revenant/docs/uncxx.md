# C++ inline STL recovery

`revenant-uncxx` recovers optimized STL member expressions from field-level IR. It ranks candidates by module-local field layout and requires structural evidence before emitting a `revenant.cxx.*` helper. MSVC vector end and string size both use byte offset +16, so +16 alone is never enough to identify a member.

## Inline-remark evidence

The inline remarks captured by `MuseDeoptInline` provide a prioritization signal, not proof that a member's optimized body can be reconstructed. Its verified outline dataset covers 36,249 events for which the callee was deleted. Every event was unrecoverable by that pass, because these callees are thin STL/template wrappers specialized past clone-window matching. The largest C++ STL member groups in `test/Transforms/Uninline/measurements/outline/sites.csv` are string destructors (672), vector `back` (325), string `size` (323), string `c_str` (173), vector `empty` (140), string `append` (128), and vector `size` (50). These are event counts, not successful deoptimizations. They include only the outline dataset and should not be read as counts across all inline remarks.

That ranking favors field accessors and destructor paths where the caller may retain enough layout information to recover the operation. The remark data also explains why exact callee outlining is not the right recovery mechanism for these deleted wrappers. `uncxx` instead recognizes each member's inlined field/CFG shape.

## Conservative field classification

Vector empty detection requires both compared operands to be loads of the same object's +0 and +8 fields. A comparison between an iterator and a vector's +16 end field is a loop-exit test, not `vector.empty()`.

For MSVC, a string-size candidate at +16 is accepted only when the same object also has the string capacity field at +24 and lacks the vector middle pointer at +8. The `{first,last,end}` vector evidence therefore blocks misclassification as `string.size`, even if other accesses reach +24. Ambiguous layouts stay unchanged.

## Regression coverage

`test/Transforms/Uncxx/field-disambiguation.ll` checks a genuine first/last empty comparison and rejects both an iterator-versus-end comparison and an MSVC vector end-field load as `string.size`. The test runs the standalone plugin through stock `opt` and FileCheck.
