; Merged cleanup chain recognition without tables: single cleanuppad under
; __CxxFrameHandler3 passes through unchanged (re-splitting needs the
; per-state dtor list from $stateUnwindMap/$ip2state; owner: analysis/lift).
; Corpus: cpp-clang-o2 main.post.ll @"?vector_ops@@YAHH@Z" block %56.
; RUN: %revenant-opt -passes=revenant-uneh -S %s | %FileCheck %s

declare i32 @__CxxFrameHandler3(...)
declare void @vec_dtor(ptr)
declare void @reserve(ptr, i32)

define i32 @vector_ops(i32 %n) personality ptr @__CxxFrameHandler3 {
entry:
  ; CHECK-LABEL: define i32 @vector_ops(
  ; CHECK: %pad = cleanuppad within none []
  ; CHECK: call void @vec_dtor(ptr {{.*}}) [ "funclet"(token %pad) ]
  ; CHECK: cleanupret from %pad unwind to caller
  invoke void @reserve(ptr null, i32 %n) to label %ok unwind label %padbb, !revenant.va !0

ok:
  ret i32 0, !revenant.va !1

padbb:
  %pad = cleanuppad within none [], !revenant.va !0
  call void @vec_dtor(ptr null) [ "funclet"(token %pad) ], !revenant.va !0
  cleanupret from %pad unwind to caller, !revenant.va !0
}

!0 = !{i64 4198400}
!1 = !{i64 4198410}
