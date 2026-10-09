import os
import lit.formats

# Standalone lit config for the UntailDup slice. Run with:
#   python <build-revenant>/bin/llvm-lit.py -v <worktree>/revenant/test/Transforms/UntailDup
# The plugin DLL path comes from the environment (set by the build), else it
# defaults to <worktree>/build-rv-untaildup/RevenantTransformUntailDup.dll.
config.name = "RevenantUntailDup"
# Internal shell: this box has no usable external bash for lit's probe
# (the system32 WSL stub shadows Git bash and fails the -f check).
config.test_format = lit.formats.ShTest(False)
config.suffixes = [".ll"]
config.test_source_root = os.path.dirname(os.path.abspath(__file__))

bindir = r"C:\Users\User\RustroverProjects\llvm-project\build-revenant\bin"
plugin = os.environ.get("REVENANT_UNTAILUDP_PLUGIN", "")
if not plugin:
    plugin = os.path.normpath(os.path.join(
        config.test_source_root, "..", "..", "..", "..",
        "build-rv-untaildup", "RevenantTransformUntailDup.dll"))

config.substitutions.append(("%opt", bindir + r"\opt.exe"))
config.substitutions.append(("%FileCheck", bindir + r"\FileCheck.exe"))
config.substitutions.append(("%plugin", plugin))
