#!/usr/bin/env python3
"""Tests for tools/check_client_server_words.py: each control must go red."""

import hashlib
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
        self.assertEqual(check.violations_in_text("scripts/netem-pair-soak", line), [])

    def test_veth_syntax_does_not_excuse_the_rest_of_the_line(self):
        line = f'ip link add a type veth {WORD} name b  # the {WORD} end'
        self.assertEqual(len(check.violations_in_text("scripts/netem-pair-soak", line)), 1)

    def test_type_error_is_allowed(self):
        self.assertEqual(check.violations_in_text("tools/profile_1000_load.py", "except (TypeError, ValueError):"),
                         [])
        self.assertEqual(check.violations_in_text("tools/run_backend_examples.py",
                                                  "raise argparse.ArgumentTypeError('x')"), [])

    def test_span_outside_its_files_fails(self):
        # VERIFY-3 A-6's plant: the second word's helper name in the public header
        planted = "struct planted_t { int host_" + check.WORD2 + "; };"
        self.assertEqual(check.violations_in_text("tools/run_xdp_filter_guest.sh", planted), [])
        self.assertEqual(len(check.violations_in_text("sdk/rocketnet.h", planted)), 1)
        self.assertEqual(len(check.violations_in_text("a.sh", f"ip link add a type veth {WORD} name b")), 1)

    def test_path_with_the_word_fails(self):
        self.assertEqual(len(check.violations_in_path(f"tests/functional/posix_hard_killed_{WORD}.cpp")), 1)
        self.assertEqual(check.violations_in_path("tests/functional/posix_hard_killed_client.cpp"), [])


class TreeTest(unittest.TestCase):

    def test_tree_control_goes_red_and_allowed_file_stays_green(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            licence = f"{WORD}-to-{WORD} transmission\n"
            (root / "LICENSE").write_text(licence)
            allowed = {"LICENSE": (hashlib.sha256(licence.encode()).hexdigest(), "control")}
            (root / "clean.c").write_text("// the client\n")
            self.assertEqual(check.check(root, ["LICENSE", "clean.c"], allowed, (), ()), [])
            (root / "dirty.c").write_text(f"int {WORD}_index = 0;\n")
            self.assertEqual(check.check(root, ["LICENSE", "clean.c", "dirty.c"], allowed, (), ()),
                             [("dirty.c", 1, f"int {WORD}_index = 0;")])

    def test_new_text_in_an_allowed_file_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            licence = f"{WORD}-to-{WORD} transmission\n"
            (root / "LICENSE").write_text(licence)
            allowed = {"LICENSE": (hashlib.sha256(licence.encode()).hexdigest(), "control")}
            (root / "LICENSE").write_text(licence + f"a new {WORD} line\n")
            found = check.check(root, ["LICENSE"], allowed, (), ())
            self.assertTrue(any("allowed whole file changed" in line for _, _, line in found), found)
            self.assertIn(("LICENSE", 2, f"a new {WORD} line"), found)

    def test_stale_exceptions_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "a.sh").write_text("echo nothing here\n")
            allowed = {"gone.md": ("0" * 64, "control")}
            spans = ((check.re.compile(r"\btype veth " + WORD + r" name\b"), ("a.sh",), "control"),)
            found = check.check(root, ["a.sh"], allowed, spans, ())
            self.assertIn(("gone.md", 0, "stale whole-file exception: no such tracked file"), found)
            self.assertTrue(any(path == "a.sh" and "stale span exception" in line for path, _, line in found), found)
            (root / "a.sh").write_text(f"ip link add a type veth {WORD} name b\n")
            self.assertEqual(check.check(root, ["a.sh"], {}, spans, ()), [])

    def test_binary_file_is_skipped(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "blob.bin").write_bytes(b"\0" + WORD.encode())
            self.assertEqual(check.check(root, ["blob.bin"], {}, (), ()), [])

    def test_zero_tracked_files_is_an_error(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(check.main(["--root", directory]), 1)

    def test_repository_is_clean(self):
        self.assertEqual(check.check(check.ROOT, check.tracked_files(check.ROOT)), [])


if __name__ == "__main__":
    unittest.main()
