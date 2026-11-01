#!/usr/bin/env python3
"""Tests for the MemorySanitizer gate's verdicts in tools/run_msan.sh."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
RUN_MSAN = REPO_ROOT / "tools" / "run_msan.sh"

KNOWN_TOP = "known_frame tools/fixture.cpp:2:1"
UNKNOWN_TOP = "unlisted_frame tools/fixture.cpp:1:1"

FAKE_TEMPLATE = '''#!/usr/bin/env python3
import os
import sys

options = os.environ.get("MSAN_OPTIONS", "")
log_path = ""
for field in options.split(":"):
    if field.startswith("log_path="):
        log_path = field[len("log_path="):]
if log_path:
    with open(log_path + "." + str(os.getpid()), "w") as handle:
        handle.write(%(warning)r)
sys.stdout.write(%(stdout)r)
sys.exit(%(status)d)
'''


FATAL_TEXT = (
    "==1==ERROR: MemorySanitizer: requested allocation size 0x2d25c8997 exceeds"
    " maximum supported size of 0x200000000\n"
    "    #0 0x1000 in malloc\n"
    "    #1 0x2000 in test_server_stats_current_every_update tools/fixture.cpp:3:1\n"
    "SUMMARY: MemorySanitizer: allocation-size-too-big in malloc\n"
)


def warning_text(top):
    if top is None:
        return ""
    if top == "fatal":
        return FATAL_TEXT
    return (
        "==1==WARNING: MemorySanitizer: use-of-uninitialized-value\n"
        "    #0 0x1000 in %s\n"
        "    #1 0x2000 in main tools/fixture.cpp:1:1\n"
        "SUMMARY: MemorySanitizer: use-of-uninitialized-value\n" % top
    )


class RunMsanGateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=REPO_ROOT)

    def tearDown(self):
        self.tmp.cleanup()

    def run_gate(self, stdout, status, top):
        binary = Path(self.tmp.name) / "fake-test"
        binary.write_text(
            FAKE_TEMPLATE
            % {"warning": warning_text(top), "stdout": stdout, "status": status}
        )
        binary.chmod(0o755)
        known = Path(self.tmp.name) / "known.txt"
        known.write_text("# the fixture's known list\n%s\n" % KNOWN_TOP)
        env = dict(os.environ)
        env["RUN_MSAN_TEST_BIN"] = str(binary)
        env["RUN_MSAN_KNOWN_FILE"] = str(known)
        result = subprocess.run(
            ["bash", str(RUN_MSAN)],
            cwd=str(REPO_ROOT),
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        return result.returncode, result.stdout

    def test_signal_fail_line_fails_even_with_known_finding(self):
        stdout = (
            "check failed: "
            "test_wire_totals_staged_finish_counts_exactly signal 5\n"
        )
        code, output = self.run_gate(stdout, 1, KNOWN_TOP)
        self.assertEqual(code, 1, output)
        self.assertIn(
            "MSan: test failed: test_wire_totals_staged_finish_counts_exactly signal 5",
            output,
        )

    def test_illegal_instruction_fail_line_fails_with_known_finding(self):
        stdout = (
            "check failed: "
            "test_wire_totals_staged_finish_counts_exactly signal 4\n"
        )
        code, output = self.run_gate(stdout, 1, KNOWN_TOP)
        self.assertEqual(code, 1, output)
        self.assertIn(
            "MSan: test failed: test_wire_totals_staged_finish_counts_exactly signal 4",
            output,
        )

    def test_binary_status_without_fail_line_fails(self):
        code, output = self.run_gate("", 133, KNOWN_TOP)
        self.assertEqual(code, 1, output)
        self.assertIn("MSan: test run failed with status 133", output)

    def test_known_findings_with_zero_exit_passes(self):
        stdout = "test_random_bytes\n*** ALL TESTS PASSED ***\n"
        code, output = self.run_gate(stdout, 0, KNOWN_TOP)
        self.assertEqual(code, 0, output)
        self.assertIn("(all known)", output)

    def test_ordinary_exit_code_with_known_finding_fails(self):
        stdout = "check failed: test_random_bytes exit 1\n"
        code, output = self.run_gate(stdout, 1, KNOWN_TOP)
        self.assertEqual(code, 1, output)
        self.assertIn("MSan: test failed: test_random_bytes exit 1", output)

    def test_unlisted_finding_fails_naming_it(self):
        code, output = self.run_gate("", 0, UNKNOWN_TOP)
        self.assertEqual(code, 1, output)
        self.assertIn(UNKNOWN_TOP, output)

    def test_fatal_error_with_zero_exit_fails_naming_the_case(self):
        # MSan's exitcode is 0 in the gate, so a case killed by a fatal
        # error exits 0 and the suite prints its pass line
        stdout = "test_server_stats_current_every_update\n*** ALL TESTS PASSED ***\n"
        code, output = self.run_gate(stdout, 0, "fatal")
        self.assertEqual(code, 1, output)
        self.assertIn(
            "MSan: fatal error: test_server_stats_current_every_update: "
            "ERROR: MemorySanitizer: requested allocation size",
            output,
        )

    def test_clean_run_passes(self):
        code, output = self.run_gate("", 0, None)
        self.assertEqual(code, 0, output)
        self.assertIn("MSan: clean", output)


if __name__ == "__main__":
    unittest.main()
