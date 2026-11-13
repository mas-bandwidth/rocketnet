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


class TableNameTests(unittest.TestCase):
    def _names(self, source):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "table.cpp"
            path.write_text(source)
            return checker._table_names(path)

    def test_null_predicate_is_every_host(self):
        all_names, always = self._names("""\
static const entry_t table[] = {
    { "test_two_fields", fn_a },
    { "test_null", fn_b, NULL },
    { "test_nullptr", fn_c, nullptr },
    { "test_zero", fn_d, 0 },
    { "test_predicate", fn_e, runs_here_linux },
#if defined(__linux__)
    { "test_guarded", fn_f, NULL },
#endif
};
""")
        self.assertEqual(all_names, {"test_two_fields", "test_null", "test_nullptr", "test_zero",
                                     "test_predicate", "test_guarded"})
        self.assertEqual(always, {"test_two_fields", "test_null", "test_nullptr", "test_zero"})


class DuplicateRegistrationTests(unittest.TestCase):
    # registered_unit_names refuses a name registered twice: in a table and a
    # unit file, or in two unit files under guards that can meet in one binary
    def _registered(self, table, units):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "sdk").mkdir()
            (root / "tests").mkdir()
            (root / "sdk" / "rocketnet_tests.cpp").write_text(table)
            (root / "tests" / "test_channels.cpp").write_text("")
            for name, text in units.items():
                (root / "tests" / name).write_text(text)
            return checker.registered_unit_names(root=root)

    TABLE = '    { "test_in_table", rocketnet_test_in_table },\n'

    def test_distinct_names_pass(self):
        all_names, always = self._registered(self.TABLE, {
            "test_unit_a.cpp": "REGISTER_UNIT_TEST( test_a );\n",
            "test_unit_b.cpp": "#if defined(__linux__)\nREGISTER_UNIT_TEST( test_b );\n#endif\n",
        })
        self.assertEqual(all_names, {"test_in_table", "test_a", "test_b"})
        self.assertEqual(always, {"test_in_table", "test_a"})

    def test_a_table_name_registered_again_is_refused(self):
        with self.assertRaisesRegex(SystemExit, r"registered more than once: test_in_table \(registry table, test_unit_a\.cpp\)"):
            self._registered(self.TABLE, {"test_unit_a.cpp": "REGISTER_UNIT_TEST( test_in_table );\n"})

    def test_a_repeat_under_compatible_guards_is_refused(self):
        with self.assertRaisesRegex(SystemExit, r"registered more than once: test_twice \(test_unit_a\.cpp, test_unit_b\.cpp\)"):
            self._registered(self.TABLE, {
                "test_unit_a.cpp": "#if ROCKETNET_DEV\nREGISTER_UNIT_TEST( test_twice );\n#endif\n",
                "test_unit_b.cpp": "#if defined(__linux__)\nREGISTER_UNIT_TEST( test_twice );\n#endif\n",
            })

    def test_exclusive_guard_twins_pass(self):
        all_names, _ = self._registered(self.TABLE, {
            "test_unit_a.cpp": "#if ROCKETNET_DEV\nREGISTER_UNIT_TEST( test_twin );\n#endif\n",
            "test_unit_b.cpp": "#if !ROCKETNET_DEV\nREGISTER_UNIT_TEST( test_twin );\n#endif\n",
        })
        self.assertIn("test_twin", all_names)


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

    def test_zero_names_fails_naming_the_count(self):
        for lines in (["", checker.PASSED_LINE, ""], ["", "", checker.PASSED_LINE, ""]):
            with self.assertRaisesRegex(SystemExit, "0 test names printed"):
                checker.check_shape(lines, "suite")

    def test_missing_passed_line_fails(self):
        with self.assertRaises(SystemExit):
            checker.check_shape(["test_a"], "suite")

    def test_repeated_name_fails_once(self):
        with self.assertRaises(SystemExit):
            checker.check_shape(["test_a", "test_a", "test_b", "", checker.PASSED_LINE, ""], "suite", once=True)

    def test_repeated_name_passes_without_once(self):
        self.assertEqual(checker.check_shape(["test_a", "test_a", "", checker.PASSED_LINE, ""], "suite"),
                         ["test_a", "test_a"])


class FunctionalLogTests(unittest.TestCase):
    ROWS = {"test_a": (True, None), "test_b": (True, None)}

    def check(self, *middle):
        lines = ["test_a", "test_b", "skipped: 0", *middle, "", checker.PASSED_LINE, ""]
        return checker.check_functional_log(lines, self.ROWS, None)

    def test_whole_run_passes(self):
        self.assertEqual(self.check(), [])

    def test_keep_going_summary_of_a_passing_run_passes(self):
        self.assertEqual(self.check("keep going: 0 failed; 2 of 2 collected cases ran"), [])

    def test_keep_going_summary_with_a_failure_is_refused(self):
        self.assertTrue(self.check("keep going: 1 failed; 2 of 2 collected cases ran"))

    def test_keep_going_summary_with_a_case_unrun_is_refused(self):
        self.assertTrue(self.check("keep going: 0 failed; 1 of 2 collected cases ran"))

    def test_two_keep_going_summaries_are_refused(self):
        line = "keep going: 0 failed; 2 of 2 collected cases ran"
        self.assertTrue(self.check(line, line))



class SelfNamePrintTests(unittest.TestCase):
    def scan(self, text):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test_unit_example.cpp"
            path.write_text(text)
            return checker.unit_sources_printing_names([path])

    def test_case_printing_its_name_is_found(self):
        self.assertEqual(self.scan('static void test_a()\n{\n    printf( "test_a\\n" );\n}\n'),
                         ["test_unit_example.cpp:3 prints test_a"])

    def test_other_printf_is_not_found(self):
        self.assertEqual(self.scan('    printf( "  slept %.3f\\n", t );\n    printf( "test_a %d\\n", 1 );\n'), [])

    def test_tree_has_none(self):
        self.assertEqual(checker.unit_sources_printing_names(), [])


if __name__ == "__main__":
    unittest.main()
