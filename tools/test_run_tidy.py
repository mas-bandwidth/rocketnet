#!/usr/bin/env python3
"""Tests for the clang-tidy and cppcheck gate's verdicts in tools/run_tidy.py.

No real analyzer: a fake cppcheck prints canned diagnostics, and the compile
commands name a compiler that does not exist, so no platform macros are read.
"""

import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_tidy

URING = "sdk/rocketnet_server_uring_linux.cpp"

FAKE_CPPCHECK = '''#!/usr/bin/env python3
import os
import sys
sys.stderr.write(os.environ.get("FAKE_CPPCHECK_STDERR", ""))
'''


def entry(defines):
    return {"directory": run_tidy.ROOT,
            "command": "/nonexistent/c++ %s -c %s" % (" ".join(defines), URING),
            "file": URING}


class RunTidyGateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cppcheck = Path(self.tmp.name) / "cppcheck"
        self.cppcheck.write_text(FAKE_CPPCHECK)
        self.cppcheck.chmod(0o755)
        self.saved_env = dict(os.environ)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self.saved_env)
        self.tmp.cleanup()

    def cppcheck_file(self, stderr, defines):
        os.environ["FAKE_CPPCHECK_STDERR"] = stderr
        return run_tidy.run_cppcheck_file(str(self.cppcheck), entry(defines),
                                          os.path.join(run_tidy.ROOT, URING))

    def test_an_error_directive_is_a_file_not_analyzed(self):
        # cppcheck stops a configuration at #error: nothing after it is read,
        # so the file must fail the gate rather than read as clean
        raw, errors = self.cppcheck_file(
            "%s:58:2: error: #error \"needs liburing 2.3\" [preprocessorErrorDirective]\n" % URING,
            ["-DROCKETNET_IO_URING=1"])
        self.assertEqual(raw, [])
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("preprocessorErrorDirective", errors[0])

    def test_a_configured_io_uring_build_gives_cppcheck_the_checked_macro(self):
        # configure defines ROCKETNET_IO_URING only against a liburing with
        # IORING_RECV_MULTISHOT; cppcheck does not read liburing.h
        with_uring = run_tidy.cppcheck_arguments(entry(["-DROCKETNET_IO_URING=1"]))
        self.assertTrue(any(arg.startswith("-DIORING_RECV_MULTISHOT=") for arg in with_uring), with_uring)
        without = run_tidy.cppcheck_arguments(entry(["-DROCKETNET_XDP=1"]))
        self.assertFalse(any(arg.startswith("-DIORING_RECV_MULTISHOT") for arg in without), without)

    def test_a_finding_at_another_site_is_a_new_finding(self):
        # fixing a reviewed cast and adding one elsewhere in the same file
        # keeps the count of identical messages; it must not keep the keys
        cast = "Potentially invalid type conversion in old-style C cast, clarify/fix with C++ cast"

        def findings(lines):
            stderr = "".join("%s:%d:9: warning: %s [dangerousTypeCast]\n" % (URING, line, cast) for line in lines)
            raw, errors = self.cppcheck_file(stderr, ["-DROCKETNET_IO_URING=1"])
            self.assertEqual(errors, [])
            return set(run_tidy.finding_lines("cppcheck", raw))

        reviewed = findings([420, 439])
        self.assertEqual(len(reviewed), 2)
        self.assertEqual(findings([439, 420]), reviewed)
        self.assertNotEqual(findings([420, 550]), reviewed)

    def test_zero_translation_units_fails(self):
        # zero inputs is an error: a tree with no sdk/*.cpp is never clean
        import contextlib
        import io
        saved = (run_tidy.ROOT, run_tidy.BUILD, run_tidy.COMPILE_DB, run_tidy.TIDY_DB_DIR, sys.argv)
        out = io.StringIO()
        try:
            sys.argv = ["run_tidy.py", "--root", self.tmp.name]
            with contextlib.redirect_stdout(out):
                code = run_tidy.main()
        finally:
            run_tidy.ROOT, run_tidy.BUILD, run_tidy.COMPILE_DB, run_tidy.TIDY_DB_DIR, sys.argv = saved
        self.assertEqual(code, 1, out.getvalue())
        self.assertIn("0 sdk/*.cpp", out.getvalue())


if __name__ == "__main__":
    unittest.main()
