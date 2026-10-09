; Rust unwind cleanup survival: invoke + cleanuppad + funclet-bundle drop
; call + cleanupret under __CxxFrameHandler3 passes through unchanged.
; Corpus: rust-o0/artifacts/corpus_rust.pre.ll drop_glue (invoke drop,
; cleanuppad, drop_glue call with funclet bundle, cleanupret to caller).
; RUN: %revenant-opt -passes=revenant-uneh -S %s | %FileCheck %s

declare i32 @__CxxFrameHandler3(...)
declare void @drop(ptr)

define void @drop_glue(ptr %p) personality ptr @__CxxFrameHandler3 {
  ; CHECK: invoke void @drop(ptr %p)
  ; CHECK: to label %ok unwind label %padbb
  ; CHECK: %pad = cleanuppad within none []
  ; CHECK: call void @drop(ptr %p) [ "funclet"(token %pad) ]
  ; CHECK: cleanupret from %pad unwind to caller
  invoke void @drop(ptr %p) to label %ok unwind label %padbb, !revenant.va !0

ok:
  ret void, !revenant.va !1

padbb:
  %pad = cleanuppad within none [], !revenant.va !0
  call void @drop(ptr %p) [ "funclet"(token %pad) ], !revenant.va !0
  cleanupret from %pad unwind to caller, !revenant.va !0
}

!0 = !{i64 4198400}
!1 = !{i64 4198410}
