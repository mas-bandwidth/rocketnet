#!/usr/bin/env python3
"""Tests for tools/check_platform_policy.py: a supported row whose runner
label no workflow job names is refused, a table whose every claim is backed by
its workflow passes, and a table with 0 rows (or no files at all) is refused.
Each case runs the tool as a process on a fixture root in a temporary
directory and asserts its exit code. The fixture's tests/functional/ holds
exactly the listed wildcard lines, so the wildcard check is clean."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_platform_policy

TOOL = Path(__file__).resolve().parent / "check_platform_policy.py"

HEADER = ("| Operating system | Architecture | Compiler | Transports | Tier | Workflow "
          "| Runner label | Per-commit functional |\n"
          "|---|---|---|---|---|---|---|---|\n")
SUPPORTED = ("| Linux | x64 | gcc | portable | supported | `.github/workflows/ci.yml` "
             "| `{label}` | full: Functional tests |\n")
UNSUPPORTED = "| Plan 9 | x64 | kencc | none | unsupported | - | - | - |\n"
WORKFLOW = """name: ci
on: push
jobs:
  functional:
    runs-on: ubuntu-24.04
    steps:
      - name: Functional tests
        run: make test
"""


class CheckPlatformPolicyTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        functional = self.root / "tests" / "functional"
        functional.mkdir(parents=True)
        lines = {}
        for name, line in check_platform_policy.WILDCARD_ALLOWED:
            lines.setdefault(name, []).append(line)
        for name, listed in lines.items():
            (self.root / name).write_text("\n".join(listed) + "\n")
        for name in check_platform_policy.FUNCTIONAL_TIER_SOURCES:
            if not (self.root / name).exists():
                (self.root / name).write_text("int tier_source = 0;\n")
        (self.root / ".github" / "workflows").mkdir(parents=True)
        (self.root / ".github" / "workflows" / "ci.yml").write_text(WORKFLOW)
        (self.root / "docs").mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, root):
        return subprocess.run([sys.executable, str(TOOL), "--root", str(root)],
                              capture_output=True, text=True)

    def write_table(self, rows):
        (self.root / "docs" / "PLATFORMS.md").write_text("# Platforms\n\n" + HEADER + rows + "\n")

    def test_a_wildcard_bind_in_a_tier_source_outside_functional_is_refused(self):
        # the macOS transport binary is built from tests/test_msgx.cpp, not tests/functional/
        self.write_table(SUPPORTED.format(label="ubuntu-24.04"))
        (self.root / "tests" / "test_msgx.cpp").write_text('const char * bind = "0.0.0.0:40000";\n')
        result = self.run_tool(self.root)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("tests/test_msgx.cpp:1: wildcard address in the functional tier is not on the listed set",
                      result.stderr)

    def test_backed_claims_pass(self):
        self.write_table(SUPPORTED.format(label="ubuntu-24.04") + UNSUPPORTED)
        result = self.run_tool(self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("1 supported, 0 nightly, 1 unsupported", result.stdout)

    def test_a_label_no_workflow_names_is_refused(self):
        self.write_table(SUPPORTED.format(label="ubuntu-22.04-arm") + UNSUPPORTED)
        result = self.run_tool(self.root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("runner label token 'ubuntu-22.04-arm'", result.stderr)

    def test_zero_rows_is_refused(self):
        self.write_table("")
        result = self.run_tool(self.root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("0 rows", result.stderr)

    def test_an_empty_root_is_refused(self):
        with tempfile.TemporaryDirectory() as empty:
            result = self.run_tool(Path(empty))
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
