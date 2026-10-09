import os
import lit.formats

# Standalone lit config for the Uneh slice. Run from the slice build:
#   <build-revenant>/bin/llvm-lit -v <worktree>/revenant/test/Transforms/Uneh
# The plugin DLL path comes from the environment (set by the build), else it
# defaults to <worktree>/build-rv-uneh/RevenantTransformUneh.dll.
config.name = "RevenantUneh"
config.test_format = lit.formats.ShTest(True)
config.suffixes = [".ll"]
config.test_source_root = os.path.dirname(os.path.abspath(__file__))

bindir = r"C:\Users\User\RustroverProjects\llvm-project\build-revenant\bin"
plugin = os.environ.get("REVENANT_UNEH_PLUGIN", "")
if not plugin:
    plugin = os.path.normpath(os.path.join(
        config.test_source_root, "..", "..", "..", "..",
        "build-rv-uneh", "RevenantTransformUneh.dll"))

config.substitutions.append(("%opt", bindir + r"\opt.exe"))
config.substitutions.append(("%FileCheck", bindir + r"\FileCheck.exe"))
config.substitutions.append(("%plugin", plugin))
