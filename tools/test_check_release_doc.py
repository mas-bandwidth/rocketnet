#!/usr/bin/env python3
"""Tests for tools/check_release_doc.py: a version site the release-check job
reads but docs/RELEASING.md does not name is refused, a document naming every
site passes, and a job naming 0 version sites (or no files at all) is refused.
Each case runs the tool as a process on a fixture root in a temporary
directory and asserts its exit code."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parent / "check_release_doc.py"

WORKFLOW = """name: release-check
on: push
jobs:
  version-consistency:
    runs-on: ubuntu-24.04
    steps:
      - run: grep VERSION sdk/rocketnet.h
      - run: python3 tools/release_version.py
  other:
    runs-on: ubuntu-24.04
    steps:
      - run: cat docs/UNRELATED.md
"""
NO_SITES = """name: release-check
on: push
jobs:
  version-consistency:
    runs-on: ubuntu-24.04
    steps:
      - run: echo nothing
"""
DOC_ALL = "The version sites are sdk/rocketnet.h and tools/release_version.py.\n"
DOC_MISSING = "The version site is sdk/rocketnet.h.\n"


class CheckReleaseDocTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def plant(self, workflow, doc):
        (self.root / ".github" / "workflows").mkdir(parents=True, exist_ok=True)
        (self.root / "docs").mkdir(exist_ok=True)
        (self.root / ".github" / "workflows" / "release-check.yml").write_text(workflow)
        (self.root / "docs" / "RELEASING.md").write_text(doc)

    def run_tool(self, root):
        return subprocess.run([sys.executable, str(TOOL), "--root", str(root)],
                              capture_output=True, text=True)

    def test_every_site_named_passes(self):
        self.plant(WORKFLOW, DOC_ALL)
        result = self.run_tool(self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("names 2 version site(s)", result.stdout)

    def test_an_unnamed_site_is_refused(self):
        self.plant(WORKFLOW, DOC_MISSING)
        result = self.run_tool(self.root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not name version site: tools/release_version.py", result.stderr)

    def test_zero_version_sites_is_refused(self):
        self.plant(NO_SITES, DOC_ALL)
        result = self.run_tool(self.root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("0 version sites", result.stderr)

    def test_an_empty_root_is_refused(self):
        result = self.run_tool(self.root)
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
