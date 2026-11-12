#!/usr/bin/env python3
"""Tests for tools/check_tla_ledger_merge.py: four fixture ledgers whose
.gitattributes merges them with git's union driver pass, the same ledgers
without the union driver (two appends then conflict) are refused, and an
empty tree is refused. Each case runs the tool as a process with --root on a
fixture in a temporary directory; the tool's own scratch git repository is
made inside that directory."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parent / "check_tla_ledger_merge.py"

LEDGERS = {
    "proofs/tla/CASES.tsv": "config\tmodule\texpected\nMCLink.cfg\tMCLink.tla\tpass\n",
    "proofs/tla/COVERAGE.tsv": "machine\tcode\tmodule\nlink\tsdk/link.cpp\tLink\n",
    "proofs/tla/RUNS.tsv": "config\tmodule\tresult\nMCLink.cfg\tMCLink.tla\tPASS\n",
    "proofs/tla/README.md": "# TLA\n\n| group | about |\n| --- | --- |\n| `link` | Lossy link. |\n\nEnd.\n",
}

UNION = "".join(f"{rel} merge=union\n" for rel in LEDGERS)


class LedgerMergeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def plant(self, attributes):
        for rel, text in LEDGERS.items():
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        (self.root / ".gitattributes").write_text(attributes)

    def run_tool(self):
        return subprocess.run([sys.executable, str(TOOL), "--root", str(self.root)],
                              capture_output=True, text=True, timeout=30)

    def test_union_ledgers_merge_clean(self):
        self.plant(UNION)
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("LEDGER MERGE OK 4 ledgers, 2 branches", result.stdout)

    def test_ledgers_without_union_are_refused(self):
        self.plant("* text=auto eol=lf\n")
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("CONFLICT", result.stdout)
        self.assertIn("LEDGER MERGE FAIL", result.stdout)

    def test_an_empty_tree_is_refused(self):
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("LEDGER MERGE FAIL 0 files of 5", result.stdout)


if __name__ == "__main__":
    unittest.main()
