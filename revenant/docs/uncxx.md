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

## Corpus coverage

Running the standalone pass on all 78 `ir-post` translation units for the
requested clang C++ corpus IDs produced helper calls in 16 modules. The pass
completed on 72 units and crashed on all 6 pugixml variants. Among successful
runs it emitted 1,965 calls: vector data 1,071, string data 422, string size
401, vector push_back 39, vector empty 19, string destructor 8, vector size
3, and vector back 2.

The remark-ranked groups are source call events, while these are all helper
call sites in module IR. They are not a one-to-one recall score. In particular,
the 325 vector-back remarks are ranked candidates, and only 2 back calls were
recovered in these runs. The 672 string-destructor remarks compare with 8
`string_dtor` helper calls. This gap is the main next recovery target.

The existing `revenant-decompile` accepts `--emit ir-raw|ir-clean|ir-final`,
not `--emit ir`. On `cpp-o2.exe`, `--emit ir-final` succeeded, but applying
`revenant-uncxx` to that lifted IR emitted no helper calls. On `json-cl-o2.exe`,
the driver failed LLVM verification before writing IR: `Entry block to function
must not have predecessors! label %bb_140057412`. These are observations from
the current driver build; they do not establish parity for either target.
