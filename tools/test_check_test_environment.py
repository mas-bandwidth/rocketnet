#!/usr/bin/env python3
"""Focused tests for the unit registration inventory and the output shape."""
import tempfile
import unittest
from pathlib import Path

import check_test_environment as checker


class RegisterNameTests(unittest.TestCase):
    def _guards(self, source):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test_unit_guards.cpp"
            path.write_text(source)
            return checker._register_guards(path)

    def test_dev_and_production_twins_are_exclusive(self):
        guards = self._guards("""\
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_twin );
#else
REGISTER_UNIT_TEST( test_other_twin );
#endif
#if !ROCKETNET_DEV
REGISTER_UNIT_TEST( test_twin );
#endif
#ifndef ROCKETNET_DEV
REGISTER_UNIT_TEST( test_other_twin );
#endif
""")
        dev, production = guards[0][1], guards[2][1]
        self.assertTrue(checker._guards_exclusive(dev, production))
        else_branch, ifndef = guards[1][1], guards[3][1]
        self.assertEqual(else_branch, frozenset([("ROCKETNET_DEV", False)]))
        self.assertEqual(ifndef, frozenset([("defined ROCKETNET_DEV", False)]))

    def test_repeat_under_compatible_or_opaque_guards_is_not_exclusive(self):
        guards = self._guards("""\
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_repeat );
#endif
#if defined(__linux__) && ROCKETNET_DEV
REGISTER_UNIT_TEST( test_repeat );
#elif ROCKETNET_DEV
REGISTER_UNIT_TEST( test_repeat );
#endif
REGISTER_UNIT_TEST( test_repeat );
""")
        for _, guard in guards[1:]:
            self.assertFalse(checker._guards_exclusive(guards[0][1], guard))

    def test_guarded_registration_is_conditional(self):
        source = """\
#if defined(__linux__)
#if defined(UDP_GRO)
REGISTER_UNIT_TEST( test_linux_gro );
#endif
#endif
REGISTER_UNIT_TEST( test_unguarded );
"""
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test_unit_platform.cpp"
            path.write_text(source)
            self.assertEqual(checker._register_names(path),
                             [("test_linux_gro", False), ("test_unguarded", True)])

    def test_directive_looking_comment_does_not_hide_unconditional_name(self):
        source = """\
/*
#if defined(__linux__)
*/
REGISTER_UNIT_TEST( test_unguarded_after_comment );
"""
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test_unit_comment.cpp"
            path.write_text(source)
            self.assertEqual(checker._register_names(path), [("test_unguarded_after_comment", True)])

    def test_comment_markers_in_literals_do_not_hide_unconditional_name(self):
        source = r'''
#if defined(__linux__)
const char *ordinary = "/*";
const char *raw = R"tag(/* literal */)tag";
#endif
const char *after = "*/";
REGISTER_UNIT_TEST( test_unguarded_after_literals );
'''
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test_unit_literals.cpp"
            path.write_text(source)
            self.assertEqual(checker._register_names(path), [("test_unguarded_after_literals", True)])

    def test_unguarded_missing_registration_still_fails(self):
        with self.assertRaises(SystemExit) as raised:
            checker.assert_name_set({"test_present"},
                                    {"test_present", "test_unguarded"},
                                    {"test_present", "test_unguarded"},
                                    "SDK suite")
        self.assertIn("missing from output: test_unguarded", str(raised.exception))


class ShapeTests(unittest.TestCase):
    def test_names_then_passed_line(self):
        self.assertEqual(checker.check_shape(["test_a", "test_b", "", checker.PASSED_LINE, ""], "suite"),
                         ["test_a", "test_b"])

    def test_skip_line_fails(self):
        with self.assertRaises(SystemExit):
            checker.check_shape(["test_a", "test_xdp_matcher [SKIPPED linux only]", "", checker.PASSED_LINE, ""], "suite")

    def test_capital_letter_fails(self):
        with self.assertRaises(SystemExit):
            checker.check_shape(["TestWindowRing", "", checker.PASSED_LINE, ""], "suite")

    def test_missing_passed_line_fails(self):
        with self.assertRaises(SystemExit):
            checker.check_shape(["test_a"], "suite")


if __name__ == "__main__":
    unittest.main()
