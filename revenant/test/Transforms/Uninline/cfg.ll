; Corpus: miniz-clang-o3/artifacts/miniz.post.ll, mz_zip_get_mode.
; The callee is the actual optimized null-check/load/return-PHI shape,
; with SSA names normalized. Caller context exercises inliner splicing,
; a live prefix, a live exit suffix, and a successor PHI.
; RUN: %revenant-opt -passes=revenant-uninline,verify -S %s | %FileCheck %s

define i32 @mz_zip_get_mode(ptr %p) {
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

; CHECK-LABEL: define i32 @caller(
; CHECK: %prefix = add i32 %x, 13
; CHECK-NEXT: %[[CALL:[0-9]+]] = call i32 @mz_zip_get_mode(ptr %p), !revenant.va ![[VA:[0-9]+]]
; CHECK-NEXT: br label %uninline.cont
; CHECK-NOT: load i32
; CHECK: uninline.cont:
; CHECK-NEXT: %suffix = add i32 %[[CALL]], %prefix
; CHECK-NEXT: br label %follow
; CHECK: %out = phi i32 [ %suffix, %uninline.cont ]
define i32 @caller(ptr %p, i32 %x) {
entry:
  %prefix = add i32 %x, 13
  %null.i = icmp eq ptr %p, null, !revenant.va !0
  br i1 %null.i, label %exit.i, label %load.i
load.i:
  %field.i = getelementptr inbounds nuw i8, ptr %p, i64 20
  %mode.i = load i32, ptr %field.i, align 4
  br label %exit.i
exit.i:
  ; Incoming order can differ from the callee.
  %result.i = phi i32 [ 0, %entry ], [ %mode.i, %load.i ]
  %suffix = add i32 %result.i, %prefix
  br label %follow
follow:
  %out = phi i32 [ %suffix, %exit.i ]
  ret i32 %out
}

; CHECK-LABEL: define i32 @side_entry(
; CHECK-NOT: call i32 @mz_zip_get_mode
; CHECK: load i32
; CHECK: ret i32
define i32 @side_entry(ptr %p, i1 %extra) {
start:
  br i1 %extra, label %load.i, label %entry
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

; CHECK-LABEL: define i32 @extra_escape(
; CHECK-NOT: call i32 @mz_zip_get_mode
; CHECK: ptrtoint
; CHECK: ret i32
define i32 @extra_escape(ptr %p) {
entry:
  %null.i = icmp eq ptr %p, null
  br i1 %null.i, label %exit.i, label %load.i
load.i:
  %field.i = getelementptr inbounds nuw i8, ptr %p, i64 20
  %mode.i = load i32, ptr %field.i, align 4
  br label %exit.i
exit.i:
  %result.i = phi i32 [ %mode.i, %load.i ], [ 0, %entry ]
  %escaped = phi ptr [ %field.i, %load.i ], [ null, %entry ]
  %bits = ptrtoint ptr %escaped to i32
  %out = add i32 %result.i, %bits
  ret i32 %out
}

; CHECK: ![[VA]] = !{i64 6000}
!0 = !{i64 6000}
