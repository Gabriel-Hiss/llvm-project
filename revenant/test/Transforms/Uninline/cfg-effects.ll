; Corpus: miniz-clang-o3/artifacts/miniz.post.ll, mz_zip_set_last_error.
; Actual null-check/load/store/return-PHI body, with normalized SSA names.
; Caller contexts and metadata are controlled proof/provenance regressions.
; RUN: %revenant-opt -passes=revenant-uninline,verify -S %s | %FileCheck %s

define i32 @mz_zip_set_last_error(ptr %p, i32 %error) {
entry:
  %null = icmp eq ptr %p, null
  br i1 %null, label %exit, label %set
set:
  %field = getelementptr inbounds nuw i8, ptr %p, i64 28
  %previous = load i32, ptr %field, align 4
  store i32 %error, ptr %field, align 4
  br label %exit
exit:
  %result = phi i32 [ %previous, %set ], [ 24, %entry ]
  ret i32 %result
}

; CHECK-LABEL: define i32 @effects(
; CHECK: store i32 11, ptr %other
; CHECK-NEXT: %[[CALL:[0-9]+]] = call i32 @mz_zip_set_last_error(ptr %p, i32 %error)
; CHECK-NEXT: br label %uninline.cont
; CHECK: uninline.cont:
; CHECK-NEXT: store i32 12, ptr %other
; CHECK-NEXT: ret i32 %[[CALL]]
define i32 @effects(ptr %p, ptr %other, i32 %error) {
entry:
  store i32 11, ptr %other
  %null.i = icmp eq ptr %p, null
  br i1 %null.i, label %exit.i, label %set.i
set.i:
  %field.i = getelementptr inbounds nuw i8, ptr %p, i64 28
  %previous.i = load i32, ptr %field.i, align 4
  store i32 %error, ptr %field.i, align 4
  br label %exit.i
exit.i:
  %result.i = phi i32 [ %previous.i, %set.i ], [ 24, %entry ]
  store i32 12, ptr %other
  ret i32 %result.i
}

; Debug loc occurs before the first VA: preserve both, independently.
; CHECK-LABEL: define i32 @provenance(
; CHECK: call i32 @mz_zip_set_last_error(ptr %p, i32 %error), !dbg ![[LOC:[0-9]+]], !revenant.va ![[VA:[0-9]+]]
; CHECK: ret i32
define i32 @provenance(ptr %p, i32 %error) !dbg !4 {
entry:
  %null.i = icmp eq ptr %p, null, !dbg !7
  br i1 %null.i, label %exit.i, label %set.i, !revenant.va !8
set.i:
  %field.i = getelementptr inbounds nuw i8, ptr %p, i64 28
  %previous.i = load i32, ptr %field.i, align 4
  store i32 %error, ptr %field.i, align 4
  br label %exit.i
exit.i:
  %result.i = phi i32 [ %previous.i, %set.i ], [ 24, %entry ]
  ret i32 %result.i
}

; A write inserted inside the cloned body must not move across the call.
; CHECK-LABEL: define i32 @extra_write(
; CHECK-NOT: call i32 @mz_zip_set_last_error
; CHECK: store i32 99
; CHECK: ret i32
define i32 @extra_write(ptr %p, ptr %other, i32 %error) {
entry:
  %null.i = icmp eq ptr %p, null
  br i1 %null.i, label %exit.i, label %set.i
set.i:
  %field.i = getelementptr inbounds nuw i8, ptr %p, i64 28
  %previous.i = load i32, ptr %field.i, align 4
  store i32 99, ptr %other
  store i32 %error, ptr %field.i, align 4
  br label %exit.i
exit.i:
  %result.i = phi i32 [ %previous.i, %set.i ], [ 24, %entry ]
  ret i32 %result.i
}

; CHECK-LABEL: define i32 @annotated(
; CHECK-NOT: call i32 @mz_zip_set_last_error
; CHECK: br i1 %null.i{{.*}}!revenant.region
; CHECK: ret i32
define i32 @annotated(ptr %p, i32 %error) {
entry:
  %null.i = icmp eq ptr %p, null
  br i1 %null.i, label %exit.i, label %set.i, !revenant.region !9
set.i:
  %field.i = getelementptr inbounds nuw i8, ptr %p, i64 28
  %previous.i = load i32, ptr %field.i, align 4
  store i32 %error, ptr %field.i, align 4
  br label %exit.i
exit.i:
  %result.i = phi i32 [ %previous.i, %set.i ], [ 24, %entry ]
  ret i32 %result.i
}

; CHECK: ![[LOC]] = !DILocation(line: 42, column: 3
; CHECK: ![[VA]] = !{i64 8192}
!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3}
!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "corpus", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "miniz.c", directory: "/corpus")
!2 = !{i32 2, !"Dwarf Version", i32 4}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = distinct !DISubprogram(name: "provenance", scope: !1, file: !1, line: 40, type: !5, scopeLine: 40, spFlags: DISPFlagDefinition, unit: !0)
!5 = !DISubroutineType(types: !6)
!6 = !{}
!7 = !DILocation(line: 42, column: 3, scope: !4)
!8 = !{i64 8192}
!9 = !{!"if", i32 2}
