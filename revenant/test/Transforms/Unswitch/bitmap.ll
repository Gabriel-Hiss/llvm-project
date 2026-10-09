; RUN: %revenant-opt -passes=revenant-unswitch -S %s | %FileCheck %s
; Bitmap (SimplifyCFG SwitchReplacement::BitMapKind) back to a switch:
; `zext; mul by width; lshr bitmap; trunc` under a range check.
; Table i64 0x282212 (= 2,3,1,2 little-endian bytes) over [0,4).

define i32 @bitmap(i32 %c) {
; CHECK-LABEL: define i32 @bitmap(
; CHECK: switch i32 %c, label %{{.*}} [
; CHECK-DAG: i32 0, label %{{.*}}
; CHECK-DAG: i32 3, label %{{.*}}
; CHECK: ]
entry:
  %sub = add i32 %c, 0, !revenant.va !{i64 4096}
  %rc = icmp ult i32 %sub, 4, !revenant.va !{i64 4097}
  br i1 %rc, label %in, label %out

in:
  %idx = zext i32 %sub to i64
  %sh = mul i64 %idx, 8
  %down = lshr i64 2634026, %sh
  %res = trunc i64 %down to i32
  br label %join

out:
  br label %join

join:
  %p = phi i32 [ %res, %in ], [ -1, %out ], !revenant.va !{i64 4098}
  ret i32 %p
}
