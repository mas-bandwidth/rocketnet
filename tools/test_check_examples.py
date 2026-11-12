#!/usr/bin/env python3
"""Process-level tests for tools/check_examples.py: an example including a
header outside the allowed set exits nonzero, a clean example exits 0, and a
tree with zero examples exits nonzero naming the count. Each case runs the
tool as a subprocess with --root pointed at a fixture tree in a temporary
directory."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().with_name("check_examples.py")

HEADER = "int rocketnet_init( void );\nvoid rocketnet_term( void );\n"

CLEAN = """#include "rocketnet.h"
#include <stdio.h>

int main( void )
{
    if ( rocketnet_init() != 0 )
    {
        return 1;
    }
    rocketnet_term();
    return 0;
}
"""


class CheckExamplesProcessTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "sdk").mkdir()
        (self.root / "sdk" / "rocketnet.h").write_text(HEADER)
        (self.root / "examples").mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self):
        return subprocess.run([sys.executable, str(TOOL), "--root", str(self.root)],
                              capture_output=True, text=True)

    def test_clean_example_passes(self):
        (self.root / "examples" / "hello.c").write_text(CLEAN)
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("OK examples/hello.c", result.stdout)

    def test_disallowed_include_fails(self):
        (self.root / "examples" / "hello.c").write_text(CLEAN.replace("#include <stdio.h>",
                                                                       "#include <sys/socket.h>"))
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('examples/hello.c:2: #include "sys/socket.h" is not allowed', result.stderr)

    def test_zero_examples_fail(self):
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("0 files", result.stderr)


if __name__ == "__main__":
    unittest.main()
