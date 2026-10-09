; Multi-block matches annotate only: this is the cleaned pat_cpp_O2.ll
; vector_push_back shape (len==cap guard + grow call + store/len bump
; across three blocks). The oracle catalog matches it at full coverage,
; but the region spans blocks, so the pass must leave it untouched.
; RUN: env REVENANT_PATTERNS_CATALOG=%S/Inputs %opt -load-pass-plugin=%plugin -passes=revenant-patterns -S %s | %FileCheck %s
declare ptr @"??$_Emplace_reallocate@AEBH@?$vector@HV?$allocator@H@std@@@std@@AEAAPEAHQEAHAEBH@Z"(ptr, ptr, ptr)

define void @push_like(ptr %0, i32 %1) {
  %3 = alloca i32, align 4
  store i32 %1, ptr %3, align 4
  %4 = getelementptr i8, ptr %0, i64 8
  %5 = load ptr, ptr %4, align 8
  %6 = getelementptr i8, ptr %0, i64 16
  %7 = load ptr, ptr %6, align 8
  %8 = icmp eq ptr %5, %7
  br i1 %8, label %grow, label %fast

grow:
  ; CHECK: call ptr @"??$_Emplace_reallocate@AEBH@?$vector@HV?$allocator@H@std@@@std@@AEAAPEAHQEAHAEBH@Z"
  ; CHECK-NOT: __pat_vector_push_back
  %9 = call ptr @"??$_Emplace_reallocate@AEBH@?$vector@HV?$allocator@H@std@@@std@@AEAAPEAHQEAHAEBH@Z"(ptr %0, ptr %5, ptr %3)
  br label %fast

fast:
  store i32 %1, ptr %5, align 4
  %10 = load ptr, ptr %4, align 8
  %11 = getelementptr i8, ptr %10, i64 4
  store ptr %11, ptr %4, align 8
  ret void
}
