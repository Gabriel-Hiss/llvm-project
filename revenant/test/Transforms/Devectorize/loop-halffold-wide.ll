; Wide AVX2 reduction carried as i128 halves (the GCC AVX2 lowering): the
; <8 x i32> accumulator lives in two i128 PHIs, is rebuilt every iteration
; (zext/shl/or/bitcast), updated, and split back (bitcast to <2 x i128> plus
; two extractelements). The promotion carries the vector directly, the loop
; explodes into one scalar chain, and the exit fold (bitcast to <4 x i64>,
; high-half shuffle, i128 whole-lane shifts, final lane extract) aliases to
; that chain. Mirrors sum_vec in cpp-gcc-o3avx2 (0x140013C20).
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

define i32 @sum_vec(ptr %base, i64 %n) {
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
  br label %tail

tail:
  %acc = phi i32 [ %r, %exit ], [ %acc.next, %body ]
  %idx = phi i64 [ %wide, %exit ], [ %idx.next, %body ]
  %more = icmp ult i64 %idx, %n
  br i1 %more, label %body, label %ret
body:
  %p = getelementptr i32, ptr %base, i64 %idx
  %lv = load i32, ptr %p, align 4
  %acc.next = add i32 %acc, %lv
  %idx.next = add i64 %idx, 1
  %done2 = icmp eq i64 %idx.next, %n
  br i1 %done2, label %ret, label %tail
ret:
  %out = phi i32 [ %acc, %tail ], [ %acc.next, %body ]
  ret i32 %out
}

; One scalar accumulator PHI; the i128 halves and the split are gone.
; CHECK: s.unopt = phi i32 [ 0, %entry ], [ {{.*}}, %loop ]
; Eight lane adds per wide iteration.
; CHECK: add i32 {{.*}}, {{.*}}
; The exit fold is replaced by the scalar tail seed; the tail PHI now reads
; the scalar image of the fold extract.
; CHECK: %acc = phi i32 [ %s.unopt.n21, %exit ]
; CHECK-NOT: <8 x i32>
; CHECK-NOT: <4 x i32>
; CHECK-NOT: <2 x i128>
; CHECK-NOT: i128
; CHECK: ret i32
