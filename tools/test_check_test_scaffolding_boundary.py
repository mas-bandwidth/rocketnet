#!/usr/bin/env python3
"""Tests for tools/check_test_scaffolding_boundary.py: a fixture tree that
names a retired per-feature test switch is refused, the same tree without it
passes, and an empty tree is refused. Each case runs the tool as a process
with --repo-root on a fixture in a temporary directory (no --red-controls,
so no compiler)."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parent / "check_test_scaffolding_boundary.py"

FIXTURE = {
    "CMakeLists.txt": "option(ROCKETNET_DEV \"development build\" OFF)\n",
    "sdk/rocketnet.h": "int rocketnet_init(void);\n",
    "sdk/rocketnet_connect_token.h": (
        "#if ROCKETNET_DEV\n"
        "int rocketnet_write_connect_token_deterministic(void);\n"
        "#endif\n"
    ),
    "sdk/rocketnet_connect_token.cpp": (
        "int rocketnet_box_seal_with_ephemeral(void) { return 0; }\n"
        "int rocketnet_write_connect_token_deterministic(void) { return 0; }\n"
    ),
    "sdk/rocketnet_connection_reset.h": (
        "void rocketnet_client_connection_reset(void);\n"
        "void rocketnet_client_protocol_wipe_secrets(void);\n"
        "void rocketnet_server_connection_reset(void);\n"
        "#if ROCKETNET_DEV\n"
        "int rocketnet_reset_inventory_matches(void);\n"
        "#endif\n"
    ),
    "docs/TEST-SCAFFOLDING.md": (
        "`ROCKETNET_DEV` is the one switch. It guards "
        "`rocketnet_write_connect_token_deterministic`, "
        "`rocketnet_box_seal_with_ephemeral` and "
        "`rocketnet_reset_inventory_matches`.\n"
    ),
}


class ScaffoldingBoundaryTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def plant(self):
        for rel, text in FIXTURE.items():
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)

    def run_tool(self):
        return subprocess.run([sys.executable, str(TOOL), "--repo-root", str(self.root)],
                              capture_output=True, text=True, timeout=30)

    def test_a_clean_tree_passes(self):
        self.plant()
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Source & header boundary clean", result.stdout)

    def test_a_retired_switch_is_refused(self):
        self.plant()
        (self.root / "sdk/rocketnet_sim.c").write_text("#if ROCKETNET_TEST_SEAMS\n#endif\n")
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("names retired switch ROCKETNET_TEST_SEAMS", result.stderr)

    def test_an_empty_tree_is_refused(self):
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0 files", result.stderr)


if __name__ == "__main__":
    unittest.main()
