#!/usr/bin/env python3
"""Tests for tools/check_other_libraries.py: each control must go red."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_other_libraries as check

# One spelling of every name the check refuses, built from parts like the check's own.
SPELLINGS = (
    "net" + "code.io",
    "NET" + "CODE_X64",
    "Yo" + "jimbo",
    "Rak" + "Net",
    "E" + "Net",
    "reli" + "able.io",
    "mas-bandwidth/reli" + "able",
    "reli" + "able_endpoint_t",
    "the reli" + "able library",
    "reli" + "able's ack",
    "seri" + "alize.h",
    "seri" + "alize's reader",
    "Bit" + "Reader",
    "Read" + "Stream",
    "the seri" + "alize library",
    "Network " + "Next",
    "network" + "next",
    "next" + ".h",
    "next_" + "platform.h",
    "Game" + "NetworkingSockets",
    "St" + "eam",
    "QU" + "IC",
)


class TextTest(unittest.TestCase):

    def test_every_name_fails(self):
        for spelling in SPELLINGS:
            with self.subTest(spelling=spelling):
                self.assertEqual(len(check.violations_in_text("a.c", f"// as {spelling} does\n")), 1)

    def test_rocketnet_words_pass(self):
        for line in ("// reliable ordered delivery on channel 3",
                     "// serialize the compare and tag",
                     "// the next packet goes out on the next update",
                     "// generic network code is the caller's",
                     "// ROCKETNET_RELIABLE_ORDERED",
                     "// steady state, upstream, streams of bytes",
                     "// RFC 9001 section 6.6 bounds the forgery rate"):
            with self.subTest(line=line):
                self.assertEqual(check.violations_in_text("a.c", line + "\n"), [])

    def test_line_number_and_name_are_reported(self):
        word = "yo" + "jimbo"
        found = check.violations_in_text("a.c", f"one\ntwo {word}\n")
        self.assertEqual(found, [("a.c", 2, word, f"two {word}")])

    def test_path_with_a_name_fails(self):
        self.assertEqual(len(check.violations_in_path("tests/fuzz/rocketnet_fuzz_" + "net" + "code.cpp")), 1)
        self.assertEqual(check.violations_in_path("tests/fuzz/rocketnet_fuzz_session.cpp"), [])


class TreeTest(unittest.TestCase):

    def test_tree_control_goes_red_and_dated_record_stays_green(self):
        word = "rak" + "net"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs" / "audits").mkdir(parents=True)
            (root / "docs" / "audits" / "2026-10-24-a.md").write_text(f"compared with {word}\n")
            (root / "clean.c").write_text("// the client\n")
            files = ["docs/audits/2026-10-24-a.md", "clean.c"]
            self.assertEqual(check.check(root, files), [])
            (root / "dirty.c").write_text(f"// like {word}\n")
            self.assertEqual(check.check(root, files + ["dirty.c"]),
                             [("dirty.c", 1, word, f"// like {word}")])

    def test_a_record_name_outside_its_folder_is_checked(self):
        self.assertIsNone(check.allowed("tools/audits/x.md"))
        self.assertIsNone(check.allowed("docs/QUALIFICATION-x/y.md"))
        self.assertIsNotNone(check.allowed("docs/QUALIFICATION-v1.0.0-preview-3.md"))

    def test_binary_file_is_skipped(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "blob.bin").write_bytes(b"\0" + ("yo" + "jimbo").encode())
            self.assertEqual(check.check(root, ["blob.bin"]), [])

    def test_zero_tracked_files_is_an_error(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(check.main(["--root", directory]), 1)

    def test_repository_is_clean(self):
        self.assertEqual(check.check(check.ROOT, check.tracked_files(check.ROOT)), [])


if __name__ == "__main__":
    unittest.main()
