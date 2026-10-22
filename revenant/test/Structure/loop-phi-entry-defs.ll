; REQUIRES: revenant-clang
; RUN: %revenant-clang %s 2>&1 | %FileCheck %s
; Regression fixture for the emitter's if/loop header work.
;
; Reduced from cpp-clang-o2avx2 sum_vec (VA 0x1400016E0, lifted by the tip).
; The shape is the frontend's post-SsaOut IR: allocas in the entry block,
; loads in the loop header, the arithmetic there too, and the backedge
; stores in a separate latch block.
;
; The tip's emitIf dropped the header's non-terminator instructions, so the
; entry block's loads/geps never reached the TU and both tests read
; unassigned temporaries. FeedsCond likewise dropped definitions the
; emitted test does not actually fold inline. Both must now appear above
; the tests, and the loop's IV step plus the accumulator backedge stores
; must be assigned inside the loop instead of being replaced by zero
; literals and the bound constant.
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i32 @sub_1400016E0(ptr %0) {
entry:
  %base = alloca ptr, align 8
  %acc0 = alloca i128, align 16
  %acc1 = alloca i128, align 16
  %rax = alloca i64, align 8
  %1 = load ptr, ptr %0, align 1
  store ptr %1, ptr %base, align 8
  %field.bits = ptrtoint ptr %1 to i64
  %2 = load i64, ptr %1, align 1
  %empty = icmp eq i64 %2, %field.bits
  br i1 %empty, label %done, label %check

check:
  %3 = sub i64 %2, %field.bits
  %bound = add i64 %3, 32
  br label %loop

loop:
  %acc0.0 = load i128, ptr %acc0, align 16
  %acc1.0 = load i128, ptr %acc1, align 16
  %rax.0 = load i64, ptr %rax, align 8
  %sc = shl i64 %rax.0, 2
  %b0 = load ptr, ptr %base, align 8
  %addr = getelementptr i8, ptr %b0, i64 %sc
  %a = load i128, ptr %addr, align 1
  %b = load i128, ptr %b0, align 1
  %acc0.1 = add i128 %acc0.0, %a
  %acc0.1b = add i128 %acc0.1, %b
  %acc1.1 = add i128 %acc1.0, %a
  %acc1.1b = add i128 %acc1.1, %b
  %step = add i64 %rax.0, 32
  %again = icmp eq i64 %bound, %step
  br i1 %again, label %latch, label %exit, !revenant.region !10

latch:
  store i128 %acc0.1b, ptr %acc0, align 16
  store i128 %acc1.1b, ptr %acc1, align 16
  store i64 %step, ptr %rax, align 8
  br label %loop

exit:
  %s0 = add i128 %acc0.1b, %acc1.1b
  %sum = add i128 %s0, %s0
  %wide = bitcast i128 %sum to <4 x i32>
  %half = shufflevector <4 x i32> %wide, <4 x i32> poison, <2 x i32> <i32 2, i32 3>
  %halfwide = bitcast <2 x i32> %half to i64
  %total = add i64 %halfwide, %halfwide
  %out = trunc i64 %total to i32
  br label %done

done:
  %result = phi i32 [ 0, %entry ], [ %out, %exit ]
  ret i32 %result
}

; Block order: 0 entry, 1 check, 2 loop, 3 latch, 4 exit, 5 done.
!10 = !{!"while", i32 2, i32 3, i32 4}

; CHECK: unsigned int sub_1400016E0(void *a1)
; The entry block's definitions are emitted before the first test.
; CHECK: v0 = *(void **)a1
; CHECK: base = v0
; CHECK: v1 = (unsigned long long)v0
; CHECK: v2 = *(unsigned long long *)v0
; CHECK: if (v2 == v1)
; The second test reads the entry block's own bound, not an unassigned temp.
; CHECK: while (v2 - v1 + 32ULL
; The loop header's loads reach the body.
; CHECK: v3 = rax
; CHECK: v5 = base
; CHECK: v6 = *(struct revenant_i128_u *)((unsigned char *)v5 + (v3 << 2ULL))
; CHECK: v7 = *(struct revenant_i128_u *)v5
; CHECK: v8 = revenant_i128_u_add(revenant_i128_u_add(acc0, v6), v7)
; CHECK: v9 = revenant_i128_u_add(revenant_i128_u_add(acc1, v6), v7)
; The IV step is assigned inside the loop, not replaced by the bound.
; CHECK: v4 = v3 + 32ULL
; The accumulator and IV backedge stores are real values.
; CHECK: acc0 = v8
; CHECK: acc1 = v9
; The IV backedge store is the real step, spelled as a compound update.
; CHECK: rax{{( \+= 32ULL| = v4)}}
; And nothing in the loop is reset to a zero literal.
; CHECK-NOT: acc0 = (struct revenant_i128_u){0ULL, 0ULL}
