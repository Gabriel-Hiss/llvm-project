; The null-check/load is from miniz-clang-o3 mz_zip_get_mode, as in cfg.ll.
; This variant keeps two returns to exercise the inliner's shared exit PHI.
; Other cases exercise proof failures, rather than claiming corpus events.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-uninline,verify -S %s | %FileCheck %s

define i32 @two_returns(ptr %p) {
entry:
  %null = icmp eq ptr %p, null
  br i1 %null, label %zero, label %load
zero:
  ret i32 0
load:
  %field = getelementptr inbounds nuw i8, ptr %p, i64 20
  %value = load i32, ptr %field, align 4
  ret i32 %value
}

; CHECK-LABEL: define i32 @merged(
; CHECK: entry:
; CHECK-NEXT: %[[CALL:[0-9]+]] = call i32 @two_returns(ptr %p)
; CHECK-NEXT: br label %follow
; CHECK: follow:
; CHECK: %result = phi i32 [ 42, %bypass ], [ %[[CALL]], %entry ]
; CHECK: %context = phi i32 [ 7, %bypass ], [ %x, %entry ]
; CHECK: ret i32 %out
define i32 @merged(ptr %p, i32 %x, i1 %skip) {
start:
  br i1 %skip, label %bypass, label %entry
bypass:
  br label %follow
entry:
  %null.i = icmp eq ptr %p, null
  br i1 %null.i, label %zero.i, label %load.i
zero.i:
  br label %follow
load.i:
  %field.i = getelementptr inbounds nuw i8, ptr %p, i64 20
  %value.i = load i32, ptr %field.i, align 4
  br label %follow
follow:
  %result = phi i32 [ 0, %zero.i ], [ 42, %bypass ], [ %value.i, %load.i ]
  %context = phi i32 [ %x, %load.i ], [ 7, %bypass ], [ %x, %zero.i ]
  %out = add i32 %result, %context
  ret i32 %out
}

; CHECK-LABEL: define i32 @wrong_return_phi(
; CHECK-NOT: call i32 @two_returns
; CHECK: phi i32 [ 1, %zero.i ], [ %value.i, %load.i ]
; CHECK: ret i32 %result
define i32 @wrong_return_phi(ptr %p) {
entry:
  %null.i = icmp eq ptr %p, null
  br i1 %null.i, label %zero.i, label %load.i
zero.i:
  br label %follow
load.i:
  %field.i = getelementptr inbounds nuw i8, ptr %p, i64 20
  %value.i = load i32, ptr %field.i, align 4
  br label %follow
follow:
  %result = phi i32 [ 1, %zero.i ], [ %value.i, %load.i ]
  ret i32 %result
}

; CHECK-LABEL: define i32 @side_exit(
; CHECK-NOT: call i32 @two_returns
; CHECK: br i1 %extra
; CHECK: ret i32 %result
define i32 @side_exit(ptr %p, i1 %extra) {
entry:
  %null.i = icmp eq ptr %p, null
  br i1 %null.i, label %zero.i, label %load.i
zero.i:
  br label %follow
load.i:
  %field.i = getelementptr inbounds nuw i8, ptr %p, i64 20
  %value.i = load i32, ptr %field.i, align 4
  br i1 %extra, label %escape, label %follow
escape:
  ret i32 99
follow:
  %result = phi i32 [ 0, %zero.i ], [ %value.i, %load.i ]
  ret i32 %result
}

; An extra exit PHI varies with the branch independently of the return.
; CHECK-LABEL: define i32 @varying_context(
; CHECK-NOT: call i32 @two_returns
; CHECK: phi i32 [ 12, %zero.i ], [ 13, %load.i ]
; CHECK: ret i32 %out
define i32 @varying_context(ptr %p) {
entry:
  %null.i = icmp eq ptr %p, null
  br i1 %null.i, label %zero.i, label %load.i
zero.i:
  br label %follow
load.i:
  %field.i = getelementptr inbounds nuw i8, ptr %p, i64 20
  %value.i = load i32, ptr %field.i, align 4
  br label %follow
follow:
  %result = phi i32 [ 0, %zero.i ], [ %value.i, %load.i ]
  %context = phi i32 [ 12, %zero.i ], [ 13, %load.i ]
  %out = add i32 %result, %context
  ret i32 %out
}
