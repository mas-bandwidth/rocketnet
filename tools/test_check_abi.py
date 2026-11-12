#!/usr/bin/env python3
"""Tests for tools/check_abi.py: the recorded 1.x ABI refuses a moved field,
a removed export and an unrecorded field, and passes the current header.
Each case edits a copy of sdk/rocketnet.h or tools/exports.txt in a
temporary directory and compiles it with $CC (default cc)."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_abi

RECEIPT = """struct rocketnet_receipt_t
{"""


class CheckAbiTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.header = Path(self.tmp.name) / "rocketnet.h"
        self.exports = Path(self.tmp.name) / "exports.txt"
        self.header.write_text(check_abi.HEADER.read_text())
        self.exports.write_text(check_abi.EXPORTS.read_text())
        self.baseline = check_abi.read_baseline(check_abi.BASELINE)

    def tearDown(self):
        self.tmp.cleanup()

    def problems(self):
        current = check_abi.measured_lines(self.header, self.exports, os.environ.get("CC", "cc"))
        return check_abi.compare(self.baseline, current)

    def test_the_current_header_matches(self):
        self.assertEqual(self.problems(), [])

    def test_a_moved_field_is_a_break(self):
        text = self.header.read_text()
        self.assertIn(RECEIPT, text)
        self.header.write_text(text.replace(RECEIPT, RECEIPT + "\n    uint32_t moved_in;", 1))
        problems = self.problems()
        self.assertIn("ABI BREAK: field rocketnet_receipt_t status 4 4 (recorded in abi_v1.txt)", problems)
        self.assertTrue(any(p.startswith("NOT RECORDED: field rocketnet_receipt_t moved_in") for p in problems), problems)

    def test_a_removed_export_is_a_break(self):
        names = check_abi.read_exports(self.exports)
        self.exports.write_text("\n".join(name for name in names if name != "rocketnet_init") + "\n")
        self.assertEqual(self.problems(), ["ABI BREAK: export rocketnet_init (recorded in abi_v1.txt)"])

    def test_an_unrecorded_export_must_be_recorded(self):
        self.exports.write_text(self.exports.read_text() + "rocketnet_new_function\n")
        self.assertEqual(self.problems(),
                         ["NOT RECORDED: export rocketnet_new_function (a 1.x addition is added to abi_v1.txt)"])

    def test_zero_inputs_is_an_error(self):
        empty = Path(self.tmp.name) / "empty.txt"
        empty.write_text("")
        script = str(Path(__file__).resolve().parent / "check_abi.py")
        run = subprocess.run([sys.executable, script, "--header", str(empty), "--exports", str(empty),
                              "--baseline", str(empty)], capture_output=True, text=True)
        self.assertNotEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("check_abi: 0 structs in", run.stderr)
        self.assertIn("check_abi: 0 exports in", run.stderr)
        self.assertIn("check_abi: 0 baseline lines in", run.stderr)


if __name__ == "__main__":
    unittest.main()
