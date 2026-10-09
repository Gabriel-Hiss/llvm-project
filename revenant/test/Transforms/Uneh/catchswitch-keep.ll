; try/catch catchswitch survival: a complete catchswitch/catchpad/catchret
; nest under __CxxFrameHandler3 passes through unchanged (both pre-IR and
; post-IR keep it; corpus cpp-clang-o0..o3 @"?exceptions@@YAHH@Z").
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-uneh -S %s | %FileCheck %s

declare i32 @__CxxFrameHandler3(...)
declare ptr @rtti_runtime_error()
declare ptr @rtti_exception()
declare i32 @may_throw(i32)

define i32 @exceptions(i32 %x) personality ptr @__CxxFrameHandler3 {
entry:
  ; CHECK-LABEL: define i32 @exceptions(
  ; CHECK: catchswitch within none [label %c0, label %c1] unwind to caller
  ; CHECK: catchpad within %cs [ptr @rtti_runtime_error{{.*}}]
  ; CHECK: catchret from %p0 to label %merge
  ; CHECK: catchret from %p1 to label %merge
  invoke i32 @may_throw(i32 %x) to label %ok unwind label %disp, !revenant.va !0

ok:
  br label %merge, !revenant.va !1

disp:
  %cs = catchswitch within none [label %c0, label %c1] unwind to caller, !revenant.va !0

c0:
  %p0 = catchpad within %cs [ptr @rtti_runtime_error, i32 8, ptr null], !revenant.va !0
  catchret from %p0 to label %merge, !revenant.va !0

c1:
  %p1 = catchpad within %cs [ptr @rtti_exception, i32 8, ptr null], !revenant.va !0
  catchret from %p1 to label %merge, !revenant.va !0

merge:
  ret i32 0, !revenant.va !1
}

!0 = !{i64 4198400}
!1 = !{i64 4198410}
