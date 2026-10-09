import os
import lit.formats

# Standalone lit config for the Patterns slice. Run:
#   python <build-revenant>/bin/llvm-lit.py -v <worktree>/revenant/test/Transforms/Patterns
# The plugin DLL path comes from the environment (set by the build), else it
# defaults to <worktree>/build-rv-patterns/RevenantTransformPatterns.dll.
config.name = "RevenantPatterns"
# Internal shell: this box has no usable external bash for lit's probe
# (the system32 WSL stub shadows Git bash and fails the -f check).
config.test_format = lit.formats.ShTest(False)
config.suffixes = [".ll"]
config.excludes = ["Inputs"]
config.test_source_root = os.path.dirname(os.path.abspath(__file__))
bindir = "C:/Users/User/RustroverProjects/llvm-project/build-revenant/bin"
plugin = os.environ.get("REVENANT_PATTERNS_PLUGIN", "")
if not plugin:
    plugin = os.path.normpath(os.path.join(
        config.test_source_root, "..", "..", "..", "..",
        "build-rv-patterns", "Release", "RevenantTransformPatterns.dll"))
    plugin = plugin.replace("\\", "/")

config.substitutions.append(("%opt", bindir + "/opt.exe"))
config.substitutions.append(("%FileCheck", bindir + "/FileCheck.exe"))
config.substitutions.append(("%plugin", plugin))
