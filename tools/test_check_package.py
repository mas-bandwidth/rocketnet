#!/usr/bin/env python3
"""Tests for tools/check_package.py: an installed package whose CMake metadata
embeds the producer's build path is refused, a relocatable package passes, a
consumer build that registers 0 tests is refused, and a directory with no
CMakeCache.txt (0 files) is refused. Each case runs the tool as a process and
asserts its exit code.

The tool drives cmake and ctest. Fake `cmake` and `ctest` on PATH stand in
for them, so nothing is built:
  cmake --install B --prefix P   writes a RocketNet package into P (version
                                 1.0.0, both targets); FAKE_LEAK=1 makes its
                                 RocketNetConfig.cmake embed B
  cmake -S S -B B ...            creates B, records FAKE_TESTS (default 2) as
                                 the number of tests registered, and prints the
                                 settings-probe lines the real probe prints
  cmake --build ...              succeeds
  ctest --test-dir B ...         like ctest: with 0 tests it exits 8 under
                                 --no-tests=error and 0 otherwise
"""

import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parent / "check_package.py"

FAKE_CMAKE = """#!{python}
import os, sys
from pathlib import Path
args = sys.argv[1:]
if args and args[0] == "--install":
    build, prefix = args[1], Path(args[args.index("--prefix") + 1])
    package = prefix / "lib" / "cmake" / "RocketNet"
    package.mkdir(parents=True)
    leak = ("set(_producer " + chr(34) + build + chr(34) + ")" + chr(10)) if os.environ.get("FAKE_LEAK") == "1" else ""
    (package / "RocketNetConfig.cmake").write_text(leak + "include(RocketNetTargets.cmake)" + chr(10))
    (package / "RocketNetConfigVersion.cmake").write_text("set(PACKAGE_VERSION " + chr(34) + "1.0.0" + chr(34) + ")" + chr(10))
    (package / "RocketNetTargets.cmake").write_text(
        "add_library(RocketNet::rocketnet STATIC IMPORTED)" + chr(10) +
        "add_library(RocketNet::rocketnet_shared SHARED IMPORTED)" + chr(10))
    # the rest of what a real install writes: tools/check_package.py holds
    # the installed tree to its INSTALLED_FILES
    for rel in ("include/rocketnet.h", "share/rocketnet/LICENSE", "share/rocketnet/sodium/LICENSE",
                "lib/cmake/RocketNet/RocketNetTargets-release.cmake", "lib/librocketnet.a", "lib/librocketnet_shared.so"):
        (prefix / rel).parent.mkdir(parents=True, exist_ok=True)
        (prefix / rel).write_text("")
elif args and args[0] == "--build":
    pass
elif "-S" in args:
    build = Path(args[args.index("-B") + 1])
    build.mkdir(parents=True, exist_ok=True)
    (build / "fake-tests").write_text(os.environ.get("FAKE_TESTS", "2"))
    print("probe rocketnet options=[] definitions=[ROCKETNET_DEV=1;ROCKETNET_MAX_CLIENTS=1000]")
    print("probe rocketnet_shared options=[] definitions=[ROCKETNET_SHARED;ROCKETNET_DEV=1;ROCKETNET_MAX_CLIENTS=1000]")
else:
    sys.exit("fake cmake: unexpected arguments " + repr(args))
"""

FAKE_CTEST = """#!{python}
import sys
from pathlib import Path
args = sys.argv[1:]
tests = int((Path(args[args.index("--test-dir") + 1]) / "fake-tests").read_text())
if tests == 0:
    print("No tests were found!!!")
    sys.exit(8 if "--no-tests=error" in args else 0)
print("100% tests passed, 0 tests failed out of " + str(tests))
"""

CACHE = """# fixture cache
rocketnet_SOURCE_DIR:STATIC={source}
CMAKE_GENERATOR:INTERNAL=Unix Makefiles
ROCKETNET_BUILD_SHARED:BOOL=ON
ROCKETNET_BUILD_DEV_LIBRARIES:BOOL=ON
ROCKETNET_DEV:BOOL=ON
ROCKETNET_MAX_CLIENTS:STRING=1000
"""


class CheckPackageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name).resolve()
        self.bin = base / "bin"
        self.bin.mkdir()
        for name, text in (("cmake", FAKE_CMAKE), ("ctest", FAKE_CTEST)):
            path = self.bin / name
            path.write_text(text.format(python=sys.executable))
            path.chmod(path.stat().st_mode | stat.S_IXUSR)
        (base / "source").mkdir()
        self.build = base / "build"
        self.build.mkdir()
        (self.build / "CMakeCache.txt").write_text(CACHE.format(source=base / "source"))

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, build, **fake):
        env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ.get("PATH", ""))
        env.pop("FAKE_LEAK", None)
        env.pop("FAKE_TESTS", None)
        env.update(fake)
        return subprocess.run([sys.executable, str(TOOL), str(build), "--config", "Release"],
                              capture_output=True, text=True, env=env)

    def test_a_relocatable_package_passes(self):
        result = self.run_tool(self.build)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Relocated SDK: C/C++ static/shared consumers passed", result.stdout)

    def test_a_package_embedding_the_build_path_is_refused(self):
        result = self.run_tool(self.build, FAKE_LEAK="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("RocketNetConfig.cmake embeds producer path", result.stderr)

    def test_zero_consumer_tests_is_refused(self):
        result = self.run_tool(self.build, FAKE_TESTS="0")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("No tests were found", result.stdout)

    def test_an_empty_build_directory_is_refused(self):
        with tempfile.TemporaryDirectory() as empty:
            result = self.run_tool(Path(empty))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("0 files", result.stderr)


if __name__ == "__main__":
    unittest.main()
