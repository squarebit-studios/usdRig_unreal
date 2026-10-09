"""
Refreshes the plugin's vendored RigExec runtime from a usdRig checkout.

    python Tools/update_rigexec_lib.py [<usdRig root>] [--force]

The plugin links rigExecRuntime and rigExecBinary as static libraries
(plain C++17, no USD). This copies them from <usdRig>/build, and copies the
headers runtime.h reaches through its includes from <usdRig>/libs, into
Plugins/RigExec/Source/ThirdParty/RigExecLib. The previous copy is removed
first so a header the runtime stopped using does not linger. SOURCE.txt
records the commit the copy came from.

Build usdRig first (bin\\build_rigexec.bat): the libraries must match the
headers they are copied with, so the copy is refused while any runtime
source or copied header is newer than the libraries (--force overrides).
The usdRig root defaults to RIGEXEC_ROOT, then to the sibling ../usdRig.
"""

import os
import re
import shutil
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
_DEST = os.path.join(_REPO, "Plugins", "RigExec", "Source", "ThirdParty", "RigExecLib")
_LIBS = ("rigExecRuntime.lib", "rigExecBinary.lib")
_ENTRY = "rigExecRuntime/runtime.h"


def HeaderClosure(libs, *entries):
    """The entries and every quoted include reachable from them, as paths
    relative to `libs`; a quoted include resolves against `libs`, then its
    own folder."""
    seen = set()
    todo = list(entries)
    while todo:
        header = todo.pop()
        if header in seen:
            continue
        seen.add(header)
        with open(os.path.join(libs, header), encoding="utf-8") as f:
            text = f.read()
        for name in re.findall(r'#include\s+"([^"]+)"', text):
            for candidate in (name, os.path.join(os.path.dirname(header), name)):
                candidate = os.path.normpath(candidate).replace(os.sep, "/")
                if os.path.isfile(os.path.join(libs, candidate)):
                    todo.append(candidate)
                    break
    return sorted(seen)


def _Git(root, *args):
    try:
        return subprocess.run(["git", "-C", root] + list(args), capture_output=True,
                              text=True, check=True).stdout.rstrip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def main(argv):
    args = [a for a in argv[1:] if a != "--force"]
    root = args[0] if args else (
        os.environ.get("RIGEXEC_ROOT") or os.path.join(_REPO, os.pardir, "usdRig"))
    root = os.path.abspath(root)
    libs = os.path.join(root, "libs")
    build = os.environ.get("RIGEXEC_BUILD_DIR") or os.path.join(root, "build")
    missing = [n for n in _LIBS if not os.path.isfile(os.path.join(build, n))]
    if not os.path.isfile(os.path.join(libs, _ENTRY)) or missing:
        sys.exit("not a built usdRig checkout: %s (missing %s)" %
                 (root, ", ".join(missing) or os.path.join(libs, _ENTRY)))

    headers = HeaderClosure(libs, _ENTRY)
    # Each library must be at least as new as everything compiled into it --
    # its folder's sources and every header they reach -- or the copy pairs
    # headers with code built from different ones. The runtime reaches every
    # header copied here.
    stale = set()
    for name in _LIBS:
        sub = os.path.splitext(name)[0]
        built = os.path.getmtime(os.path.join(build, name))
        inputs = HeaderClosure(libs, *[sub + "/" + f for f in os.listdir(os.path.join(libs, sub))
                                       if f.endswith((".cpp", ".h"))])
        stale.update(f for f in inputs if os.path.getmtime(os.path.join(libs, f)) > built)
    stale = sorted(stale)
    if stale and "--force" not in argv:
        sys.exit("the usdRig build is older than its sources; rebuild it "
                 "(bin\\build_rigexec.bat) or pass --force:\n  " + "\n  ".join(stale))

    for sub in ("include", "lib"):
        shutil.rmtree(os.path.join(_DEST, sub), ignore_errors=True)
    for header in headers:
        target = os.path.join(_DEST, "include", header)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copy2(os.path.join(libs, header), target)
    os.makedirs(os.path.join(_DEST, "lib", "Win64"))
    for name in _LIBS:
        shutil.copy2(os.path.join(build, name), os.path.join(_DEST, "lib", "Win64", name))

    commit = _Git(root, "rev-parse", "HEAD") or "unknown"
    dirty = _Git(root, "status", "--porcelain", "--", "libs/rigExecRuntime",
                 "libs/rigExecBinary", "libs/rigExecMath")
    with open(os.path.join(_DEST, "SOURCE.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("usdRig commit: %s\n" % commit)
        if dirty:
            f.write("uncommitted changes under libs/ when copied:\n")
            f.write("".join("  %s\n" % line for line in dirty.splitlines()))
        f.write("headers:\n")
        f.write("".join("  %s\n" % h for h in headers))
        f.write("libraries (lib/Win64): %s\n" % ", ".join(_LIBS))
    print("copied %d headers and %d libraries from %s" % (len(headers), len(_LIBS), root))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
