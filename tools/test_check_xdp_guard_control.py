#!/usr/bin/env python3
"""Tests for tools/check_xdp_guard_control.py's decision, with no build.

The tool archives HEAD, removes the two inactive-index guards from
sdk/rocketnet_server_socket_linux.cpp, builds test_xdp under the sanitizers
and requires it to die with "index -1 out of bounds". Here git, cmake and the
compiler are fakes on PATH, written into a temporary directory: the fake git
archives a fixture source tree, the fake cmake "builds" a test_xdp that dies
with the sanitizer line when the guards are gone, as the real one does. The
control passes; a test_xdp that misses the removed guards (the violation the
control exists to catch) is refused; a source with one guard, and an archive
holding no source at all, are refused. Each case runs the tool as a process
with --root on the temporary directory."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parent / "check_xdp_guard_control.py"
GUARD = "    if ( flow_index < 0 )\n    {\n        return;\n    }\n"

FAKE_GIT = """import os, sys, tarfile
assert sys.argv[1:] == ["archive", "HEAD"], sys.argv
source = os.environ["FAKE_SOURCE"]
with tarfile.open(fileobj=sys.stdout.buffer, mode="w|") as archive:
    for name in sorted(os.listdir(source)):
        archive.add(os.path.join(source, name), arcname=name)
"""

FAKE_CMAKE = """import os, sys
from pathlib import Path
args = sys.argv[1:]
if args[0] == "-S":
    output = next(a.split("=", 1)[1] for a in args if a.startswith("-DCMAKE_RUNTIME_OUTPUT_DIRECTORY="))
    tree = Path(args[3])
    tree.mkdir(parents=True, exist_ok=True)
    (tree / "CMakeCache.txt").write_text(
        "CMAKE_CXX_COMPILER:FILEPATH=" + os.path.join(os.path.dirname(__file__), "fakecxx") + "\\n"
        + "FAKE_SOURCE:PATH=" + args[1] + "\\nFAKE_OUTPUT:PATH=" + output + "\\n")
elif args[0] == "--build":
    cache = dict(line.split("=", 1) for line in (Path(args[1]) / "CMakeCache.txt").read_text().splitlines())
    text = (Path(cache["FAKE_SOURCE:PATH"]) / "sdk/rocketnet_server_socket_linux.cpp").read_text()
    guarded = "flow_index < 0" in text or os.environ.get("FAKE_TEST_MISSES_GUARDS") == "1"
    output = Path(cache["FAKE_OUTPUT:PATH"])
    output.mkdir(parents=True, exist_ok=True)
    body = "print('PASS')\\n" if guarded else (
        "import sys\\nsys.stderr.write('rocketnet_server_socket_linux.cpp:40:9: runtime error: "
        "index -1 out of bounds for type rn_flow_t[4]\\\\n')\\nsys.exit(1)\\n")
    (output / "test_xdp").write_text("#!" + sys.executable + "\\n" + body)
    (output / "test_xdp").chmod(0o755)
else:
    sys.exit("fake cmake: unexpected " + repr(args))
"""

FAKE_CXX = """print("fake c++ 1.0")
"""


class XdpGuardControlTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "root"
        self.root.mkdir()
        self.source = Path(self.tmp.name) / "source"
        (self.source / "sdk").mkdir(parents=True)
        fakes = Path(self.tmp.name) / "fakebin"
        fakes.mkdir()
        for name, body in (("git", FAKE_GIT), ("cmake", FAKE_CMAKE), ("fakecxx", FAKE_CXX)):
            (fakes / name).write_text(f"#!{sys.executable}\n{body}")
            (fakes / name).chmod(0o755)
        self.env = dict(os.environ, PATH=f"{fakes}{os.pathsep}{os.environ.get('PATH', '')}",
                        FAKE_SOURCE=str(self.source))
        self.env.pop("FAKE_TEST_MISSES_GUARDS", None)

    def tearDown(self):
        self.tmp.cleanup()

    def plant(self, guards):
        body = "void finish(int flow_index)\n{\n" + GUARD + "}\n"
        (self.source / "sdk/rocketnet_server_socket_linux.cpp").write_text(body * guards)

    def run_tool(self, **extra):
        return subprocess.run([sys.executable, str(TOOL), "--root", str(self.root)],
                              capture_output=True, text=True, timeout=30, env={**self.env, **extra})

    def test_a_sanitizer_death_without_the_guards_passes(self):
        self.plant(2)
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("test_xdp_inactive_generation_control", result.stdout)

    def test_a_test_that_misses_the_removed_guards_is_refused(self):
        self.plant(2)
        result = self.run_tool(FAKE_TEST_MISSES_GUARDS="1")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("inactive-index control did not produce the expected sanitizer failure", result.stderr)
        self.assertNotIn("test_xdp_inactive_generation_control", result.stdout)

    def test_one_guard_is_refused(self):
        self.plant(1)
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("expected the two native finish/abort guards, found 1", result.stderr)

    def test_an_empty_archive_is_refused(self):
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0 files", result.stderr)


if __name__ == "__main__":
    unittest.main()
