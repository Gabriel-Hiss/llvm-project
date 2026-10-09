# Unabi measurement

Pass: `revenant-unabi` (branch `rv/MuseDeoptAbi`). Method: corpus census of
ir-pre ABI shapes + targeted fire/no-fire probes through the built plugin
(`opt -load-pass-plugin=...RevenantTransformUnabi.dll -passes=revenant-unabi
-verify-each`). Every probe below was executed; counts come from scanning
all 219 `*.pre.ll` files (57176 defines).

## Corpus shape census (ir-pre, all clang targets)

| construct | count | where |
|---|---|---|
| sret functions | 5572 | yaml (up to 748), fmt (370 x7), spdlog (287), pugi (151 x7), json (252), audio-nodegraph (37 x5), minecalc, rust, tinyxml2/cpp (a few) |
| ctor/dtor returning `ptr` (`??0`/`??1`) | 6169 ctors, 2749 dtors | yaml (1379 at o0), spdlog (479), fmt (258 x8), pugi (134 x7) |
| varargs (`...`) | 239 | lua (30), zlib (15 x3), yaml, tinyxml2, c (3 x7), fmt, md4c, miniz, pugi, cpp, spdlog, json |
| narrow candidates (i8/i16/i32/float in signature) | 16169 | everywhere |
| `byval` attributes | 0 | (only hit is a function *named* `clearbyvalues`) |
| RAX-coerced small-struct returns | rare | `?inputPortPos` (PortPos{float,float} -> i64), audio-nodegraph |

## Fire / bail per construct (probe-verified)

| construct | fires | bails | bail reason (all deliberate guards) |
|---|---|---|---|
| integer/float narrowing (r32/f32/ptr/...) | yes | — | positional rule only (arity mismatch bails, tested) |
| `this` receiver (T) | yes | — | |
| varargs `...` | yes | — | `...` must be last; already-vararg bails |
| ctor/dtor this-return (t) | yes | broken shape bails (tested: `notctor` returns null) | every return must hand back param 0 |
| sret, plain-store slot | yes | — | |
| sret, memcpy/memset destination slot | yes (new) | — | added: 1300/5393 sret functions (24%) use memcpy-dst |
| sret, slot as memcpy source / slot loads / calls on slot | no | correct bail | would read or escape the slot |
| sret forwarded to nested sret call (fmt `format_to`) | no | correct bail | needs callee-aware analysis, out of scope |
| pair (RAX-coerced struct) | yes | width mismatch bails | store size must equal old return width |
| V (struct-by-pointer) | yes, as plain ptr | unknown struct name bails | ir-pre has no byval marker; ownership stays with sigs (open with MuseSigs2) |
| musttail/callbr/indirect/pre-existing sret | no | correct bail (all tested in nofire.ll) | would break tail guarantees / unknown call sites |

Sret fire estimate from the census: plain-store 2691 + memcpy-dst 1300 of
5393 sret functions = 74.0% invert fully; the rest forward the slot to
another call (fmt-style, 1190) or genuinely read it (212) and stay bailed
by design. Narrowing/this/ctor/varargs fire on every well-formed
annotation; the 97% figure from the prior sweep matches: the remaining ~3%
are the guard bails above plus typo'd annotations.

## Tests (10 lit tests, all passing)

passthrough, narrow, this, sret, sret-memcpy (new), pair, byval,
varargs-ctor, nofire, corpus-e2e (new: real ?indexOf/ctor/addNode shapes
from audio-nodegraph-clang-o1).
