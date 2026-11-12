#!/usr/bin/env python3
"""Tests for tools/check_transport_matrix.py: a copy of this repository's
registry, ci.yml, nightly.yml and docs passes; the same copy with one more transport row
that no TRANSPORT_MATRIX cell runs is refused by name; a registry with no
rows and an empty tree are refused. Each case runs the tool as a process
with --root on a fixture in a temporary directory."""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
TOOL = TOOLS / "check_transport_matrix.py"
REPO = TOOLS.parent
INPUTS = ("sdk/rocketnet_drivers.inc", ".github/workflows/ci.yml", ".github/workflows/nightly.yml", "sdk/rocketnet.h", "docs/USAGE.md", "docs/TRANSPORTS.md")

# pin-only, so the auto orders the docs state are unchanged and the one gap
# is the missing matrix cell
SPARE_ROW = ('ROCKETNET_TRANSPORT( SPARE, "spare", true, rocketnet_spare_transport, '
             'ROCKETNET_TRANSPORT_LINUX, ROCKETNET_TRANSPORT_PIN_ONLY, ROCKETNET_TRANSPORT_PIN_ONLY )\n')


class TransportMatrixTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def plant(self):
        for rel in INPUTS:
            (self.root / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPO / rel, self.root / rel)

    def run_tool(self):
        return subprocess.run([sys.executable, str(TOOL), "--root", str(self.root)],
                              capture_output=True, text=True, timeout=30)

    def test_the_repository_inputs_pass(self):
        self.plant()
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("TRANSPORT-MATRIX OK", result.stdout)

    def test_a_transport_with_no_cell_is_refused(self):
        self.plant()
        registry = self.root / "sdk/rocketnet_drivers.inc"
        registry.write_text(registry.read_text() + SPARE_ROW)
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("transport spare: no TRANSPORT_MATRIX_LINUX cell with server transport spare", result.stdout)

    def test_a_registry_with_no_rows_is_refused(self):
        self.plant()
        (self.root / "sdk/rocketnet_drivers.inc").write_text("")
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0 ROCKETNET_TRANSPORT rows", result.stdout)

    def test_an_empty_tree_is_refused(self):
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0 files of 6", result.stdout)


if __name__ == "__main__":
    unittest.main()
