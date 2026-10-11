; Same i128 half-carry as loop-halffold-wide.ll, but the fold extract feeds
; an exit-block add instead of a scalar tail PHI or a return. The prover has
; no scalar image for that shape, so the loop stays vector: the emitted
; source would otherwise lose the add.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

define i32 @sum_vec_used(ptr %base, i64 %n, i32 %k) {
entry:
  %wide = and i64 %n, -32
  %end = getelementptr i8, ptr %base, i64 %wide
  br label %loop

loop:
  %lo = phi i128 [ 0, %entry ], [ %e0, %loop ]
  %hi = phi i128 [ 0, %entry ], [ %e1, %loop ]
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %addr = getelementptr i8, ptr %base, i64 %iv
  %v = load <8 x i32>, ptr %addr, align 1
  %zlo = zext i128 %lo to i256
  %zhi = zext i128 %hi to i256
  %shi = shl nuw i256 %zhi, 128
  %or = or disjoint i256 %shi, %zlo
  %vec = bitcast i256 %or to <8 x i32>
  %upd = add <8 x i32> %v, %vec
  %split = bitcast <8 x i32> %upd to <2 x i128>
  %e0 = extractelement <2 x i128> %split, i64 0
  %e1 = extractelement <2 x i128> %split, i64 1
  %iv.next = add i64 %iv, 32
  %done = icmp eq i64 %iv.next, %wide
  br i1 %done, label %exit, label %loop

exit:
  %hi4 = bitcast <8 x i32> %upd to <4 x i64>
  %sh = shufflevector <4 x i64> %hi4, <4 x i64> poison, <2 x i32> <i32 2, i32 3>
  %hi32 = bitcast <2 x i64> %sh to <4 x i32>
  %lo32 = bitcast i128 %e0 to <4 x i32>
  %sum4 = add <4 x i32> %lo32, %hi32
  %bc1 = bitcast <4 x i32> %sum4 to i128
  %shift64 = lshr i128 %bc1, 64
  %bc2 = bitcast i128 %shift64 to <4 x i32>
  %half2 = add <4 x i32> %sum4, %bc2
  %bc3 = bitcast <4 x i32> %half2 to i128
  %shift32 = lshr i128 %bc3, 32
  %bc4 = bitcast i128 %shift32 to <4 x i32>
  %half1 = add <4 x i32> %half2, %bc4
  %r = extractelement <4 x i32> %half1, i64 0
  %out = add i32 %r, %k
  ret i32 %out
}

; The rebuild promoted to one wide acc PHI (that rewrite is exact), but no
; scalar chain and no fold: the extract's arithmetic use blocks the alias.
; CHECK-NOT: s.unopt
; CHECK: phi <8 x i32> [ zeroinitializer, %entry ], [ {{.*}}, %loop ]
; CHECK-NOT: phi i128
; CHECK: ret i32
