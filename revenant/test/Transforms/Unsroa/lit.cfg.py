import os
import lit.formats

# Standalone lit config for the Unsroa slice. Run from the slice build:
#   <build-revenant>/bin/llvm-lit -v <worktree>/revenant/test/Transforms/Unsroa
# The plugin DLL path comes from the environment (set by the build), else it
# defaults to <worktree>/build-rv-unsroa/RevenantTransformUnsroa.dll.
config.name = "RevenantUnsroa"
config.test_format = lit.formats.ShTest(True)
config.suffixes = [".ll"]
config.test_source_root = os.path.dirname(os.path.abspath(__file__))

bindir = r"C:\Users\User\RustroverProjects\llvm-project\build-revenant\bin"
plugin = os.environ.get("REVENANT_UNSROA_PLUGIN", "")
if not plugin:
    plugin = os.path.normpath(os.path.join(
        config.test_source_root, "..", "..", "..", "..",
        "build-rv-unsroa", "RevenantTransformUnsroa.dll"))

bindir = bindir.replace("\\", "/")
config.substitutions.append(("%opt", bindir + "/opt.exe"))
config.substitutions.append(("%FileCheck", bindir + "/FileCheck.exe"))
# Forward slashes: the RUN line embeds these inside a shell argument where
# backslashes get eaten by lit's quoting.
config.substitutions.append(("%plugin", plugin.replace("\\", "/")))
