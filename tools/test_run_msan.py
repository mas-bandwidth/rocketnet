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


# a libc top frame, reached from two different in-tree call sites
LIBC_TOP = "bcmp (bin/test)"
CALLER_A = "check_refused(int) tools/fixture.cpp:5:3"
CALLER_B = "other_compare(int) tools/fixture.cpp:9:7"
KEY_A = "%s | %s" % (LIBC_TOP, CALLER_A)


def warning_text(top):
    if top is None:
        return ""
    if top == "fatal":
        return FATAL_TEXT
    if isinstance(top, tuple) and top[0] == "raw":
        return top[1]
    frames = top if isinstance(top, list) else [top, "main tools/fixture.cpp:1:1"]
    stack = "".join(
        "    #%d 0x%x in %s\n" % (i, 0x1000 * (i + 1), entry)
        for i, entry in enumerate(frames)
    )
    return (
        "==1==WARNING: MemorySanitizer: use-of-uninitialized-value\n"
        + stack
        + "  Uninitialized value was created by a heap allocation\n"
        "    #0 0x9000 in malloc\n"
        "SUMMARY: MemorySanitizer: use-of-uninitialized-value\n"
    )


class RunMsanGateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=REPO_ROOT)

    def tearDown(self):
        self.tmp.cleanup()

    def run_gate(self, stdout, status, top, known_entries=(KNOWN_TOP,), keep_dir=None):
        binary = Path(self.tmp.name) / "fake-test"
        binary.write_text(
            FAKE_TEMPLATE
            % {"warning": warning_text(top), "stdout": stdout, "status": status}
        )
        binary.chmod(0o755)
        known = Path(self.tmp.name) / "known.txt"
        known.write_text(
            "# the fixture's known list\n%s\n" % "\n".join(known_entries)
        )
        env = dict(os.environ)
        env["RUN_MSAN_TEST_BIN"] = str(binary)
        env["RUN_MSAN_KNOWN_FILE"] = str(known)
        env["RUN_MSAN_KEEP_DIR"] = str(keep_dir or Path(self.tmp.name) / "kept")
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

    def fatal_case(self, first_line, expected):
        # compiler-rt's other fatal forms end in Die() with the same exitcode
        # 0: the text is the runtime's format string with the tool name in
        text = (
            first_line + "\n"
            "    #0 0x1000 in malloc\n"
            "    #1 0x2000 in test_server_stats_current_every_update tools/fixture.cpp:3:1\n"
        )
        stdout = "test_server_stats_current_every_update\n*** ALL TESTS PASSED ***\n"
        code, output = self.run_gate(stdout, 0, ("raw", text))
        self.assertEqual(code, 1, output)
        self.assertIn(
            "MSan: fatal error: test_server_stats_current_every_update: " + expected,
            output,
        )

    def test_check_failed_with_zero_exit_fails_naming_the_case(self):
        self.fatal_case(
            '==1==MemorySanitizer: CHECK failed: msan_allocator.cpp:200 "((x)) != (0)"'
            " (0x0, 0x0) (tid=1)",
            "MemorySanitizer: CHECK failed: msan_allocator.cpp:200",
        )

    def test_allocation_failure_with_zero_exit_fails_naming_the_case(self):
        self.fatal_case(
            "==1==ERROR: MemorySanitizer failed to allocate 0x10000 (65536) bytes"
            " at address 0x700000000000 (errno: 12)",
            "ERROR: MemorySanitizer failed to allocate 0x10000",
        )

    def test_map_failure_with_zero_exit_fails_naming_the_case(self):
        self.fatal_case(
            "==1==ERROR: MemorySanitizer failed to deallocate 0x10000 (65536) bytes"
            " at address 0x700000000000 (error code: 22)",
            "ERROR: MemorySanitizer failed to deallocate 0x10000",
        )

    def test_clean_run_passes(self):
        code, output = self.run_gate("test_random_bytes\n*** ALL TESTS PASSED ***\n", 0, None)
        self.assertEqual(code, 0, output)
        self.assertIn("MSan: clean (1 test cases)", output)

    def test_zero_cases_run_fails(self):
        # zero inputs is an error: a suite that ran no case is never clean,
        # and known findings do not make an empty run pass either
        for top in (None, KNOWN_TOP):
            for stdout in ("", "*** ALL TESTS PASSED ***\n"):
                code, output = self.run_gate(stdout, 0, top)
                self.assertEqual(code, 1, output)
                self.assertIn("MSan: 0 test cases ran", output)

    # P3-12: a known entry names an in-tree frame, never a top frame alone

    def test_top_frame_only_entry_is_refused(self):
        # the plant: listing the libc top frame alone would pass every later
        # uninitialised compare; the gate refuses the list itself
        code, output = self.run_gate(
            "", 0, [LIBC_TOP, CALLER_A], known_entries=(LIBC_TOP,)
        )
        self.assertEqual(code, 1, output)
        self.assertIn("name no in-tree frame", output)
        self.assertIn(LIBC_TOP, output)

    def test_top_frame_only_entry_is_refused_on_a_clean_run(self):
        code, output = self.run_gate("", 0, None, known_entries=(LIBC_TOP,))
        self.assertEqual(code, 1, output)
        self.assertIn("name no in-tree frame", output)

    def test_finding_keyed_by_its_in_tree_caller_passes(self):
        stdout = "test_random_bytes\n*** ALL TESTS PASSED ***\n"
        code, output = self.run_gate(
            stdout, 0, [LIBC_TOP, CALLER_A, "main tools/fixture.cpp:1:1"],
            known_entries=(KEY_A,),
        )
        self.assertEqual(code, 0, output)
        self.assertIn("(all known)", output)

    def test_same_top_frame_from_another_caller_fails_naming_it(self):
        code, output = self.run_gate(
            "", 0, [LIBC_TOP, CALLER_B, "main tools/fixture.cpp:1:1"],
            known_entries=(KEY_A,),
        )
        self.assertEqual(code, 1, output)
        self.assertIn("findings not in", output)
        self.assertIn("%s | %s" % (LIBC_TOP, CALLER_B), output)

    def test_library_and_system_frames_are_not_in_tree(self):
        # entries whose only location is the C++ library or an absolute path
        for entry in (
            "memcmp include/c++/v1/__string/x.h:3:1",
            "memcmp /usr/include/string.h:3:1",
            "memcmp ../outside/x.cpp:3:1",
        ):
            code, output = self.run_gate("", 0, None, known_entries=(entry,))
            self.assertEqual(code, 1, entry + "\n" + output)
            self.assertIn("name no in-tree frame", output)


    def test_libcxx_header_frame_matches_its_documented_form(self):
        # the instrumented libc++'s header frames carry its install prefix;
        # the known list writes them include/c++/v1/<file>
        caller = "check_refused(int) tools/fixture.cpp:5:3"
        for prefix in ("/opt/msan-libcxx-21.1.8", "/home/runner/work/rocketnet/.msan-libcxx"):
            top = "std::__1::basic_string<char>::size() %s/include/c++/v1/string:1180:5" % prefix
            entry = "std::__1::basic_string<char>::size() include/c++/v1/string:1180:5 | " + caller
            stdout = "test_random_bytes\n*** ALL TESTS PASSED ***\n"
            code, output = self.run_gate(stdout, 0, [top, caller], known_entries=(entry,))
            self.assertEqual(code, 0, prefix + "\n" + output)
            self.assertIn("(all known)", output)

    # a finding names the cases that reported it, and a failing run keeps
    # the per-process logs, whose whole stacks the verdict prints in part

    def test_finding_names_its_case(self):
        code, output = self.run_gate(
            "", 0, [UNKNOWN_TOP, "test_random_bytes tools/fixture.cpp:3:1"]
        )
        self.assertEqual(code, 1, output)
        self.assertIn("  case: test_random_bytes", output)

    def test_failing_run_keeps_its_logs(self):
        keep = Path(self.tmp.name) / "kept-logs"
        code, output = self.run_gate("", 0, UNKNOWN_TOP, keep_dir=keep)
        self.assertEqual(code, 1, output)
        self.assertIn("MSan: the run's logs are kept in %s" % keep, output)
        kept = sorted(path.name for path in keep.iterdir())
        self.assertIn("msan.log", kept)
        reports = [name for name in kept if name.startswith("msan.") and name != "msan.log"]
        self.assertEqual(len(reports), 1, kept)
        self.assertIn(UNKNOWN_TOP, (keep / reports[0]).read_text())

    def test_passing_run_keeps_nothing(self):
        keep = Path(self.tmp.name) / "kept-logs"
        code, output = self.run_gate(
            "test_random_bytes\n*** ALL TESTS PASSED ***\n", 0, KNOWN_TOP, keep_dir=keep
        )
        self.assertEqual(code, 0, output)
        self.assertNotIn("kept in", output)
        self.assertFalse(keep.exists())

    def test_runner_reasons_name_the_case(self):
        # the suite runner's spawn failure and its per-case memory bound are
        # fail lines too: each fails the gate by case, beside a known finding
        for reason in ("failed to spawn", "peak_rss_mb=5000 bound_mb=4000"):
            stdout = "check failed: test_random_bytes %s\n" % reason
            code, output = self.run_gate(stdout, 1, KNOWN_TOP)
            self.assertEqual(code, 1, reason + "\n" + output)
            self.assertIn("MSan: test failed: test_random_bytes %s" % reason, output)


if __name__ == "__main__":
    unittest.main()
