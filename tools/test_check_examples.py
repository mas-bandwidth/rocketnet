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

    def scanf_example(self, declarations):
        return CLEAN.replace("    if ( rocketnet_init() != 0 )",
                             declarations + "    if ( scanf( \"%d %d\", &a, &b ) != 2 ) { return 1; }\n"
                             "    if ( rocketnet_init() != 0 )")

    def test_scanf_targets_in_a_declarator_list_need_initializers(self):
        for declarations, refused in (
                ("    int a, b;\n", ["a", "b"]),
                ("    int a = 0, b;\n", ["b"]),
                ("    struct reading a;\n    long b;\n", ["a", "b"]),
                ("    unsigned int * a, b = 0;\n", ["a"]),
                ("    int a = 0, b = 0;\n", []),
                ("    int a = 0;\n    int b[ 2 ] = { 0, 0 };\n", [])):
            (self.root / "examples" / "hello.c").write_text(self.scanf_example(declarations))
            result = self.run_tool()
            named = [line.split(": ")[1].split(" ")[0] for line in result.stderr.splitlines()
                     if "written by scanf" in line]
            self.assertEqual(named, refused, declarations + result.stderr)
            self.assertEqual(result.returncode, 1 if refused else 0, declarations + result.stderr)

    def test_zero_examples_fail(self):
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("0 files", result.stderr)


if __name__ == "__main__":
    unittest.main()
