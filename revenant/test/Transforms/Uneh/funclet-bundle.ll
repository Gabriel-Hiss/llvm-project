; Funclet bundle repair: a dtor call inside a single-cleanuppad block that
; lost its [ "funclet"(token %pad) ] bundle gets it back. Corpus:
; target/parity/cpp-clang-o1/artifacts/main.pre.ll
; @"?string_ops@@YA_KPEBD@Z" block %11: cleanuppad + dtor call with bundle.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-uneh -S %s | %FileCheck %s

declare i32 @__CxxFrameHandler3(...)
declare void @dtor(ptr)
declare void @may_throw(ptr)
define i64 @string_ops(ptr %in) personality ptr @__CxxFrameHandler3 {
entry:
  ; CHECK-LABEL: define i64 @string_ops(
  br label %body, !revenant.va !0

body:
  ; The dtor call gains the funclet bundle naming this block's pad.
  ; CHECK: %pad = cleanuppad within none []
  ; CHECK: call void @dtor(ptr %in) [ "funclet"(token %pad) ]
  invoke void @may_throw(ptr %in) to label %ok unwind label %padbb, !revenant.va !1

ok:
  ret i64 0, !revenant.va !2

padbb:
  %pad = cleanuppad within none [], !revenant.va !1
  call void @dtor(ptr %in), !revenant.va !1
  cleanupret from %pad unwind to caller, !revenant.va !1
}

!0 = !{i64 4198400}
!1 = !{i64 4198405}
!2 = !{i64 4198410}
