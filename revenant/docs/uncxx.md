# C++ inline STL recovery

`revenant-uncxx` recovers optimized STL member expressions from field-level IR. It ranks candidates by module-local field layout and requires structural evidence before emitting a `revenant.cxx.*` helper. MSVC vector end and string size both use byte offset +16, so +16 alone is never enough to identify a member.

## Inline-remark evidence

The inline remarks captured by `MuseDeoptInline` provide a prioritization signal, not proof that a member's optimized body can be reconstructed. Its verified outline dataset covers 36,249 events for which the callee was deleted. Every event was unrecoverable by that pass, because these callees are thin STL/template wrappers specialized past clone-window matching. The largest C++ STL member groups in `test/Transforms/Uninline/measurements/outline/sites.csv` are string destructors (672), vector `back` (325), string `size` (323), string `c_str` (173), vector `empty` (140), string `append` (128), and vector `size` (50). These are event counts, not successful deoptimizations. They include only the outline dataset and should not be read as counts across all inline remarks.

That ranking favors field accessors and destructor paths where the caller may retain enough layout information to recover the operation. The remark data also explains why exact callee outlining is not the right recovery mechanism for these deleted wrappers. `uncxx` instead recognizes each member's inlined field/CFG shape.

## Conservative field classification

Vector empty detection requires both compared operands to load the same object's +0 and +8 fields. An iterator-to-end comparison is a loop-exit test, not `vector.empty()`.

For MSVC, accept a string-size candidate at +16 only when the same object also has string capacity at +24 and lacks the vector middle pointer at +8. Ambiguous layouts stay unchanged.
## Rewrite eligibility

`revenant-uncxx` runs only on functions tagged `"revenant.user"="true"`. A field layout alone is not enough: the pass also needs a call on that same object to a function tagged with a libdb-derived `"revenant.lib"` name. It resolves the call target after `stripPointerCastsAndAliases()` and reads the attribute from the callee `Function`, not the call site. The attributes survive every metadata mode.


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

## Current corpus check

The `cpp-o2` image classifies both source functions `vector_ops` and `sum_vec<int>` as user functions. The IR after cleanup retains those user tags and the libdb-derived library tags. Running static `revenant-opt -passes=revenant-uncxx` on that IR produced no helper calls. The user functions contain no direct calls to a libdb-identified vector or string member on their container object, so the pass correctly rejects their layout-only candidates under the evidence rule.

The clean IR measurement is saved under `rv-scratch/LunaUncxxUser/cpp-o2.ir-clean.ll`; the static pass output is `cpp-o2.ir-uncxx.ll`. The full decompiler's `--emit ir-final` run still reports an unrelated `revenant-ssaout` verification failure for nonnull/noundef metadata, after a duplicate lifted-function failure at `0x140062f70`. The `ir-clean` stage and static Uncxx pass complete.
