#!/usr/bin/env python3
"""Tests for tools/check_xdp_archive.py --committed-only: a fixture tree
holding this repository's cmake/XdpSources.cmake and every input it lists
passes; the same tree missing one listed input is refused by name; a source
list that names nothing and an empty tree are refused. Each case runs the
tool as a process on a fixture in a temporary directory. The source list is
read by `cmake -P`, as the tool does, so cmake must be on the path; no
compiler is used."""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
TOOL = TOOLS / "check_xdp_archive.py"
REPO = TOOLS.parent

EMPTY_LIST = """set(ROCKETNET_XDP_SOURCES)
if(DEFINED ROCKETNET_XDP_SOURCE_LIST_OUTPUT)
    file(WRITE "${ROCKETNET_XDP_SOURCE_LIST_OUTPUT}" "")
    foreach(source IN LISTS ROCKETNET_XDP_SOURCES)
        file(APPEND "${ROCKETNET_XDP_SOURCE_LIST_OUTPUT}" "${source}\\n")
    endforeach()
endif()
"""


class XdpArchiveCommittedTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if shutil.which("cmake") is None:
            raise AssertionError("cmake is not on the path: check_xdp_archive.py reads its source list with cmake -P")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def plant(self):
        (self.root / "cmake").mkdir()
        shutil.copyfile(REPO / "cmake/XdpSources.cmake", self.root / "cmake/XdpSources.cmake")
        listing = self.root / "list.txt"
        subprocess.run(["cmake", f"-DROCKETNET_XDP_SOURCE_LIST_OUTPUT={listing}", "-P",
                        str(self.root / "cmake/XdpSources.cmake")], check=True, timeout=30)
        names = listing.read_text().split()
        listing.unlink()
        self.assertTrue(names)
        for name in names:
            (self.root / name).parent.mkdir(parents=True, exist_ok=True)
            (self.root / name).write_text("/* fixture */\n")
        return names

    def run_tool(self):
        return subprocess.run([sys.executable, str(TOOL), str(self.root), "--committed-only"],
                              capture_output=True, text=True, timeout=30)

    def test_every_listed_input_present_passes(self):
        self.plant()
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("test_xdp_sources", result.stdout)

    def test_a_missing_listed_input_is_refused(self):
        names = self.plant()
        (self.root / names[-1]).unlink()
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("XDP source list names a missing input", result.stderr)
        self.assertNotIn("test_xdp_sources", result.stdout)

    def test_an_empty_source_list_is_refused(self):
        self.plant()
        (self.root / "cmake/XdpSources.cmake").write_text(EMPTY_LIST)
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0 files: the XDP source list is empty", result.stderr)

    def test_an_empty_tree_is_refused(self):
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0 files", result.stderr)
        self.assertNotIn("test_xdp_sources", result.stdout)


if __name__ == "__main__":
    unittest.main()
