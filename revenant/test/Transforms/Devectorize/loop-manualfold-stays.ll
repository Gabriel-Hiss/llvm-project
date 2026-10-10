; Manual i128 horizontal fold with no llvm.vector.reduce consumer must NOT
; fire: scalarizing the body would leave the exit fold feeding severed vector
; PHIs (undef backedges) while the scalar chains feed nothing (WRONG CODE).
; Reduced from MSVC c-o2 sum_vec (deopt-board/work/pre-c-o2.ll#sub_140001B60):
; two <4 x i32> accumulators, exit combine + i128 lshr/bitcast/add fold.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

define i32 @msvc_sum_manual_fold(ptr %a, i32 %n) {
entry:
  %guard = icmp slt i32 %n, 1
  br i1 %guard, label %ret, label %pre

pre:
  %small = icmp ult i32 %n, 8
  br i1 %small, label %tail, label %setup

setup:
  %wide = and i32 %n, 2147483640
  br label %loop

loop:
  %acc.a = phi <4 x i32> [ zeroinitializer, %setup ], [ %next.a, %loop ]
  %acc.b = phi <4 x i32> [ zeroinitializer, %setup ], [ %next.b, %loop ]
  %iv = phi i32 [ 0, %setup ], [ %iv.next, %loop ]
  %ptr.a = getelementptr i32, ptr %a, i32 %iv
  %vec.a = load <4 x i32>, ptr %ptr.a, align 4
  %next.b = add <4 x i32> %acc.b, %vec.a
  %ptr.b = getelementptr <4 x i32>, ptr %ptr.a, i64 1
  %vec.b = load <4 x i32>, ptr %ptr.b, align 4
  %next.a = add <4 x i32> %acc.a, %vec.b
  %iv.next = add i32 %iv, 8
  %done = icmp slt i32 %iv.next, %wide
  br i1 %done, label %loop, label %fold

fold:
  %m = add <4 x i32> %next.a, %next.b
  %bc0 = bitcast <4 x i32> %m to i128
  %sh0 = lshr i128 %bc0, 64
  %bc1 = bitcast i128 %sh0 to <4 x i32>
  %h1 = add <4 x i32> %m, %bc1
  %bc2 = bitcast <4 x i32> %h1 to i128
  %sh1 = lshr i128 %bc2, 32
  %bc3 = bitcast i128 %sh1 to <4 x i32>
  %h2 = add <4 x i32> %h1, %bc3
  %r = extractelement <4 x i32> %h2, i64 0
  br label %tail

tail:
  %res = phi i32 [ 0, %pre ], [ %r, %fold ]
  br label %ret

ret:
  %out = phi i32 [ 0, %entry ], [ %res, %tail ]
  ret i32 %out
}

; The loop stays fully vector; no scalar accumulator is created.
; CHECK: %acc.a = phi <4 x i32>
; CHECK: %acc.b = phi <4 x i32>
; CHECK: %m = add <4 x i32> %next.a, %next.b
; CHECK: extractelement <4 x i32> %h2, i64 0
; CHECK-NOT: s.unopt
; CHECK: ret i32
