; Reduced real header-value pattern from c-clang-o2 hosted main 0x140001B90.
; The multi-use header values must be initialized exactly once before the
; region condition; calling instr() twice drains PendingInitializers.
; RUN: python %S/loop-latch-backedge-run.py %s sub_140001B90 0 1
; RUN: python %S/loop-latch-backedge-run.py %s sub_140001B90 1 4
; REQUIRES: structured-source-run

target triple = "x86_64-pc-windows-msvc"
!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"c"}
!1 = !{!"if", i32 -1}

define i32 @sub_140001B90(i32 %0) {
entry:
  %cookie = add i32 %0, 1
  %next = add i32 %cookie, 2
  %valid = icmp eq i32 %next, 3
  br i1 %valid, label %valid_path, label %invalid_path, !revenant.region !1
valid_path:
  ret i32 %cookie
invalid_path:
  ret i32 %next
}
