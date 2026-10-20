#!/usr/bin/env python3
"""Focused tests for platform-aware unit registration inventory."""
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import check_test_environment as checker


class RegisterNameTests(unittest.TestCase):
    def test_linux_guarded_registration_is_platform_aware(self):
        source = """\
#if defined(__linux__)
#if defined(UDP_GRO)
REGISTER_UNIT_TEST( linux_gro_test );
#endif
#else
REGISTER_UNIT_TEST( other_host_test );
#endif
REGISTER_UNIT_TEST( unguarded_test );
"""
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test_unit_platform.cpp"
            path.write_text(source)
            with patch.object(checker.sys, "platform", "darwin"):
                self.assertEqual(checker._register_names(path), ["other_host_test", "unguarded_test"])
            with patch.object(checker.sys, "platform", "linux"):
                self.assertEqual(
                    checker._register_names(path),
                    ["linux_gro_test", "unguarded_test"],
                )

    def test_directive_looking_comment_does_not_hide_unconditional_name(self):
        source = """\
/*
#if defined(__linux__)
*/
REGISTER_UNIT_TEST( unguarded_after_comment );
"""
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test_unit_comment.cpp"
            path.write_text(source)
            with patch.object(checker.sys, "platform", "darwin"):
                self.assertEqual(
                    checker._register_names(path), ["unguarded_after_comment"]
                )

    def test_comment_markers_in_literals_do_not_hide_unconditional_name(self):
        source = r'''
#if defined(__linux__)
const char *ordinary = "/*";
const char *raw = R"tag(/* literal */)tag";
#endif
const char *after = "*/";
REGISTER_UNIT_TEST( unguarded_after_literals );
'''
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test_unit_literals.cpp"
            path.write_text(source)
            with patch.object(checker.sys, "platform", "darwin"):
                self.assertEqual(
                    checker._register_names(path), ["unguarded_after_literals"]
                )

    def test_unguarded_missing_registration_still_fails(self):
        with self.assertRaises(SystemExit) as raised:
            checker.assert_name_set({"present_test"},
                                    {"present_test", "unguarded_test"},
                                    "SDK suite")
        self.assertIn("missing from output: unguarded_test", str(raised.exception))


if __name__ == "__main__":
    unittest.main()
