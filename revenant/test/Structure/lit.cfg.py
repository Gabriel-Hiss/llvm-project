import os
import lit.formats

config.name = "RevenantStructure"
config.test_format = lit.formats.ShTest(True)
config.suffixes = [".ll"]
config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = config.test_source_root

# Standalone build (build-rv) or build-revenant tool dirs.
tool_dir = os.environ.get("RV_STRUCTURE_TOOL_DIR", "")
filecheck = os.environ.get("FILECHECK", "FileCheck")

tool = "rv-structure-tool"
if tool_dir:
    tool = os.path.join(tool_dir, "rv-structure-tool")

config.substitutions.append(("%{rv-structure-tool}", tool))
config.substitutions.append(("%{FileCheck}", filecheck))
