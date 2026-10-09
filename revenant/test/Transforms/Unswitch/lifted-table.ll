; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUnswitch.dll -passes=revenant-unswitch -S %s | FileCheck %s
; Lifted lookup table: the compiler's switch table survives as a constant
; byte array global (g_*) addressed with a typed GEP, guarded by a `ugt`
; range check. Corpus: c-clang-o2 @switch_twolevel (sub_140001270),
; 20 i32 entries 100/200/300/400, default -1.
; Also the lifted linear map: the multiply runs on the raw condition, not
; the offset (c-clang-o2 @switch_offset, sub_140001250).

@g_140070050 = internal constant [80 x i8] c"d\00\00\00d\00\00\00d\00\00\00d\00\00\00d\00\00\00\C8\00\00\00\C8\00\00\00\C8\00\00\00\C8\00\00\00\C8\00\00\00,\01\00\00,\01\00\00,\01\00\00,\01\00\00,\01\00\00\90\01\00\00\90\01\00\00\90\01\00\00\90\01\00\00\90\01\00\00", align 4

define i32 @twolevel_lifted(i32 %0) {
; CHECK-LABEL: define i32 @twolevel_lifted(
; CHECK: switch i32 %0, label %[[DEF:.*]] [
; CHECK-DAG: i32 0, label %[[C100:.*]]
; CHECK-DAG: i32 4, label %[[C100]]
; CHECK-DAG: i32 5, label %[[C200:.*]]
; CHECK-DAG: i32 9, label %[[C200]]
; CHECK-DAG: i32 10, label %[[C300:.*]]
; CHECK-DAG: i32 14, label %[[C300]]
; CHECK-DAG: i32 15, label %[[C400:.*]]
; CHECK-DAG: i32 19, label %[[C400]]
; CHECK: ]
; CHECK-NOT: load i32
; CHECK: phi i32
; CHECK-DAG: [ -1, %{{.*}} ]
head:
  %c = icmp ugt i32 %0, 19, !revenant.va !0
  br i1 %c, label %join, label %lookup, !revenant.va !1

lookup:
  %z = zext nneg i32 %0 to i64
  %element = getelementptr i32, ptr @g_140070050, i64 %z, !revenant.va !2
  %1 = load i32, ptr %element, align 1, !revenant.va !2
  br label %join, !revenant.va !2

join:
  %r = phi i32 [ -1, %head ], [ %1, %lookup ]
  ret i32 %r, !revenant.va !0
}

define i32 @offset_lifted(i64 %0) {
; CHECK-LABEL: define i32 @offset_lifted(
; CHECK: switch i32 %{{.*}}, label %[[TAIL:.*]] [
; CHECK-DAG: i32 3, label %[[C3:.*]]
; CHECK-DAG: i32 12, label %[[C12:.*]]
; CHECK: ]
; CHECK: phi i32
; CHECK-DAG: [ -1, %{{[A-Za-z0-9_.]+}} ]
; CHECK-DAG: [ 30, %[[C3]] ]
; CHECK-DAG: [ 120, %[[C12]] ]
  %t = trunc i64 %0 to i32
  br label %entry

entry:
  %narrow = add i32 %t, -3, !revenant.va !3
  %narrow39 = mul i32 %t, 10
  %c = icmp ult i32 %narrow, 10, !revenant.va !4
  %2 = select i1 %c, i32 %narrow39, i32 -1, !revenant.va !4
  ret i32 %2, !revenant.va !5
}

!0 = !{i64 5369379312}
!1 = !{i64 5369379313}
!2 = !{i64 5369379314}
!3 = !{i64 5369379315}
!4 = !{i64 5369379316}
!5 = !{i64 5369379317}
