; SSA-out keeps exceptional dispatch and catchret edges in their EH blocks.
; RUN: %revenant-opt -passes=revenant-ssaout,verify -S %s | %FileCheck %s

; CHECK-LABEL: define i32 @catch_phi
; CHECK: catchswitch within none [label %catch] unwind to caller
; CHECK: catch:
; CHECK-NEXT: %pad = catchpad
; CHECK-NEXT: store i32 7, ptr %v_value
; CHECK: catchret from %pad to label %normal
; CHECK-LABEL: define i64 @invoke_result_copy
; CHECK: invoke ptr @may_alloc()
; CHECK: store ptr %result, ptr %v_result

declare i32 @may_throw()
declare i32 @__CxxFrameHandler3(...)

define i32 @catch_phi() personality ptr @__CxxFrameHandler3 {
entry:
  %result = invoke i32 @may_throw() to label %normal unwind label %dispatch

dispatch:
  %switch = catchswitch within none [label %catch] unwind to caller

catch:
  %caught = phi i32 [ 7, %dispatch ]
  %pad = catchpad within %switch [ptr null, i32 0, ptr null]
  catchret from %pad to label %normal

normal:
  %value = phi i32 [ %result, %entry ], [ %caught, %catch ]
  ret i32 %value
}

declare ptr @may_alloc()

define i64 @invoke_result_copy() personality ptr @__CxxFrameHandler3 {
entry:
  %result = invoke ptr @may_alloc() to label %normal unwind label %dispatch

dispatch:
  %switch = catchswitch within none [label %catch] unwind to caller

catch:
  %pad = catchpad within %switch [ptr null, i32 0, ptr null]
  catchret from %pad to label %normal

normal:
  %first = phi ptr [ %result, %entry ], [ null, %catch ]
  %second = phi ptr [ %result, %entry ], [ null, %catch ]
  %tag = phi i32 [ 0, %entry ], [ 1, %catch ]
  %pointer1 = ptrtoint ptr %first to i64
  %pointer2 = ptrtoint ptr %second to i64
  %sum = add i64 %pointer1, %pointer2
  %widened = sext i32 %tag to i64
  %answer = add i64 %sum, %widened
  ret i64 %answer
}
