; RUN: revenant-opt -passes='function(revenant-lift-normalize),verify' -S %s | FileCheck %s
; RUN: revenant-opt -passes='function(revenant-lift-normalize,sroa,mem2reg,instcombine,revenant-lift-normalize),verify' -S %s | FileCheck %s --check-prefix=CLEAN
; Reduced from c-clang-o2 sub_140024848 ir-clean: base + 48/64/72,
; i64 register phi, trunc i64 -> i32, and byte-addressed stores.
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"

; CHECK-LABEL: define i32 @fields
; CHECK: %base.ptr = inttoptr i64 %base to ptr
; CHECK: getelementptr i32, ptr %base.ptr, i64 12
; CHECK: load i32
; CHECK: getelementptr i32, ptr {{.*}}, i64 18
; CLEAN-LABEL: define i32 @fields
; CLEAN: getelementptr i32
; CLEAN: add i32
; CLEAN-NOT: add i64
; CLEAN: ret i32
define i32 @fields(i64 %base, i64 %reg) {
  %d42 = add i64 %base, 48, !revenant.va !0
  %p = inttoptr i64 %d42 to ptr, !revenant.va !0
  %v = load i32, ptr %p, align 1, !revenant.va !0
  %wide = add i64 %reg, 1
  %n = trunc i64 %wide to i32
  %d166 = add i64 %base, 72, !revenant.va !0
  %out = inttoptr i64 %d166 to ptr
  store i32 %n, ptr %out, align 1
  ret i32 %v
}

; The raw lifter uses frame + sp0 + instruction-dependent constant offset.
; Preserve overlapping accesses, and keep escaped frames as one object.
; CHECK-LABEL: define i32 @stack_slots
; CHECK: %stack_16 = alloca i32
; CHECK: %stack_24 = alloca i64
; CHECK-NOT: alloca [304 x i8]
; CLEAN-LABEL: define i32 @stack_slots
; CLEAN-NOT: alloca
; CLEAN: ret i32 %arg
define i32 @stack_slots(i32 %arg) {
  %frame = alloca [304 x i8], align 1, !revenant.va !0
  %sp0 = getelementptr inbounds i8, ptr %frame, i64 8
  %fp = getelementptr inbounds i8, ptr %sp0, i64 8
  %fp2 = getelementptr inbounds i8, ptr %sp0, i64 16
  store i32 %arg, ptr %fp, align 1
  store i64 3, ptr %fp2, align 1
  %v = load i32, ptr %fp, align 1
  ret i32 %v
}

; CHECK-LABEL: define i32 @overlap
; CHECK: %stack_16 = alloca [8 x i8]
; CHECK: getelementptr i32, ptr %stack_16, i64 1
define i32 @overlap(i64 %arg) {
  %frame = alloca [304 x i8], align 1
  %fp = getelementptr i8, ptr %frame, i64 16
  %hi = getelementptr i8, ptr %fp, i64 4
  store i64 %arg, ptr %fp, align 1
  %v = load i32, ptr %hi, align 1
  ret i32 %v
}

declare void @escape(ptr)
; CHECK-LABEL: define void @escaped
; CHECK: %frame = alloca [304 x i8]
define void @escaped() {
  %frame = alloca [304 x i8], align 1
  %fp = getelementptr i8, ptr %frame, i64 16
  call void @escape(ptr %fp)
  ret void
}

; CHECK-LABEL: define i32 @roundtrip
; CHECK-NOT: inttoptr
; CHECK: getelementptr i32, ptr %base, i64 %index
; CHECK-SAME: !revenant.va
; Scaled index is the general memory operand's idx*scale expression.
define i32 @roundtrip(ptr %base, i64 %index) {
  %bits = ptrtoint ptr %base to i64
  %sc = mul i64 %index, 4
  %sum = add i64 %bits, %sc, !revenant.va !0
  %p = inttoptr i64 %sum to ptr
  %v = load i32, ptr %p, align 1
  ret i32 %v
}

; CHECK-LABEL: define void @pointer_loop
; CHECK: %r8.ptr = phi ptr
; CHECK: getelementptr i8, ptr %r8.ptr, i64 1
; Pointer IVs arise as i64 register PHIs on lifted loop backedges.
define void @pointer_loop(ptr %base, i64 %count) {
entry:
  %bits = ptrtoint ptr %base to i64
  br label %loop
loop:
  %r8 = phi i64 [ %bits, %entry ], [ %next, %loop ]
  %iv = phi i64 [ 0, %entry ], [ %inc, %loop ]
  %p = inttoptr i64 %r8 to ptr
  store i8 0, ptr %p
  %next = add i64 %r8, 1
  %inc = add i64 %iv, 1
  %done = icmp eq i64 %inc, %count
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
; CLEAN-LABEL: define i32 @int_loop
; CLEAN: phi i32
; CLEAN: add i32
; CLEAN-NOT: phi i64
; CLEAN: ret i32
; The machine register's high half is cleared by every 32-bit write.
define i32 @int_loop(i32 %limit) {
entry:
  br label %loop
loop:
  %r = phi i64 [ 0, %entry ], [ %wide, %loop ]
  %v = trunc i64 %r to i32
  %next = add i32 %v, 1
  %wide = zext i32 %next to i64
  %done = icmp eq i32 %next, %limit
  br i1 %done, label %exit, label %loop
exit:
  ret i32 %next
}

; CHECK-LABEL: define i32 @shift_register
; CHECK: lshr i64
; CHECK: trunc i64
; A narrowed shift is not a modular add and must not query overflow flags.
define i32 @shift_register(i64 %r) {
  %high = lshr i64 %r, 32
  %v = trunc i64 %high to i32
  ret i32 %v
}

; CHECK-LABEL: define i32 @bounded_address
; CHECK: getelementptr inbounds i8
; Scaling a wide index in an inbounds element GEP can introduce poison even
; when the original wrapped byte offset was zero. Keep this byte GEP.
define i32 @bounded_address(ptr %base, i64 %index) {
  %bytes = shl i64 %index, 2
  %p = getelementptr inbounds i8, ptr %base, i64 %bytes
  %v = load i32, ptr %p
  ret i32 %v
}

; CHECK-LABEL: define i32 @signed_register
; CHECK: phi i32
; CHECK: sext i32
define i32 @signed_register(i1 %cond, i32 %arg) {
entry:
  %wide = sext i32 %arg to i64
  br i1 %cond, label %left, label %right
left:
  br label %join
right:
  br label %join
join:
  %r = phi i64 [ %wide, %left ], [ -1, %right ]
  %v = trunc i64 %r to i32
  ret i32 %v
}

; CHECK-LABEL: define void @duplicate_switch_edge
; CHECK: %r.ptr = phi ptr [ [[P:%[^ ,]+]], %entry ], [ [[P]], %entry ], [ [[P]], %entry ]
; Reduced from raw c-o2 repeated bb_14006d19e switch predecessor edges.
define void @duplicate_switch_edge(i32 %tag, i64 %arg) {
entry:
  switch i32 %tag, label %join [ i32 0, label %join
                               i32 1, label %join ]
join:
  %r = phi i64 [ %arg, %entry ], [ %arg, %entry ], [ %arg, %entry ]
  %p = inttoptr i64 %r to ptr
  store i8 0, ptr %p
  ret void
}

; CHECK-LABEL: define i32 @volatile_frame
; CHECK: %frame = alloca [304 x i8]
; CHECK: load volatile i32
define i32 @volatile_frame() {
  %frame = alloca [304 x i8], align 1
  %fp = getelementptr i8, ptr %frame, i64 16
  %v = load volatile i32, ptr %fp, align 1
  ret i32 %v
}

; CHECK-LABEL: define i32 @dynamic_frame
; CHECK: %frame = alloca [304 x i8]
; CHECK: getelementptr i32, ptr %frame, i64 %index
define i32 @dynamic_frame(i64 %index) {
  %frame = alloca [304 x i8], align 1
  %bytes = mul i64 %index, 4
  %fp = getelementptr i8, ptr %frame, i64 %bytes
  %v = load i32, ptr %fp, align 1
  ret i32 %v
}

; CHECK-LABEL: define i32 @wide_overflow_flags
; CHECK: add nsw i64
; CHECK: trunc i64
; Keep the wide poison condition instead of dropping it when narrowing.
define i32 @wide_overflow_flags(i64 %reg) {
  %sum = add nsw i64 %reg, 1
  %v = trunc i64 %sum to i32
  ret i32 %v
}

!0 = !{i64 5368858749}
