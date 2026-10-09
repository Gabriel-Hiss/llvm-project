# C++ inline STL recovery

`revenant-uncxx` recovers optimized STL member expressions from field-level IR. It ranks candidates by module-local field layout and requires structural evidence before emitting a `revenant.cxx.*` helper. MSVC vector end and string size both use byte offset +16, so +16 alone is never enough to identify a member.

## Inline-remark evidence

The inline remarks captured by `MuseDeoptInline` provide a prioritization signal, not proof that a member's optimized body can be reconstructed. Its verified outline dataset covers 36,249 events for which the callee was deleted. Every event was unrecoverable by that pass, because these callees are thin STL/template wrappers specialized past clone-window matching. The largest C++ STL member groups in `test/Transforms/Uninline/measurements/outline/sites.csv` are string destructors (672), vector `back` (325), string `size` (323), string `c_str` (173), vector `empty` (140), string `append` (128), and vector `size` (50). These are event counts, not successful deoptimizations. They include only the outline dataset and should not be read as counts across all inline remarks.

That ranking favors field accessors and destructor paths where the caller may retain enough layout information to recover the operation. The remark data also explains why exact callee outlining is not the right recovery mechanism for these deleted wrappers. `uncxx` instead recognizes each member's inlined field/CFG shape.

## Conservative field classification

Vector empty detection requires both compared operands to be loads of the same object's +0 and +8 fields. A comparison between an iterator and a vector's +16 end field is a loop-exit test, not `vector.empty()`.

For MSVC, a string-size candidate at +16 is accepted only when the same object also has the string capacity field at +24 and lacks the vector middle pointer at +8. The `{first,last,end}` vector evidence therefore blocks misclassification as `string.size`, even if other accesses reach +24. Ambiguous layouts stay unchanged.

## Regression coverage

`test/Transforms/Uncxx/field-disambiguation.ll` checks a genuine first/last empty comparison and rejects iterator-versus-end comparisons, vector end-field `string.size` misclassification, a null-data fallback mistaken for an SSO selector, and C integer fields at offsets 0 and 8 mistaken for vector pointers. It also checks `vector.back` recovery with typed and byte-offset last-element addresses.

## Corpus coverage

The standalone pass completed on all 78 `ir-post` translation units for the
requested clang C++ corpus IDs, emitting helper calls in 34 modules. It
recovered 2,322 calls: vector data 1,355, string size 452, string data 442
(including four overload-suffixed helpers), vector push_back 39, vector empty
19, string destructor 10, vector size 3, and vector back 2. Specifically,
`cpp-clang-o2` yielded 14 calls and `json-clang-o2` yielded 809.

The inline-remark groups are source call events, while these are helper call
sites in module IR. They are not a one-to-one recall score. The 325 vector-back
remarks compare with 2 `vector.back` helper calls; the 672 string-destructor
remarks compare with 10 `string_dtor` calls. The other ranked comparisons are
140 vector-empty remarks vs 19 recovered calls and 323 string-size remarks vs
452 helper calls.

On normalized C IR, the integer-field guard prevents the false `vector.empty`
rewrite, and a `revenant.lang = "c"` module flag disables Uncxx entirely. Both
`c-o2` and `c-clang-o2` then produce no helper calls. The current `cpp-o2`
normalizer output is also tagged `"c"` by the old detector and is gated; the
C++ detection fix must land before measuring Uncxx on that binary again.

The current normalized `cpp-o2` module is also tagged `"c"` by the old
detector, so the language gate skips it. Changing only that scratch module
flag to `"cpp"` yielded one `vector.size` helper in `sub_140001630`; rerun the
actual decompiler after C++ detection lands.

The existing `revenant-decompile` accepts `--emit ir-raw|ir-clean|ir-final`,
not `--emit ir`. On `cpp-o2.exe`, `--emit ir-final` succeeded, but applying
`revenant-uncxx` to that lifted IR emitted no helper calls. On `json-cl-o2.exe`,
the driver failed LLVM verification before writing IR: `Entry block to function
must not have predecessors! label %bb_140057412`. These are observations from
the current driver build; they do not establish parity for either target.
