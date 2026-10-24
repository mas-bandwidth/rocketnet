#!/usr/bin/env python3
"""Tests for tools/check_client_server_words.py: each control must go red."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_client_server_words as check

WORD = check.WORD


class TextTest(unittest.TestCase):

    def test_clean_line_passes(self):
        self.assertEqual(check.violations_in_text("a.c", "// the client sends to the server\n"), [])

    def test_word_in_a_comment_fails(self):
        self.assertEqual(len(check.violations_in_text("a.c", f"// the {WORD} acks it\n")), 1)

    def test_every_case_and_compound_fails(self):
        for line in (WORD.upper(), WORD.capitalize(), f"{WORD}s", f"{WORD}'s", f"max_{WORD}_up_kbps",
                     f"Crashed{WORD.capitalize()}Silent", f"{WORD}-to-{WORD}", f"rocketnet_{WORD}_accepted"):
            with self.subTest(line=line):
                self.assertEqual(len(check.violations_in_text("a.c", line + "\n")), 1)

    def test_line_number_is_reported(self):
        found = check.violations_in_text("a.c", f"one\ntwo {WORD}\n")
        self.assertEqual(found, [("a.c", 2, f"two {WORD}")])

    def test_veth_syntax_is_allowed(self):
        line = f'ip link add a type veth {WORD} name b mtu 1500'
        self.assertEqual(check.violations_in_text("a.sh", line), [])

    def test_veth_syntax_does_not_excuse_the_rest_of_the_line(self):
        line = f'ip link add a type veth {WORD} name b  # the {WORD} end'
        self.assertEqual(len(check.violations_in_text("a.sh", line)), 1)

    def test_type_error_is_allowed(self):
        self.assertEqual(check.violations_in_text("a.py", "except (TypeError, ValueError):"), [])
        self.assertEqual(check.violations_in_text("a.py", "raise argparse.ArgumentTypeError('x')"), [])

    def test_path_with_the_word_fails(self):
        self.assertEqual(len(check.violations_in_path(f"functional/posix_hard_killed_{WORD}.cpp")), 1)
        self.assertEqual(check.violations_in_path("functional/posix_hard_killed_client.cpp"), [])


class TreeTest(unittest.TestCase):

    def test_tree_control_goes_red_and_allowed_file_stays_green(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "LICENSE").write_text(f"{WORD}-to-{WORD} transmission\n")
            (root / "clean.c").write_text("// the client\n")
            self.assertEqual(check.check(root, ["LICENSE", "clean.c"]), [])
            (root / "dirty.c").write_text(f"int {WORD}_index = 0;\n")
            self.assertEqual(check.check(root, ["LICENSE", "clean.c", "dirty.c"]),
                             [("dirty.c", 1, f"int {WORD}_index = 0;")])

    def test_binary_file_is_skipped(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "blob.bin").write_bytes(b"\0" + WORD.encode())
            self.assertEqual(check.check(root, ["blob.bin"]), [])

    def test_repository_is_clean(self):
        self.assertEqual(check.check(check.ROOT, check.tracked_files(check.ROOT)), [])


if __name__ == "__main__":
    unittest.main()
