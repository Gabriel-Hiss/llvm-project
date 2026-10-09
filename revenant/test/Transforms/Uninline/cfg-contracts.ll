; Controlled variants of the mz_zip_get_mode corpus shape from cfg.ll.
; Rebuilding a call must not reintroduce stronger argument/return contracts.
; RUN: %revenant-opt -passes=revenant-uninline,verify -S %s | %FileCheck %s

define i32 @nonnull_mode(ptr nonnull %p) {
entry:
  %null = icmp eq ptr %p, null
  br i1 %null, label %exit, label %load
load:
  %field = getelementptr inbounds nuw i8, ptr %p, i64 20
  %mode = load i32, ptr %field, align 4
  br label %exit
exit:
  %result = phi i32 [ %mode, %load ], [ 0, %entry ]
  ret i32 %result
}

; Null reaches a defined zero result here, unlike calling nonnull_mode.
; CHECK-LABEL: define i32 @no_contract(
; CHECK-NOT: call i32 @nonnull_mode
; CHECK: load i32
; CHECK: ret i32
define i32 @no_contract(ptr %p) {
entry:
  %null.i = icmp eq ptr %p, null
  br i1 %null.i, label %exit.i, label %load.i
load.i:
  %field.i = getelementptr inbounds nuw i8, ptr %p, i64 20
  %mode.i = load i32, ptr %field.i, align 4
  br label %exit.i
exit.i:
  %result.i = phi i32 [ %mode.i, %load.i ], [ 0, %entry ]
  ret i32 %result.i
}

; CHECK-LABEL: define i32 @same_contract(
; CHECK: call i32 @nonnull_mode(ptr %p)
; CHECK: ret i32
define i32 @same_contract(ptr nonnull %p) {
entry:
  %null.i = icmp eq ptr %p, null
  br i1 %null.i, label %exit.i, label %load.i
load.i:
  %field.i = getelementptr inbounds nuw i8, ptr %p, i64 20
  %mode.i = load i32, ptr %field.i, align 4
  br label %exit.i
exit.i:
  %result.i = phi i32 [ %mode.i, %load.i ], [ 0, %entry ]
  ret i32 %result.i
}

define range(i32 0, 4) i32 @range_mode(ptr %p) {
entry:
  %null = icmp eq ptr %p, null
  br i1 %null, label %exit, label %load
load:
  %field = getelementptr inbounds nuw i8, ptr %p, i64 24
  %mode = load i32, ptr %field, align 4, !range !0
  br label %exit
exit:
  %result = phi i32 [ %mode, %load ], [ 0, %entry ]
  ret i32 %result
}

; CHECK-LABEL: define i32 @same_range(
; CHECK: call i32 @range_mode(ptr %p)
; CHECK: ret i32
define i32 @same_range(ptr %p) {
entry:
  %null.i = icmp eq ptr %p, null
  br i1 %null.i, label %exit.i, label %load.i
load.i:
  %field.i = getelementptr inbounds nuw i8, ptr %p, i64 24
  %mode.i = load i32, ptr %field.i, align 4, !range !0
  br label %exit.i
exit.i:
  %result.i = phi i32 [ %mode.i, %load.i ], [ 0, %entry ]
  ret i32 %result.i
}

; The body lacks the callee's range guarantee, so the call is not equivalent.
; CHECK-LABEL: define i32 @missing_range(
; CHECK-NOT: call i32 @range_mode
; CHECK: load i32
; CHECK: ret i32
define i32 @missing_range(ptr %p) {
entry:
  %null.i = icmp eq ptr %p, null
  br i1 %null.i, label %exit.i, label %load.i
load.i:
  %field.i = getelementptr inbounds nuw i8, ptr %p, i64 24
  %mode.i = load i32, ptr %field.i, align 4
  br label %exit.i
exit.i:
  %result.i = phi i32 [ %mode.i, %load.i ], [ 0, %entry ]
  ret i32 %result.i
}

!0 = !{i32 0, i32 4}
