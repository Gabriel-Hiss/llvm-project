; The none-vs-embedded invariant: the two metadata modes must produce the same
; control flow, statements and call argument counts; names, types and metadata
; are the only things allowed to differ.
;
; `invariant.py --selftest` checks the comparison itself against fixtures:
; one function spelled the way each mode spells it must normalize equal, and
; edits that change instructions, constants, branch structure or call arity
; must normalize different. The corpus sweep itself (driver plus parity
; binaries) is a manual run; this test guards the rule the sweep enforces.
;
; RUN: %python %S/../meta-mode/invariant.py --selftest
