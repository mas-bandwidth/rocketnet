#!/usr/bin/env python3
"""Tests for tools/check_tla_ledger_merge.py: four fixture ledgers whose
.gitattributes merges them with git's union driver pass, the same ledgers
without the union driver (two appends then conflict) are refused, and an
empty tree is refused. Each case runs the tool as a process with --root on a
fixture in a temporary directory; the tool's own scratch git repository is
made inside that directory. A fake git placed first on PATH fails one
command (init, commit or a merge) the way a host setting or a missing branch
would; every such run must FAIL naming the command, never print OK on
unmerged files."""

import os
import shutil
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

# Runs the real git (LEDGER_TEST_REAL_GIT) unless the first non-option word is
# LEDGER_TEST_FAIL, which exits 128 the way git does on a refused command.
FAKE_GIT = """import os
import sys
args = sys.argv[1:]
words = []
skip = False
for arg in args:
    if skip:
        skip = False
        continue
    if arg == "-c":
        skip = True
        continue
    words.append(arg)
if words and words[0] == os.environ["LEDGER_TEST_FAIL"]:
    sys.stderr.write("fatal: planted failure of git " + words[0] + "\\n")
    sys.exit(128)
real = os.environ["LEDGER_TEST_REAL_GIT"]
os.execv(real, [real] + args)
"""


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

    def run_tool(self, env=None):
        return subprocess.run([sys.executable, str(TOOL), "--root", str(self.root)],
                              capture_output=True, text=True, timeout=30, env=env)

    def run_with_failing_git(self, command):
        real = shutil.which("git")
        self.assertIsNotNone(real, "git is not on PATH")
        bindir = Path(self.tmp.name) / "fake-git-bin"
        bindir.mkdir()
        fake = bindir / "git"
        fake.write_text(f"#!{sys.executable}\n" + FAKE_GIT)
        fake.chmod(0o755)
        env = dict(os.environ)
        env["PATH"] = str(bindir) + os.pathsep + env.get("PATH", "")
        env["LEDGER_TEST_REAL_GIT"] = real
        env["LEDGER_TEST_FAIL"] = command
        return self.run_tool(env)

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

    @unittest.skipIf(sys.platform == "win32", "the fake git is a script with a #! line")
    def test_a_failed_commit_is_refused(self):
        self.plant(UNION)
        result = self.run_with_failing_git("commit")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("git commit -q -m base failed (exit 128)", result.stdout)
        self.assertIn("LEDGER MERGE FAIL", result.stdout)
        self.assertNotIn("LEDGER MERGE OK", result.stdout)

    @unittest.skipIf(sys.platform == "win32", "the fake git is a script with a #! line")
    def test_a_merge_that_fails_without_a_conflict_is_refused(self):
        self.plant(UNION)
        result = self.run_with_failing_git("merge")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("git merge a failed (exit 128) without a conflict", result.stdout)
        self.assertNotIn("LEDGER MERGE OK", result.stdout)

    @unittest.skipIf(sys.platform == "win32", "the fake git is a script with a #! line")
    def test_a_failed_init_is_refused(self):
        self.plant(UNION)
        result = self.run_with_failing_git("init")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("git init -q failed (exit 128)", result.stdout)
        self.assertNotIn("LEDGER MERGE OK", result.stdout)

    def test_an_empty_tree_is_refused(self):
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("LEDGER MERGE FAIL 0 files of 5", result.stdout)


if __name__ == "__main__":
    unittest.main()
