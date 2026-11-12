#!/usr/bin/env python3
"""Process-level tests for tools/check_driver_extension.py's decisions, with
fake cmake and ctest first on PATH so nothing is configured or built: a
failing extension test exits nonzero, a passing one exits 0, an extension
that registers zero tests exits nonzero (the fake ctest, like the real one,
exits 0 on zero tests unless --no-tests=error is passed), and a build
directory with no CMakeCache.txt exits nonzero naming the count."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().with_name("check_driver_extension.py")

FAKE_CMAKE = "#!/bin/sh\necho \"cmake $*\" >> \"$FAKE_LOG\"\nexit 0\n"

FAKE_CTEST = """#!/bin/sh
echo "ctest $*" >> "$FAKE_LOG"
case "$FAKE_CTEST" in
    fail) echo "0% tests passed, 1 tests failed out of 1"; exit 8 ;;
    none)
        echo "No tests were found!!!"
        for arg in "$@"; do
            if [ "$arg" = "--no-tests=error" ]; then exit 8; fi
        done
        exit 0 ;;
esac
echo "100% tests passed, 0 tests failed out of 1"
exit 0
"""


class CheckDriverExtensionProcessTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        bin_dir = self.dir / "bin"
        bin_dir.mkdir()
        for name, text in (("cmake", FAKE_CMAKE), ("ctest", FAKE_CTEST)):
            (bin_dir / name).write_text(text)
            (bin_dir / name).chmod(0o755)
        self.build = self.dir / "build"
        self.build.mkdir()
        (self.build / "CMakeCache.txt").write_text(
            "// a fixture cache\n"
            f"rocketnet_SOURCE_DIR:STATIC={self.dir / 'source'}\n"
            "CMAKE_GENERATOR:INTERNAL=Unix Makefiles\n")
        self.log = self.dir / "calls.log"
        self.env = dict(os.environ, PATH=str(bin_dir) + os.pathsep + os.environ.get("PATH", ""),
                        FAKE_LOG=str(self.log))

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, build, ctest_mode):
        env = dict(self.env, FAKE_CTEST=ctest_mode)
        return subprocess.run([sys.executable, str(TOOL), str(build), "--config", "Release"],
                              capture_output=True, text=True, env=env)

    def test_passing_extension_test_passes(self):
        result = self.run_tool(self.build, "pass")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Native extension: new client/server transport registered", result.stdout)
        calls = self.log.read_text()
        self.assertIn("cmake --build", calls)
        self.assertIn("ctest --test-dir", calls)

    def test_failing_extension_test_fails(self):
        result = self.run_tool(self.build, "fail")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("Native extension: new client/server transport registered", result.stdout)

    def test_zero_extension_tests_fail(self):
        result = self.run_tool(self.build, "none")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("No tests were found", result.stdout)
        self.assertNotIn("Native extension: new client/server transport registered", result.stdout)

    def test_empty_build_directory_fails(self):
        empty = self.dir / "empty"
        empty.mkdir()
        result = self.run_tool(empty, "pass")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("0 cache entries", result.stderr)


if __name__ == "__main__":
    unittest.main()
