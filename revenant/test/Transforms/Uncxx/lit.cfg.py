import os
import lit.formats

config.name = "RevenantUncxx"
config.test_format = lit.formats.ShTest(False)
config.suffixes = [".ll"]
config.test_source_root = os.path.dirname(os.path.abspath(__file__))

worktree = os.path.normpath(os.path.join(config.test_source_root, "..", "..", "..", ".."))
revenant_opt = os.environ.get(
    "REVENANT_OPT",
    os.path.join(worktree, "build-rv", "tools", "revenant-opt", "revenant-opt.exe"),
)
bindir = os.environ.get(
    "LLVM_BUILD_BIN",
    r"C:\Users\User\RustroverProjects\Revenant\llvm-project\build-revenant\bin",
)
config.substitutions.append(("%revenant-opt", '"' + revenant_opt + '"'))
config.substitutions.append(("%FileCheck", '"' + os.path.join(bindir, "FileCheck.exe") + '"'))
