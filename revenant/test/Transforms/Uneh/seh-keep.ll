; SEH shape survival: __try/__except under __C_specific_handler passes
; through unchanged; the filter call stays a plain opaque call (filters are
; not lifted to IR). Shape from LLVM's seh-catch-all test.
; RUN: %revenant-opt -passes=revenant-uneh -S %s | %FileCheck %s

declare i32 @__C_specific_handler(...)
declare i32 @llvm.eh.exceptioncode(token)
declare void @crash()
declare i32 @filt() nounwind
declare i32 @puts(ptr) nounwind

define i32 @seh_main() personality ptr @__C_specific_handler {
entry:
  ; CHECK-LABEL: define i32 @seh_main(
  ; CHECK: catchswitch within none [label %catchall] unwind to caller
  ; CHECK: catchpad within %cs [ptr null, i32 64, ptr null]
  ; CHECK: catchret from %p to label %cont
  invoke void @crash() to label %cont unwind label %lpad, !revenant.va !0

cont:
  ret i32 0, !revenant.va !1

lpad:
  %cs = catchswitch within none [label %catchall] unwind to caller, !revenant.va !0

catchall:
  %p = catchpad within %cs [ptr null, i32 64, ptr null], !revenant.va !0
  %code = call i32 @llvm.eh.exceptioncode(token %p), !revenant.va !0
  call i32 @puts(ptr null) [ "funclet"(token %p) ], !revenant.va !0
  catchret from %p to label %cont, !revenant.va !0
}

!0 = !{i64 4198400}
!1 = !{i64 4198410}
