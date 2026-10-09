import os
import lit.formats

config.name = "RevenantUnoutline"
config.test_format = lit.formats.ShTest(False)
config.suffixes = [".ll"]
config.test_source_root = os.path.dirname(os.path.abspath(__file__))

bindir = r"C:\Users\User\RustroverProjects\llvm-project\build-revenant\bin"
plugin = os.environ.get("REVENANT_UNOUTLINE_PLUGIN", "")
if not plugin:
    plugin = os.path.normpath(os.path.join(
        config.test_source_root, "..", "..", "..", "..",
        "build-rv-unoutline", "RevenantTransformUnoutline.dll"))
config.substitutions.append(("%opt", bindir + r"\opt.exe"))
config.substitutions.append(("%FileCheck", bindir + r"\FileCheck.exe"))
config.substitutions.append(("%plugin", plugin))
