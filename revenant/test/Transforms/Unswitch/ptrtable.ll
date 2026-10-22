; RUN: %revenant-opt -passes=revenant-unswitch -S %s | %FileCheck %s
;
; Constant function-pointer table built on the stack behind an opaque tail
; jump: GCC materializes `binop ops[2] = {add_op, sub_op}` with a vector store
; loads `ops[which & 1]` and tail-jumps through it. The lift leaves the
; indirect branch as an opaque `revenant.asm.jmp` call, which the emitter
; turns into an undefined `revenant_2easm_2ejmp` reference; the pass folds it
; back into a switch of direct tail calls.
; Corpus: c-gcc-o2 / c-gcc-o2avx2 / cjson-gcc-os @call_ptr 0x140001B40.

@f_sub_1400016D0 = external global [16 x i8]
@f_sub_1400016E0 = external global [16 x i8]

declare void @revenant.asm.jmp(...)

define i64 @sub_1400016D0(i64 %0, i64 %1) #36 {
bb_1400016d0:
  %frame = alloca [304 x i8], align 1
  %pf = alloca i1, align 1
  %of = alloca i1, align 1
  %cf = alloca i1, align 1
  %sf = alloca i1, align 1
  %zf = alloca i1, align 1
  %xmm15 = alloca i128, align 16
  %xmm14 = alloca i128, align 16
  %xmm13 = alloca i128, align 16
  %xmm12 = alloca i128, align 16
  %xmm11 = alloca i128, align 16
  %xmm10 = alloca i128, align 16
  %xmm9 = alloca i128, align 16
  %xmm8 = alloca i128, align 16
  %xmm7 = alloca i128, align 16
  %xmm6 = alloca i128, align 16
  %xmm5 = alloca i128, align 16
  %xmm4 = alloca i128, align 16
  %xmm3 = alloca i128, align 16
  %xmm2 = alloca i128, align 16
  %xmm1 = alloca i128, align 16
  %xmm0 = alloca i128, align 16
  %r15 = alloca i64, align 8
  %r14 = alloca i64, align 8
  %r13 = alloca i64, align 8
  %r12 = alloca i64, align 8
  %r11 = alloca i64, align 8
  %r10 = alloca i64, align 8
  %r9 = alloca i64, align 8
  %r8 = alloca i64, align 8
  %rbp = alloca i64, align 8
  %rdi = alloca i64, align 8
  %rsi = alloca i64, align 8
  %rbx = alloca i64, align 8
  %rdx = alloca i64, align 8
  %rcx = alloca i64, align 8
  %rax = alloca i64, align 8
  %sp0 = getelementptr inbounds i8, ptr %frame, i64 8
  store i64 %0, ptr %rcx, align 8
  store i64 %1, ptr %rdx, align 8
  store i64 undef, ptr %rax, align 8
  store i64 undef, ptr %rbx, align 8
  store i64 undef, ptr %rsi, align 8
  store i64 undef, ptr %rdi, align 8
  store i64 undef, ptr %rbp, align 8
  store i64 undef, ptr %r8, align 8
  store i64 undef, ptr %r9, align 8
  store i64 undef, ptr %r10, align 8
  store i64 undef, ptr %r11, align 8
  store i64 undef, ptr %r12, align 8
  store i64 undef, ptr %r13, align 8
  store i64 undef, ptr %r14, align 8
  store i64 undef, ptr %r15, align 8
  store i1 undef, ptr %cf, align 1
  store i1 undef, ptr %of, align 1
  store i1 undef, ptr %pf, align 1
  store i1 undef, ptr %sf, align 1
  store i1 undef, ptr %zf, align 1
  %r = load i64, ptr %rdx, align 8
  %r1 = load i64, ptr %rcx, align 8
  %sc = mul i64 %r, 1
  %a = add i64 %r1, %sc
  %2 = trunc i64 %a to i32
  %z = zext i32 %2 to i64
  store i64 %z, ptr %rax, align 8
  %r2 = load i64, ptr %rax, align 8
  ret i64 %r2
}

define i64 @sub_1400016E0(i32 %0, i32 %1) #37 {
bb_1400016e0:
  %frame = alloca [304 x i8], align 1
  %pf = alloca i1, align 1
  %of = alloca i1, align 1
  %cf = alloca i1, align 1
  %sf = alloca i1, align 1
  %zf = alloca i1, align 1
  %xmm15 = alloca i128, align 16
  %xmm14 = alloca i128, align 16
  %xmm13 = alloca i128, align 16
  %xmm12 = alloca i128, align 16
  %xmm11 = alloca i128, align 16
  %xmm10 = alloca i128, align 16
  %xmm9 = alloca i128, align 16
  %xmm8 = alloca i128, align 16
  %xmm7 = alloca i128, align 16
  %xmm6 = alloca i128, align 16
  %xmm5 = alloca i128, align 16
  %xmm4 = alloca i128, align 16
  %xmm3 = alloca i128, align 16
  %xmm2 = alloca i128, align 16
  %xmm1 = alloca i128, align 16
  %xmm0 = alloca i128, align 16
  %r15 = alloca i64, align 8
  %r14 = alloca i64, align 8
  %r13 = alloca i64, align 8
  %r12 = alloca i64, align 8
  %r11 = alloca i64, align 8
  %r10 = alloca i64, align 8
  %r9 = alloca i64, align 8
  %r8 = alloca i64, align 8
  %rbp = alloca i64, align 8
  %rdi = alloca i64, align 8
  %rsi = alloca i64, align 8
  %rbx = alloca i64, align 8
  %rdx = alloca i64, align 8
  %rcx = alloca i64, align 8
  %rax = alloca i64, align 8
  %sp0 = getelementptr inbounds i8, ptr %frame, i64 8
  %e = zext i32 %0 to i64
  store i64 %e, ptr %rcx, align 8
  %e1 = zext i32 %1 to i64
  store i64 %e1, ptr %rdx, align 8
  store i64 undef, ptr %rax, align 8
  store i64 undef, ptr %rbx, align 8
  store i64 undef, ptr %rsi, align 8
  store i64 undef, ptr %rdi, align 8
  store i64 undef, ptr %rbp, align 8
  store i64 undef, ptr %r8, align 8
  store i64 undef, ptr %r9, align 8
  store i64 undef, ptr %r10, align 8
  store i64 undef, ptr %r11, align 8
  store i64 undef, ptr %r12, align 8
  store i64 undef, ptr %r13, align 8
  store i64 undef, ptr %r14, align 8
  store i64 undef, ptr %r15, align 8
  store i1 undef, ptr %cf, align 1
  store i1 undef, ptr %of, align 1
  store i1 undef, ptr %pf, align 1
  store i1 undef, ptr %sf, align 1
  store i1 undef, ptr %zf, align 1
  %r = load i64, ptr %rcx, align 8
  %t = trunc i64 %r to i32
  %z = zext i32 %t to i64
  store i64 %z, ptr %rax, align 8
  %r2 = load i64, ptr %rdx, align 8
  %t3 = trunc i64 %r2 to i32
  %r4 = load i64, ptr %rax, align 8
  %t5 = trunc i64 %r4 to i32
  %2 = sub i32 %t5, %t3
  %zf6 = icmp eq i32 %2, 0
  store i1 %zf6, ptr %zf, align 1
  %sf7 = and i32 %2, -2147483648
  %sf8 = icmp ne i32 %sf7, 0
  store i1 %sf8, ptr %sf, align 1
  %l = trunc i32 %2 to i8
  %3 = call i8 @llvm.ctpop.i8(i8 %l)
  %pf9 = trunc i8 %3 to i1
  %pf10 = xor i1 %pf9, true
  store i1 %pf10, ptr %pf, align 1
  %cf11 = icmp ult i32 %t5, %t3
  store i1 %cf11, ptr %cf, align 1
  %o = xor i32 %t5, %t3
  %o2 = xor i32 %t5, %2
  %o3 = and i32 %o, %o2
  %o4 = and i32 %o3, -2147483648
  %of12 = icmp ne i32 %o4, 0
  store i1 %of12, ptr %of, align 1
  %z13 = zext i32 %2 to i64
  store i64 %z13, ptr %rax, align 8
  %r16 = load i64, ptr %rax, align 8
  ret i64 %r16
}

define i64 @sub_140001B40(i32 %0, i32 %1, i32 %2) #54 {
bb_140001b40:
  %frame = alloca [328 x i8], align 1
  %pf = alloca i1, align 1
  %of = alloca i1, align 1
  %cf = alloca i1, align 1
  %sf = alloca i1, align 1
  %zf = alloca i1, align 1
  %xmm15 = alloca i128, align 16
  %xmm14 = alloca i128, align 16
  %xmm13 = alloca i128, align 16
  %xmm12 = alloca i128, align 16
  %xmm11 = alloca i128, align 16
  %xmm10 = alloca i128, align 16
  %xmm9 = alloca i128, align 16
  %xmm8 = alloca i128, align 16
  %xmm7 = alloca i128, align 16
  %xmm6 = alloca i128, align 16
  %xmm5 = alloca i128, align 16
  %xmm4 = alloca i128, align 16
  %xmm3 = alloca i128, align 16
  %xmm2 = alloca i128, align 16
  %xmm1 = alloca i128, align 16
  %xmm0 = alloca i128, align 16
  %r15 = alloca i64, align 8
  %r14 = alloca i64, align 8
  %r13 = alloca i64, align 8
  %r12 = alloca i64, align 8
  %r11 = alloca i64, align 8
  %r10 = alloca i64, align 8
  %r9 = alloca i64, align 8
  %r8 = alloca i64, align 8
  %rbp = alloca i64, align 8
  %rdi = alloca i64, align 8
  %rsi = alloca i64, align 8
  %rbx = alloca i64, align 8
  %rdx = alloca i64, align 8
  %rcx = alloca i64, align 8
  %rax = alloca i64, align 8
  %sp0 = getelementptr inbounds i8, ptr %frame, i64 32
  %e = zext i32 %0 to i64
  store i64 %e, ptr %rcx, align 8
  %e1 = zext i32 %1 to i64
  store i64 %e1, ptr %rdx, align 8
  %e2 = zext i32 %2 to i64
  store i64 %e2, ptr %r8, align 8
  store i64 undef, ptr %rax, align 8
  store i64 undef, ptr %rbx, align 8
  store i64 undef, ptr %rsi, align 8
  store i64 undef, ptr %rdi, align 8
  store i64 undef, ptr %rbp, align 8
  store i64 undef, ptr %r9, align 8
  store i64 undef, ptr %r10, align 8
  store i64 undef, ptr %r11, align 8
  store i64 undef, ptr %r12, align 8
  store i64 undef, ptr %r13, align 8
  store i64 undef, ptr %r14, align 8
  store i64 undef, ptr %r15, align 8
  store i1 undef, ptr %cf, align 1
  store i1 undef, ptr %of, align 1
  store i1 undef, ptr %pf, align 1
  store i1 undef, ptr %sf, align 1
  store i1 undef, ptr %zf, align 1
  %fp = getelementptr inbounds i8, ptr %sp0, i64 0
  %rsp = ptrtoint ptr %fp to i64
  %3 = sub i64 %rsp, 24
  %zf3 = icmp eq i64 %3, 0
  store i1 %zf3, ptr %zf, align 1
  %sf4 = and i64 %3, -9223372036854775808
  %sf5 = icmp ne i64 %sf4, 0
  store i1 %sf5, ptr %sf, align 1
  %l = trunc i64 %3 to i8
  %4 = call i8 @llvm.ctpop.i8(i8 %l)
  %pf6 = trunc i8 %4 to i1
  %pf7 = xor i1 %pf6, true
  store i1 %pf7, ptr %pf, align 1
  %cf8 = icmp ult i64 %rsp, 24
  store i1 %cf8, ptr %cf, align 1
  %o = xor i64 %rsp, 24
  %o2 = xor i64 %rsp, %3
  %o3 = and i64 %o, %o2
  %o4 = and i64 %o3, -9223372036854775808
  %of9 = icmp ne i64 %o4, 0
  store i1 %of9, ptr %of, align 1
  store i64 ptrtoint (ptr @f_sub_1400016D0 to i64), ptr %r9, align 8
  %r = load i64, ptr %r9, align 8
  %5 = zext i64 %r to i128
  store i128 %5, ptr %xmm0, align 16
  %r16 = load i64, ptr %rcx, align 8
  %t = trunc i64 %r16 to i32
  %z = zext i32 %t to i64
  store i64 %z, ptr %rax, align 8
  %r17 = load i64, ptr %rdx, align 8
  %t18 = trunc i64 %r17 to i32
  %z19 = zext i32 %t18 to i64
  store i64 %z19, ptr %rcx, align 8
  %r20 = load i64, ptr %r8, align 8
  %t21 = trunc i64 %r20 to i32
  %z22 = zext i32 %t21 to i64
  store i64 %z22, ptr %rdx, align 8
  store i64 ptrtoint (ptr @f_sub_1400016E0 to i64), ptr %r8, align 8
  %r23 = load i64, ptr %rax, align 8
  %t24 = trunc i64 %r23 to i32
  %6 = and i32 %t24, 1
  %zf25 = icmp eq i32 %6, 0
  store i1 %zf25, ptr %zf, align 1
  %sf26 = and i32 %6, -2147483648
  %sf27 = icmp ne i32 %sf26, 0
  store i1 %sf27, ptr %sf, align 1
  %l28 = trunc i32 %6 to i8
  %7 = call i8 @llvm.ctpop.i8(i8 %l28)
  %pf29 = trunc i8 %7 to i1
  %pf30 = xor i1 %pf29, true
  store i1 %pf30, ptr %pf, align 1
  store i1 false, ptr %cf, align 1
  store i1 false, ptr %of, align 1
  %z31 = zext i32 %6 to i64
  store i64 %z31, ptr %rax, align 8
  %r32 = load i64, ptr %r8, align 8
  %8 = zext i64 %r32 to i128
  store i128 %8, ptr %xmm1, align 16
  %x = load i128, ptr %xmm1, align 16
  %x33 = load i128, ptr %xmm0, align 16
  %9 = bitcast i128 %x33 to <2 x i64>
  %10 = bitcast i128 %x to <2 x i64>
  %11 = shufflevector <2 x i64> %9, <2 x i64> %10, <2 x i32> <i32 0, i32 2>
  %12 = bitcast <2 x i64> %11 to i128
  store i128 %12, ptr %xmm0, align 16
  %x34 = load i128, ptr %xmm0, align 16
  %fp35 = getelementptr inbounds i8, ptr %sp0, i64 -24
  %13 = ptrtoint ptr %fp35 to i64
  store i128 %x34, ptr %fp35, align 1
  %r36 = load i64, ptr %rax, align 8
  %sc = mul i64 %r36, 8
  %a = add i64 %sc, -24
  %fp37 = getelementptr inbounds i8, ptr %sp0, i64 %a
  %14 = ptrtoint ptr %fp37 to i64
  %15 = load i64, ptr %fp37, align 1
  store i64 %15, ptr %rax, align 8
  %fp38 = getelementptr inbounds i8, ptr %sp0, i64 -24
  %rsp39 = ptrtoint ptr %fp38 to i64
  %16 = add i64 %rsp39, 24
  %zf40 = icmp eq i64 %16, 0
  store i1 %zf40, ptr %zf, align 1
  %sf41 = and i64 %16, -9223372036854775808
  %sf42 = icmp ne i64 %sf41, 0
  store i1 %sf42, ptr %sf, align 1
  %l43 = trunc i64 %16 to i8
  %17 = call i8 @llvm.ctpop.i8(i8 %l43)
  %pf44 = trunc i8 %17 to i1
  %pf45 = xor i1 %pf44, true
  store i1 %pf45, ptr %pf, align 1
  %cf46 = icmp ult i64 %16, %rsp39
  store i1 %cf46, ptr %cf, align 1
  %o47 = xor i64 %rsp39, 24
  %n = xor i64 %o47, -1
  %o248 = xor i64 %rsp39, %16
  %o349 = and i64 %n, %o248
  %o450 = and i64 %o349, -9223372036854775808
  %of51 = icmp ne i64 %o450, 0
  store i1 %of51, ptr %of, align 1
  %r52 = load i64, ptr %rax, align 8
  call void (...) @revenant.asm.jmp(i64 %r52), !revenant.opaque !6
  store i64 undef, ptr %rax, align 8
  store i64 undef, ptr %rcx, align 8
  store i64 undef, ptr %rdx, align 8
  store i64 undef, ptr %r8, align 8
  store i64 undef, ptr %r9, align 8
  store i64 undef, ptr %r10, align 8
  store i64 undef, ptr %r11, align 8
  store i128 undef, ptr %xmm0, align 16
  store i128 undef, ptr %xmm1, align 16
  store i128 undef, ptr %xmm2, align 16
  store i128 undef, ptr %xmm3, align 16
  store i128 undef, ptr %xmm4, align 16
  store i128 undef, ptr %xmm5, align 16
  store i1 undef, ptr %cf, align 1
  store i1 undef, ptr %of, align 1
  store i1 undef, ptr %pf, align 1
  store i1 undef, ptr %sf, align 1
  store i1 undef, ptr %zf, align 1
  unreachable
}

; CHECK-LABEL: define i64 @sub_140001B40(
; The opaque tail jump is gone: a switch of direct tail calls replaces it.
; CHECK-NOT:   call void (...) @revenant.asm.jmp
; CHECK-DAG:   %sw.ptr.elem = lshr i64 %sc, 3
; CHECK-DAG:   switch i64 %sw.ptr.elem, label %sw.ptr.dead [
; CHECK-DAG:   i64 0, label %sw.ptr.case
; CHECK-DAG:   i64 1, label %sw.ptr.case2
; CHECK-DAG:   %18 = tail call i64 @sub_1400016D0(
; CHECK-DAG:   %19 = tail call i64 @sub_1400016E0(
; CHECK-DAG:   sw.ptr.dead:
; CHECK-DAG:   unreachable
; The table write itself survives: only the load-and-jump region is rewritten.
; CHECK-DAG:   %11 = shufflevector <2 x i64>

!6 = !{}
