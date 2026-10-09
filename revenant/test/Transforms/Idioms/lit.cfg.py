import os
import lit.formats

# lit config for the Idioms slice. Run with:
#   <build-revenant>/bin/llvm-lit.cmd -v <worktree>/revenant/test/Transforms/Idioms
# revenant-opt links every RevenantTransform* library statically, so no
# plugin flag is needed: the RUN lines are plain
#   %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
config.name = "RevenantIdioms"
# Internal shell: this box has no usable external bash for lit's probe
# (the system32 WSL stub shadows Git bash and fails the -f check).
config.test_format = lit.formats.ShTest(False)
config.suffixes = [".ll"]
config.test_source_root = os.path.dirname(os.path.abspath(__file__))

# The worktree's own build tree by default; override with
# REVENANT_IDIOMS_OPT to use a shared build directory. Four dirs up from
# test/Transforms/Idioms is the worktree root (Idioms -> Transforms ->
# test -> revenant -> worktree).
_bindir = os.environ.get("REVENANT_IDIOMS_OPT", "")
if not _bindir:
    _bindir = os.path.normpath(os.path.join(
        config.test_source_root, "..", "..", "..", "..",
        "build-rv", "tools", "revenant-opt"))
config.substitutions.append(("%revenant-opt", _bindir + r"\revenant-opt.exe"))

# FileCheck and llvm-lit ship with the shared prebuilt LLVM+Clang tree.
_fc = os.environ.get("REVENANT_FILECHECK", "")
if not _fc:
    _fc = os.path.normpath(r"C:\Users\User\RustroverProjects\llvm-project\build-revenant\bin\FileCheck.exe")
config.substitutions.append(("%FileCheck", _fc))
