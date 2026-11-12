#!/usr/bin/env python3
"""Tests for tools/check_sdk_sources.py: an sdk/ source that
cmake/RocketnetSources.cmake neither lists nor excludes is refused, a fully
listed tree passes, and a tree with 0 sdk/ sources, 0 listed files or no files
at all is refused. Each case runs the tool as a process on a fixture root in a
temporary directory and asserts its exit code.

The tool runs `cmake -P` to read the list. A fake `cmake` on PATH stands in
for it: it reads the fixture's set(ROCKETNET_SDK_SOURCES_<GROUP> ...) lines and
writes "<GROUP> <path>" rows in the real script's group order, so no cmake is
needed."""

import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parent / "check_sdk_sources.py"

FAKE_CMAKE = """#!{python}
import re, sys
output = next(a.split("=", 1)[1] for a in sys.argv if a.startswith("-DROCKETNET_SDK_SOURCE_LIST_OUTPUT="))
script = sys.argv[sys.argv.index("-P") + 1]
groups = dict((m.group(1), m.group(2).split()) for m in
              re.finditer(r"set\\(ROCKETNET_SDK_SOURCES_(\\w+)((?:\\s+[^)\\s]+)*)\\s*\\)", open(script).read()))
with open(output, "w") as out:
    for group in ("COMMON", "LINUX", "APPLE", "WINDOWS", "EXCLUDED"):
        for source in groups.get(group, []):
            out.write(group + " " + source + "\\n")
"""

LISTS = """set(ROCKETNET_SDK_SOURCES_COMMON sdk/rocketnet.cpp)
set(ROCKETNET_SDK_SOURCES_LINUX sdk/rocketnet_socket_linux.cpp)
set(ROCKETNET_SDK_SOURCES_EXCLUDED sdk/rocketnet_sodium_test.c)
"""
SOURCES = ("rocketnet.cpp", "rocketnet_socket_linux.cpp", "rocketnet_sodium_test.c")


class CheckSdkSourcesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.bin = base / "bin"
        self.bin.mkdir()
        cmake = self.bin / "cmake"
        cmake.write_text(FAKE_CMAKE.format(python=sys.executable))
        cmake.chmod(cmake.stat().st_mode | stat.S_IXUSR)
        self.root = base / "root"
        (self.root / "cmake").mkdir(parents=True)
        (self.root / "sdk").mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def plant(self, lists, sources):
        (self.root / "cmake" / "RocketnetSources.cmake").write_text(lists)
        for name in sources:
            (self.root / "sdk" / name).write_text("// fixture\n")

    def run_tool(self, root):
        env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ.get("PATH", ""))
        return subprocess.run([sys.executable, str(TOOL), "--root", str(root)],
                              capture_output=True, text=True, env=env)

    def test_a_listed_tree_passes(self):
        self.plant(LISTS, SOURCES)
        result = self.run_tool(self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("test_sdk_sources_unlisted_is_red", result.stdout)

    def test_an_unlisted_source_is_refused(self):
        self.plant(LISTS, SOURCES + ("rocketnet_forgotten.cpp",))
        result = self.run_tool(self.root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sdk/rocketnet_forgotten.cpp is neither listed nor excluded", result.stderr)

    def test_zero_sdk_sources_is_refused(self):
        self.plant(LISTS, ())
        result = self.run_tool(self.root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("0 files", result.stderr)

    def test_zero_listed_files_is_refused(self):
        self.plant("# no lists\n", ())
        (self.root / "sdk" / "rocketnet.cpp").write_text("// fixture\n")
        result = self.run_tool(self.root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("lists 0 files", result.stderr)

    def test_no_lists_and_no_sources_is_refused(self):
        self.plant("# no lists\n", ())
        result = self.run_tool(self.root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("0 files", result.stderr)

    def test_an_empty_root_is_refused(self):
        with tempfile.TemporaryDirectory() as empty:
            result = self.run_tool(Path(empty))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("0 files", result.stderr)


if __name__ == "__main__":
    unittest.main()
