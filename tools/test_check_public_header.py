#!/usr/bin/env python3
"""Tests for tools/check_public_header.py: a public declaration without
ROCKETNET_EXPORT and an #include of a non-allowed header are each refused, a
copy of the current sdk/rocketnet.h passes, and an empty header (0 lines) is
refused. Each case runs the tool as a process on a header in a temporary
directory and asserts its exit code.

The compile checks (a) take `true` as the C and C++ compiler, so these tests
exercise the text checks (b), (c), (e) and (f) without a compiler; --no-run
skips the build of tools/header_alone (d)."""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
TOOL = TOOLS / "check_public_header.py"
HEADER = TOOLS.parent / "sdk" / "rocketnet.h"
DECLARATION = "ROCKETNET_EXPORT double rocketnet_time( void );"
INCLUDE = "#include <stdbool.h>"


class CheckPublicHeaderTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.header = Path(self.tmp.name) / "rocketnet.h"
        self.text = HEADER.read_text()
        self.true = shutil.which("true")
        self.assertIsNotNone(self.true, "no `true` on PATH")

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, text):
        self.header.write_text(text)
        return subprocess.run([sys.executable, str(TOOL), "--header", str(self.header), "--no-run",
                               "--cc", self.true, "--cxx", self.true],
                              capture_output=True, text=True)

    def test_the_current_header_passes(self):
        result = self.run_tool(self.text)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("public header: clean", result.stdout)

    def test_a_declaration_without_export_is_refused(self):
        self.assertIn(DECLARATION, self.text)
        result = self.run_tool(self.text.replace(DECLARATION, "double rocketnet_time( void );", 1))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("rocketnet_time does not carry ROCKETNET_EXPORT", result.stdout)

    def test_a_non_standard_include_is_refused(self):
        self.assertIn(INCLUDE, self.text)
        result = self.run_tool(self.text.replace(INCLUDE, INCLUDE + "\n#include <stdio.h>", 1))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("#include <stdio.h> is not a standard header this API allows", result.stdout)

    def test_an_empty_header_is_refused(self):
        result = self.run_tool("")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(": 0 lines", result.stdout)


if __name__ == "__main__":
    unittest.main()
