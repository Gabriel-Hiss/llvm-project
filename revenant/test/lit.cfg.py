# Root lit config for the whole Revenant suite. One command runs everything:
#
#   python C:\Users\User\RustroverProjects\Revenant\llvm-project\build-revenant\bin\llvm-lit.py -v <worktree>\revenant\test
#
# Environment overrides (optional):
#   REVENANT_OPT          path to the static revenant-opt binary
#   REVENANT_BUILD_DIR / RV_BUILD_DIR
#                         build dir that holds the tools (searched below)
#   RV_OPT                path to stock opt (default: the shared bin dir)
#   RV_FILECHECK          path to FileCheck
#   RV_STRUCTURE_TOOL_DIR directory holding rv-structure-tool
#
# Tool lookup order: env var, then <worktree>/build-rv*/bin, then the shared
# build-revenant bin dir. The static revenant-opt links every landed
# revenant-<name> pass, so no RUN line needs -load-pass-plugin.

import os
import lit.formats

config.name = "Revenant"
# Internal shell: llvm-lit on this box resolves bash to the WSL stub in
# System32, so the external-shell probe fails; the internal shell also gives
# `env VAR=x cmd` for RUN lines that need it.
config.test_format = lit.formats.ShTest(execute_external=False)
config.suffixes = [".ll"]
config.test_source_root = os.path.dirname(os.path.abspath(__file__))
config.test_exec_root = config.test_source_root
# Contract examples (ir-contract) and the corpus measurement scripts
# (meta-mode) are not lit tests.
config.excludes = ["ir-contract", "meta-mode", "Inputs"]

worktree = os.path.normpath(os.path.join(config.test_source_root, "..", ".."))
shared_bin = os.environ.get(
    "RV_LLVM_BIN",
    r"C:\Users\User\RustroverProjects\Revenant\llvm-project\build-revenant\bin",
)


def _first_env(*names):
    for name in names:
        value = os.environ.get(name, "").strip()
        if value:
            return value
    return ""


def _file(*candidates):
    for path in candidates:
        if path and os.path.isfile(path):
            return path
    return ""


def _join(*parts):
    return os.path.join(*parts).replace("\\", "/")


def _build_dirs():
    # Newest build dir first: agents name them build-rv-<slice>.
    dirs = []
    env_dir = _first_env("REVENANT_BUILD_DIR", "RV_BUILD_DIR")
    if env_dir:
        dirs.append(env_dir)
    if os.path.isdir(worktree):
        names = [n for n in os.listdir(worktree) if n.startswith("build-rv")]
        for name in sorted(names, reverse=True):
            dirs.append(_join(worktree, name))
    return dirs


def _tool_in(build_dir, name):
    # Where landed slice binaries live: <build>/bin (add_llvm_tool default),
    # <build>/tools/<name>/<name>, and <build>/lib/<Area>/tool/<name>
    # (add_llvm_executable inside a lib subdirectory). Windows: the platform
    # adds the .exe suffix.
    bases = [
        _join(build_dir, "bin"),
        build_dir,
        _join(build_dir, "tools", name),
        # add_llvm_executable under revenant/frontend/clang builds into
        # <build>/frontend/clang (revenant-clang lives there).
        _join(build_dir, "frontend", "clang"),
    ]
    bases.extend(sorted(_join(build_dir, "lib", area, "tool")
                        for area in _lib_areas(build_dir)))
    for base in bases:
        for suffix in ("", ".exe"):
            path = _join(base, name + suffix)
            if os.path.isfile(path):
                return path
    return ""


def _lib_areas(build_dir):
    lib_dir = _join(build_dir, "lib")
    if not os.path.isdir(lib_dir):
        return []
    return [name for name in os.listdir(lib_dir)
            if os.path.isdir(_join(lib_dir, name, "tool"))]


def _tool(name, env_names):
    path = _first_env(*env_names)
    if path:
        return path
    for build_dir in _build_dirs():
        found = _tool_in(build_dir, name)
        if found:
            return found
    return _file(_join(shared_bin, name), _join(shared_bin, name + ".exe"))


# Stock opt: the shared build always has it; a worktree build dir may too.
opt = _tool("opt", ("RV_OPT",))
filecheck = _tool("FileCheck", ("RV_FILECHECK", "FILECHECK"))
revenant_opt = _tool("revenant-opt", ("REVENANT_OPT", "RV_REVENANT_OPT"))
revenant_decompile = _tool("revenant-decompile", ("REVENANT_DECOMPILE",))
revenant_clang = _tool("revenant-clang", ("REVENANT_CLANG",))
revenant_detect = _tool("revenant-detect", ("REVENANT_DETECT",))

# Parity corpus for the tests that need a real PE (producer detection).
# Opt-in: set REVENANT_CORPUS to the corpus root; tests requiring it are
# skipped (REQUIRES) when it is unset or absent.
corpus = _first_env("REVENANT_CORPUS", "RV_CORPUS")
if corpus and os.path.isdir(corpus):
    config.substitutions.append(("%revenant-corpus", corpus.replace("\\", "/")))
    config.available_features.add("revenant-corpus")

# rv-structure-tool ships with the Structure slice build.
rv_tool_dir = _first_env("RV_STRUCTURE_TOOL_DIR")
rv_structure_tool = _first_env("RV_STRUCTURE_TOOL")
if not rv_structure_tool:
    if rv_tool_dir:
        rv_structure_tool = _join(rv_tool_dir, "rv-structure-tool")
    else:
        for build_dir in _build_dirs():
            found = _tool_in(build_dir, "rv-structure-tool")
            if found:
                rv_structure_tool = found
                break
if not rv_structure_tool:
    rv_structure_tool = "rv-structure-tool"

if opt:
    config.substitutions.append(("%opt", opt))
if filecheck:
    config.substitutions.append(("%FileCheck", filecheck))
    config.substitutions.append(("%{FileCheck}", filecheck))
if revenant_opt:
    config.substitutions.append(("%revenant-opt", revenant_opt))
    config.substitutions.append(("%{revenant-opt}", revenant_opt))
if revenant_decompile:
    config.substitutions.append(("%revenant-decompile", revenant_decompile))
    config.substitutions.append(("%{revenant-decompile}", revenant_decompile))
if revenant_detect:
    config.substitutions.append(("%revenant-detect", revenant_detect))
    config.substitutions.append(("%{revenant-detect}", revenant_detect))
    config.available_features.add("revenant-detect")
if revenant_clang:
    config.substitutions.append(("%revenant-clang", revenant_clang))
    config.substitutions.append(("%{revenant-clang}", revenant_clang))
    config.available_features.add("revenant-clang")
config.substitutions.append(("%{rv-structure-tool}", rv_structure_tool))
config.substitutions.append(("%{rv-structure-tool-exe}", rv_structure_tool))

missing = [
    name
    for name, path in (
        ("%opt", opt),
        ("%FileCheck", filecheck),
        ("%revenant-opt", revenant_opt),
        ("%revenant-decompile", revenant_decompile),
    )
    if not path
]
if missing:
    lit_config.fatal(
        "revenant lit: could not resolve %s; set REVENANT_BUILD_DIR or build "
        "revenant-opt" % ", ".join(missing)
    )
