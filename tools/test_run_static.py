#!/usr/bin/env python3
"""Tests for the gcc -fanalyzer gate's verdicts in tools/run_static.py.

No real compiler: a fake g++ first on PATH prints canned diagnostics and exits
with a chosen status, and the compile database is a temporary file. Each test
runs the gate's main() against an empty temporary baseline.
"""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_static

TU = "sdk/rocketnet_base64.cpp"

FAKE_GXX = '''#!/usr/bin/env python3
import os
import sys
sys.stderr.write(os.environ.get("FAKE_GXX_STDERR", ""))
sys.exit(int(os.environ.get("FAKE_GXX_STATUS", "0")))
'''

LEAK = "leak of 'p' [CWE-401]"


def diagnostic(level, option):
    return "%s/%s:12:5: %s: %s [%s]\n" % (run_static.ROOT, TU, level, LEAK, option)


class RunStaticGateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        tmp = Path(self.tmp.name)
        bindir = tmp / "bin"
        bindir.mkdir()
        gxx = bindir / "g++"
        gxx.write_text(FAKE_GXX)
        gxx.chmod(0o755)
        self.compile_db = tmp / "compile_commands.json"
        self.baseline = tmp / "baseline.txt"
        self.baseline.write_text("# empty\n")
        self.report = tmp / "report.txt"
        self.saved = {
            "BUILD": run_static.BUILD,
            "COMPILE_DB": run_static.COMPILE_DB,
            "parser_files": run_static.parser_files,
        }
        run_static.BUILD = str(tmp)
        run_static.COMPILE_DB = str(self.compile_db)
        run_static.parser_files = lambda: [TU]
        self.saved_env = dict(os.environ)
        os.environ["PATH"] = str(bindir) + os.pathsep + os.environ.get("PATH", "")
        self.write_db([TU])

    def tearDown(self):
        for name, value in self.saved.items():
            setattr(run_static, name, value)
        os.environ.clear()
        os.environ.update(self.saved_env)
        self.tmp.cleanup()

    def write_db(self, files):
        entries = [{"directory": self.tmp.name,
                    "command": "/usr/bin/c++ -O2 -Werror -c %s/%s" % (run_static.ROOT, rel),
                    "file": "%s/%s" % (run_static.ROOT, rel)} for rel in files]
        self.compile_db.write_text(json.dumps(entries))

    def gate(self, stderr, status, *extra):
        os.environ["FAKE_GXX_STDERR"] = stderr
        os.environ["FAKE_GXX_STATUS"] = str(status)
        argv = ["run_static.py", "--baseline", str(self.baseline)] + list(extra)
        out = io.StringIO()
        saved_argv = sys.argv
        sys.argv = argv
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
                code = run_static.main()
        finally:
            sys.argv = saved_argv
        return code, out.getvalue()

    def test_clean_run_passes(self):
        code, out = self.gate("", 0)
        self.assertEqual(code, 0, out)
        self.assertIn("static gate passed", out)

    def test_warning_finding_fails(self):
        code, out = self.gate(diagnostic("warning", "-Wanalyzer-malloc-leak"), 0)
        self.assertEqual(code, 1, out)
        self.assertIn("%s:12:5: %s [-Wanalyzer-malloc-leak]" % (TU, LEAK), out)

    def test_werror_finding_fails_as_the_same_finding(self):
        code, out = self.gate(diagnostic("warning", "-Wanalyzer-malloc-leak"), 0,
                              "--report", str(self.report))
        self.assertEqual(code, 1, out)
        warning_report = self.report.read_text()
        self.report.unlink()
        code, out = self.gate(diagnostic("error", "-Werror=analyzer-malloc-leak"), 1,
                              "--report", str(self.report))
        self.assertEqual(code, 1, out)
        self.assertIn("-Wanalyzer-malloc-leak", out)
        self.assertEqual(self.report.read_text(), warning_report)

    def test_unexplained_compiler_failure_fails(self):
        for stderr in ("", "g++: fatal error: no input files\n",
                       "%s/%s:1:10: fatal error: missing.h: No such file or directory\n"
                       % (run_static.ROOT, TU)):
            code, out = self.gate(stderr, 1, "--report", str(self.report))
            self.assertEqual(code, 1, out)
            self.assertIn("incomplete", out)
            self.assertFalse(self.report.exists(), out)

    def test_compiler_failure_beside_a_finding_fails(self):
        stderr = (diagnostic("error", "-Werror=analyzer-malloc-leak")
                  + "%s/%s:3:1: error: expected ';' before '}' token\n" % (run_static.ROOT, TU))
        code, out = self.gate(stderr, 1)
        self.assertEqual(code, 1, out)
        self.assertIn("incomplete", out)

    def test_missing_translation_unit_fails(self):
        self.write_db([])
        code, out = self.gate("", 0, "--report", str(self.report))
        self.assertEqual(code, 1, out)
        self.assertIn(TU, out)
        self.assertFalse(self.report.exists(), out)

    def test_incomplete_run_writes_no_baseline(self):
        self.write_db([])
        self.baseline.write_text("kept\n")
        code, out = self.gate("", 0, "--generate")
        self.assertEqual(code, 1, out)
        self.assertEqual(self.baseline.read_text(), "kept\n")


if __name__ == "__main__":
    unittest.main()
