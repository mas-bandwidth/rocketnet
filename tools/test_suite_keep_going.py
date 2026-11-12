#!/usr/bin/env python3
"""The suite runner's keep-going mode (tests/suite_runner.h, suite_keep_going),
run end to end on a four-case suite whose second and third cases fail
(tools/testdata/keep_going_suite.cpp).

    python3 tools/test_suite_keep_going.py

It builds the suite with $CXX (default c++) into a temporary directory and runs
it one job at a time (ROCKETNET_TEST_JOBS=1), so the order is the suite order:

- ROCKETNET_TEST_NIGHTLY=1 ROCKETNET_TEST_KEEP_GOING=1: exit 1; both failures
  print their check lines; the summary names both failed cases and says
  "4 of 4 collected cases ran"; the fourth case, after both failures, ran.
- ROCKETNET_TEST_NIGHTLY=1 alone: exit 1 at the first failure; the third and
  fourth cases never ran (the per-commit behaviour, unchanged).
- ROCKETNET_TEST_KEEP_GOING=1 without the nightly tier: exit 1 before any
  case runs, with the line that says why.

POSIX hosts only: the Windows runner shares suite_print_ready and suite_finish,
which carry the mode, and the MSVC build compiles them.
"""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "tools" / "testdata" / "keep_going_suite.cpp"
NAMES = [
    "test_keep_going_first_passes",
    "test_keep_going_second_fails",
    "test_keep_going_third_fails",
    "test_keep_going_fourth_passes",
]


class KeepGoing(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.work.name) / "keep_going_suite"
        compiler = os.environ.get("CXX", "c++")
        built = subprocess.run(
            [compiler, "-std=c++17", "-ffp-contract=off", "-O1", "-I", str(ROOT / "sdk"), "-I", str(ROOT / "tests"),
             str(SOURCE), "-o", str(cls.binary), "-lpthread"],
            capture_output=True, text=True)
        if built.returncode != 0:
            raise RuntimeError("keep_going_suite did not build:\n" + built.stdout + built.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def run_suite(self, **extra):
        marks = tempfile.mkdtemp(dir=self.work.name)
        env = {k: v for k, v in os.environ.items()
               if k not in ("ROCKETNET_TEST_NIGHTLY", "ROCKETNET_TEST_KEEP_GOING", "ROCKETNET_TEST_VERBOSE")}
        env.update({"ROCKETNET_TEST_JOBS": "1", "KEEP_GOING_SUITE_MARKS": marks})
        env.update(extra)
        done = subprocess.run([str(self.binary)], cwd=self.work.name, env=env, capture_output=True, text=True)
        ran = sorted(name for name in os.listdir(marks))
        return done.returncode, done.stdout + done.stderr, ran

    def test_keep_going_runs_past_both_failures(self):
        rc, out, ran = self.run_suite(ROCKETNET_TEST_NIGHTLY="1", ROCKETNET_TEST_KEEP_GOING="1")
        self.assertEqual(rc, 1, out)
        self.assertEqual(ran, sorted(NAMES), out)
        self.assertIn("check failed: test_keep_going_second_fails planted failure", out)
        self.assertIn("check failed: test_keep_going_third_fails planted failure", out)
        self.assertIn("FAILED test_keep_going_second_fails", out)
        self.assertIn("FAILED test_keep_going_third_fails", out)
        self.assertNotIn("FAILED test_keep_going_fourth_passes", out)
        self.assertIn("keep going: 2 failed; 4 of 4 collected cases ran", out)
        self.assertNotIn("ALL TESTS PASSED", out)
        # the fourth case's name is printed after both failures, as a pass
        self.assertGreater(out.index("\ntest_keep_going_fourth_passes\n"), out.index("third_fails planted failure"), out)

    def test_without_keep_going_the_first_failure_ends_the_run(self):
        rc, out, ran = self.run_suite(ROCKETNET_TEST_NIGHTLY="1")
        self.assertEqual(rc, 1, out)
        self.assertEqual(ran, sorted(NAMES[:2]), out)
        self.assertIn("check failed: test_keep_going_second_fails planted failure", out)
        self.assertNotIn("third_fails planted failure", out)
        self.assertNotIn("keep going:", out)

    def test_keep_going_outside_the_nightly_tier_is_refused(self):
        rc, out, ran = self.run_suite(ROCKETNET_TEST_KEEP_GOING="1")
        self.assertEqual(rc, 1, out)
        self.assertEqual(ran, [], out)
        self.assertIn("ROCKETNET_TEST_KEEP_GOING=1 is for the nightly tier only", out)


if __name__ == "__main__":
    if sys.platform == "win32":
        print("test_suite_keep_going.py: POSIX only (see the docstring)")
        sys.exit(0)
    unittest.main()
