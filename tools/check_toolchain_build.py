#!/usr/bin/env python3
"""Verdicts for nightly-toolchains.yml: is this build the compiler it names,
did it leave a warning, and does every Mach-O carry the deployment target.

  check_toolchain_build.py warnings LOG...
      exit 1 on any line matching WARNING (a CMake warning, a gcc/clang
      `warning:`, an MSVC or linker `warning C4100` / `warning LNK4098`),
      each printed; 2 when a log is missing or empty; 0 otherwise.

  check_toolchain_build.py compiler BUILD_DIR WANT [WANT_CXX]
      reads CMAKE_C_COMPILER and CMAKE_CXX_COMPILER from the
      CMakeFiles/<version>/CMake{C,CXX}Compiler.cmake that configure wrote and
      exits 1 unless the C path contains WANT and the C++ path WANT_CXX
      (WANT when not given): for MSVC `MSVC/14.29.` for both, for Linux the
      program names, `/clang-18` and `/clang++-18`; 2 when the record is
      missing.

  check_toolchain_build.py minos DIR MINIMUM
      runs `otool -arch all -l` on every regular file in DIR (not recursive) that is a
      Mach-O, and exits 1 unless each carries LC_BUILD_VERSION minos MINIMUM;
      2 when DIR holds no Mach-O at all (a build that wrote nothing is not a
      pass).

Every exit path prints one summary line starting TOOLCHAIN-.
"""

import glob
import os
import re
import subprocess
import sys

WARNING = re.compile(r"CMake Warning|warning:|[: ]warning [A-Z]+[0-9]+")
COMPILER = re.compile(r'^set\((CMAKE_C_COMPILER|CMAKE_CXX_COMPILER) "([^"]*)"\)', re.M)
MACHO_MAGIC = {
    b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",  # 32-bit
    b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe",  # 64-bit
    b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",  # universal
}


def warning_lines(text):
    return [(n, line) for n, line in enumerate(text.splitlines(), 1) if WARNING.search(line)]


def check_warnings(paths, out=sys.stdout):
    found = 0
    for path in paths:
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            text = ""
        if not text:
            print(f"TOOLCHAIN-LOG-MISSING {path}", file=out)
            return 2
        for n, line in warning_lines(text):
            print(f"{path}:{n}: {line.rstrip()}", file=out)
            found += 1
    if found:
        print(f"TOOLCHAIN-WARNINGS {found} warning line(s) in {' '.join(paths)}", file=out)
        return 1
    print(f"TOOLCHAIN-ZERO-WARNINGS {' '.join(paths)}", file=out)
    return 0


def compiler_paths(build_dir):
    found = {}
    pattern = os.path.join(build_dir, "CMakeFiles", "*", "CMake*Compiler.cmake")
    for path in sorted(glob.glob(pattern)):
        with open(path, encoding="utf-8", errors="replace") as f:
            for name, value in COMPILER.findall(f.read()):
                found[name] = value
    return found


def check_compiler(build_dir, want, want_cxx=None, out=sys.stdout):
    want_cxx = want if want_cxx is None else want_cxx
    found = compiler_paths(build_dir)
    if "CMAKE_C_COMPILER" not in found or "CMAKE_CXX_COMPILER" not in found:
        print(f"TOOLCHAIN-NO-RECORD no CMAKE_C_COMPILER and CMAKE_CXX_COMPILER under {build_dir}/CMakeFiles", file=out)
        return 2
    bad = 0
    for name, wanted in (("CMAKE_C_COMPILER", want), ("CMAKE_CXX_COMPILER", want_cxx)):
        print(f"{name} {found[name]} (want {wanted})", file=out)
        if wanted not in found[name]:
            bad = 1
    if bad:
        print(f"TOOLCHAIN-WRONG-COMPILER the build did not use {want} / {want_cxx}", file=out)
        return 1
    print(f"TOOLCHAIN-COMPILER {want} / {want_cxx}", file=out)
    return 0


def is_macho(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) in MACHO_MAGIC
    except OSError:
        return False


def minos_values(otool_text):
    """Every minos under an LC_BUILD_VERSION (one per slice), in order."""
    values = []
    in_build_version = False
    for line in otool_text.splitlines():
        words = line.split()
        if not words:
            continue
        if words[0] == "cmd":
            in_build_version = len(words) > 1 and words[1] == "LC_BUILD_VERSION"
        elif in_build_version and words[0] == "minos" and len(words) > 1:
            values.append(words[1])
            in_build_version = False
    return values


def check_minos(directory, minimum, otool=None, out=sys.stdout):
    if otool is None:
        def otool(path):
            return subprocess.run(["otool", "-arch", "all", "-l", path], check=True, capture_output=True, text=True).stdout
    checked = 0
    bad = 0
    for name in sorted(os.listdir(directory)):
        path = os.path.join(directory, name)
        if not os.path.isfile(path) or os.path.islink(path) or not is_macho(path):
            continue
        checked += 1
        values = minos_values(otool(path))
        if not values or any(v != minimum for v in values):
            print(f"MINOS {path} {','.join(values) or 'none'} (want {minimum})", file=out)
            bad = 1
    if checked == 0:
        print(f"TOOLCHAIN-NO-MACHO no Mach-O in {directory}", file=out)
        return 2
    if bad:
        print(f"TOOLCHAIN-MINOS-WRONG of {checked} Mach-O files", file=out)
        return 1
    print(f"TOOLCHAIN-MINOS {minimum} on all {checked} Mach-O files", file=out)
    return 0


def main(argv):
    if len(argv) >= 2 and argv[0] == "warnings":
        return check_warnings(argv[1:])
    if len(argv) in (3, 4) and argv[0] == "compiler":
        return check_compiler(*argv[1:])
    if len(argv) == 3 and argv[0] == "minos":
        return check_minos(argv[1], argv[2])
    print(__doc__, file=sys.stderr)
    print("TOOLCHAIN-USAGE", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
