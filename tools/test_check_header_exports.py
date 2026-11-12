#!/usr/bin/env python3
"""Process-level tests for tools/check_header_exports.py: a ROCKETNET_EXPORT
declaration missing from the list exits nonzero, a matching header and list
exit 0, and a header with zero exports exits nonzero naming the count. Each
case runs the tool as a subprocess on fixtures in a temporary directory."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().with_name("check_header_exports.py")

HEADER = """#define ROCKETNET_EXPORT
/* ROCKETNET_EXPORT int rocketnet_commented_out( void ); */
ROCKETNET_EXPORT int rocketnet_alpha( void );
ROCKETNET_EXPORT void rocketnet_beta( int value );
"""


class CheckHeaderExportsProcessTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, header_text, exports_text):
        header = self.dir / "rocketnet.h"
        exports = self.dir / "exports.txt"
        header.write_text(header_text)
        exports.write_text(exports_text)
        return subprocess.run([sys.executable, str(TOOL), "--header", str(header), "--exports", str(exports)],
                              capture_output=True, text=True)

    def test_clean_header_and_list_pass(self):
        result = self.run_tool(HEADER, "rocketnet_alpha\nrocketnet_beta\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("2 declared, list matches", result.stdout)

    def test_unlisted_export_fails(self):
        result = self.run_tool(HEADER + "ROCKETNET_EXPORT int rocketnet_planted( void );\n",
                               "rocketnet_alpha\nrocketnet_beta\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing from the list: rocketnet_planted", result.stderr)

    def test_zero_exports_fail(self):
        result = self.run_tool("", "")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("0 declared", result.stderr)


if __name__ == "__main__":
    unittest.main()
