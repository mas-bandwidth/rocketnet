#!/usr/bin/env python3
"""Process-level tests for tools/check_dll_consumer.py's decisions, with fake
compilers (--cc/--cxx), a fake nm and lipo first on PATH and fake library
files in a fixture build directory, so nothing is compiled, linked or run
against the real library: a production library exporting a name outside
tools/exports.txt exits nonzero, a consumer program that fails exits nonzero,
exactly the listed exports and passing consumers exit 0, and an empty build
directory (zero libraries) exits nonzero."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_dll_consumer

TOOL = Path(__file__).resolve().with_name("check_dll_consumer.py")
EXPORTS = Path(__file__).resolve().with_name("exports.txt")

# writes an "executable" that exits FAKE_RUN_EXIT to the -o target
FAKE_COMPILER = """#!/bin/sh
out=""
while [ $# -gt 0 ]; do
    if [ "$1" = "-o" ]; then out=$2; shift; fi
    shift
done
[ -n "$out" ] || exit 1
printf '#!/bin/sh\\nexit %s\\n' "${FAKE_RUN_EXIT:-0}" > "$out" && chmod +x "$out"
"""

FAKE_NM = "#!/bin/sh\ncat \"$FAKE_NM_OUT\"\n"
FAKE_LIPO = "#!/bin/sh\nexit 1\n"


def listed_exports():
    return sorted(line.strip() for line in EXPORTS.read_text().splitlines()
                  if line.strip() and not line.startswith("#"))


@unittest.skipIf(sys.platform == "win32", "the fakes are POSIX shell scripts")
class CheckDllConsumerProcessTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.bin = self.dir / "fakes"
        self.bin.mkdir()
        for name, text in (("cc", FAKE_COMPILER), ("nm", FAKE_NM), ("lipo", FAKE_LIPO)):
            (self.bin / name).write_text(text)
            (self.bin / name).chmod(0o755)
        self.build = self.dir / "build"
        self.build.mkdir()
        for name in set(check_dll_consumer.library_names()):
            (self.build / name).write_bytes(b"fixture library\n")
        self.nm_out = self.dir / "nm.txt"

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, build, symbols, run_exit=0):
        self.nm_out.write_text("".join(f"0000000000001000 T {name}\n" for name in symbols))
        env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ.get("PATH", ""),
                   FAKE_NM_OUT=str(self.nm_out), FAKE_RUN_EXIT=str(run_exit))
        fake = str(self.bin / "cc")
        return subprocess.run([sys.executable, str(TOOL), "--build", str(build), "--cc", fake, "--cxx", fake],
                              capture_output=True, text=True, env=env)

    def test_listed_exports_and_passing_consumers_pass(self):
        result = self.run_tool(self.build, listed_exports())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.count("PASS  "), 5, result.stdout)
        self.assertIn("exports exactly the", result.stdout)

    def test_unlisted_export_fails(self):
        result = self.run_tool(self.build, listed_exports() + ["rocketnet_planted_export"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exports names outside tools/exports.txt: rocketnet_planted_export", result.stdout)

    def test_failing_consumer_fails(self):
        result = self.run_tool(self.build, listed_exports(), run_exit=1)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exchanges one message each way", result.stdout)

    def test_empty_build_directory_fails(self):
        empty = self.dir / "empty"
        empty.mkdir()
        result = self.run_tool(empty, listed_exports())
        self.assertNotEqual(result.returncode, 0)
        if not (check_dll_consumer.ROOT / "bin").exists():
            self.assertIn("was not found under", result.stdout)


if __name__ == "__main__":
    unittest.main()
