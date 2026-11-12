#!/usr/bin/env python3
"""Tests for tools/check_toolchain_build.py: each verdict red on a planted
violation and green on a clean input, through main()'s exit code."""

import importlib.util
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TOOL = REPO_ROOT / "tools" / "check_toolchain_build.py"


def load():
    spec = importlib.util.spec_from_file_location("check_toolchain_build", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


OTOOL_12 = """bin/test:
Load command 9
      cmd LC_BUILD_VERSION
  cmdsize 32
 platform 1
    minos 12.0
      sdk 26.0
"""

OTOOL_15 = OTOOL_12.replace("minos 12.0", "minos 15.0")


class Base(unittest.TestCase):
    def setUp(self):
        self.tool = load()
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)

    def tearDown(self):
        self.dir.cleanup()

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return str(path)

    def run_main(self, *argv):
        """main() in a child process, so the exit code is the process's."""
        result = subprocess.run([sys.executable, str(TOOL), *argv], capture_output=True, text=True)
        return result.returncode, result.stdout


class WarningsTest(Base):
    def test_clean_log_is_green(self):
        log = self.write("build.log", "[100%] Built target test\n")
        self.assertEqual(self.run_main("warnings", log)[0], 0)

    def test_gcc_warning_is_red(self):
        log = self.write("build.log", "sdk/x.c:3:5: warning: unused variable 'a' [-Wunused-variable]\n")
        code, out = self.run_main("warnings", log)
        self.assertEqual(code, 1)
        self.assertIn("TOOLCHAIN-WARNINGS", out)

    def test_msvc_and_linker_warnings_are_red(self):
        for line in ("x.cpp(12,5): warning C4100: 'a': unreferenced parameter",
                     "LINK : warning LNK4098: defaultlib 'MSVCRT' conflicts"):
            log = self.write("build.log", line + "\n")
            self.assertEqual(self.run_main("warnings", log)[0], 1, line)

    def test_cmake_warning_is_red(self):
        log = self.write("configure.log", "CMake Warning:\n  Manually-specified variables were not used\n")
        self.assertEqual(self.run_main("warnings", log)[0], 1)

    def test_missing_or_empty_log_is_not_a_pass(self):
        self.assertEqual(self.run_main("warnings", str(self.root / "absent.log"))[0], 2)
        self.assertEqual(self.run_main("warnings", self.write("empty.log", ""))[0], 2)


class CompilerTest(Base):
    def record(self, c, cxx):
        self.write("build/CMakeFiles/4.2.0/CMakeCCompiler.cmake", f'set(CMAKE_C_COMPILER "{c}")\n')
        self.write("build/CMakeFiles/4.2.0/CMakeCXXCompiler.cmake", f'set(CMAKE_CXX_COMPILER "{cxx}")\n')
        return str(self.root / "build")

    def test_named_msvc_is_green(self):
        cl = "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.29.30133/bin/Hostx64/x64/cl.exe"
        self.assertEqual(self.run_main("compiler", self.record(cl, cl), "MSVC/14.29.")[0], 0)

    def test_other_msvc_is_red(self):
        cl = "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe"
        code, out = self.run_main("compiler", self.record(cl, cl), "MSVC/14.29.")
        self.assertEqual(code, 1)
        self.assertIn("TOOLCHAIN-WRONG-COMPILER", out)

    def test_linux_compiler_pair(self):
        # what CMake 4 wrote on hetzner2 for -DCMAKE_C_COMPILER=clang-18 (2026-11-12)
        build = self.record("/usr/bin/clang-18", "/usr/bin/clang++-18")
        self.assertEqual(self.run_main("compiler", build, "/clang-18", "/clang++-18")[0], 0)
        self.assertEqual(self.run_main("compiler", build, "/clang-21", "/clang++-21")[0], 1)
        # the C++ path is judged on its own
        self.assertEqual(self.run_main("compiler", build, "/clang-18", "/clang++-21")[0], 1)
        default = self.record("/usr/bin/clang", "/usr/bin/clang++")
        self.assertEqual(self.run_main("compiler", default, "/clang-18", "/clang++-18")[0], 1)

    def test_no_record_is_not_a_pass(self):
        self.assertEqual(self.run_main("compiler", str(self.root / "nothing"), "MSVC/14.29.")[0], 2)


class MinosTest(Base):
    def macho(self, name):
        path = self.root / "bin" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"\xcf\xfa\xed\xfe" + b"\0" * 28)
        return path

    def check(self, otool_text):
        out = io.StringIO()
        code = self.tool.check_minos(str(self.root / "bin"), "12.0", otool=lambda path: otool_text, out=out)
        return code, out.getvalue()

    def test_every_macho_at_minimum_is_green(self):
        self.macho("test")
        self.macho("librocketnet.dylib")
        (self.root / "bin" / "notes.txt").write_text("not a Mach-O\n")
        code, out = self.check(OTOOL_12)
        self.assertEqual(code, 0, out)
        self.assertIn("on all 2 Mach-O files", out)

    def test_newer_minos_is_red(self):
        self.macho("test")
        code, out = self.check(OTOOL_15)
        self.assertEqual(code, 1)
        self.assertIn("MINOS", out)

    def test_no_build_version_is_red(self):
        self.macho("test")
        self.assertEqual(self.check("bin/test:\n      cmd LC_SEGMENT_64\n")[0], 1)

    def test_universal_slice_below_is_red(self):
        self.macho("test")
        self.assertEqual(self.check(OTOOL_12 + OTOOL_15)[0], 1)

    def test_no_macho_is_not_a_pass(self):
        (self.root / "bin").mkdir()
        (self.root / "bin" / "notes.txt").write_text("text\n")
        self.assertEqual(self.check(OTOOL_12)[0], 2)


class UsageTest(Base):
    def test_unknown_command_is_not_a_pass(self):
        self.assertEqual(self.run_main("nonsense")[0], 2)


if __name__ == "__main__":
    unittest.main()
