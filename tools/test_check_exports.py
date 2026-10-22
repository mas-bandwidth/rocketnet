#!/usr/bin/env python3
"""Tests for the source-side export guard in tools/check_exports.py."""

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_exports
import check_header_exports

REPO_ROOT = Path(__file__).resolve().parent.parent
HEADER = REPO_ROOT / "sdk" / "rocketnet.h"
TESTS = REPO_ROOT / "sdk" / "rocketnet_tests.cpp"
EXPORTS = REPO_ROOT / "tools" / "exports.txt"


def run_source_check(header_path, tests_path):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = check_exports.source_check(header_path, tests_path)
    return code, err.getvalue()


class ExportFixtureTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=REPO_ROOT)

    def tearDown(self):
        self.tmp.cleanup()

    def replace_once(self, text, marker, replacement):
        self.assertEqual(text.count(marker), 1, f"fixture marker must occur once: {marker!r}")
        changed = text.replace(marker, replacement, 1)
        self.assertNotEqual(changed, text)
        return changed

    def add_header_decl(self, header_text, development=False):
        self.assertNotIn("rocketnet_fixture_export", header_text)
        marker = "#if ROCKETNET_DEVELOPMENT\n"
        declaration = "ROCKETNET_EXPORT int rocketnet_fixture_export( void * p );\n"
        # Both positions are inside extern C; only the latter is development-only.
        replacement = marker + declaration if development else declaration + marker
        changed = self.replace_once(header_text, marker, replacement)
        self.assertEqual(changed.count(declaration), 1)
        return changed

    def add_table_entry(self, tests_text):
        self.assertNotIn('"rocketnet_fixture_export"', tests_text)
        return self.replace_once(
            tests_text,
            "rocketnet_test_exports[] =\n{",
            "rocketnet_test_exports[] =\n{\n    \"rocketnet_fixture_export\",",
        )


class CheckExportsSourceTest(ExportFixtureTest):
    def test_real_tree_passes(self):
        code, err = run_source_check(HEADER, TESTS)
        self.assertEqual(code, 0, err)

    def test_added_declaration_fails_naming_it(self):
        header_text = self.add_header_decl(HEADER.read_text())
        header_copy = Path(self.tmp.name) / "rocketnet.h"
        header_copy.write_text(header_text)
        tests_copy = Path(self.tmp.name) / "rocketnet_tests.cpp"
        tests_copy.write_text(TESTS.read_text())
        code, err = run_source_check(header_copy, tests_copy)
        self.assertNotEqual(code, 0)
        self.assertIn("rocketnet_fixture_export", err)

    def test_added_table_entry_passes_without_number_edit(self):
        header_text = self.add_header_decl(HEADER.read_text())
        header_copy = Path(self.tmp.name) / "rocketnet.h"
        header_copy.write_text(header_text)
        tests_copy = Path(self.tmp.name) / "rocketnet_tests.cpp"
        tests_copy.write_text(self.add_table_entry(TESTS.read_text()))
        code, err = run_source_check(header_copy, tests_copy)
        self.assertEqual(code, 0, err)
        before = len(check_exports.header_declared_names(HEADER))
        after = len(check_exports.header_declared_names(header_copy))
        self.assertEqual(after, before + 1)

    def test_duplicate_table_entry_fails(self):
        tests_text = self.replace_once(
            TESTS.read_text(),
            "rocketnet_test_exports[] =\n{",
            "rocketnet_test_exports[] =\n{\n    \"rocketnet_version\",",
        )
        tests_copy = Path(self.tmp.name) / "rocketnet_tests.cpp"
        tests_copy.write_text(tests_text)
        code, err = run_source_check(HEADER, tests_copy)
        self.assertNotEqual(code, 0)
        self.assertIn("Duplicate in rocketnet_test_exports: rocketnet_version", err)

    def test_dev_block_declaration_ignored(self):
        header_text = self.add_header_decl(HEADER.read_text(), development=True)
        header_copy = Path(self.tmp.name) / "rocketnet.h"
        header_copy.write_text(header_text)
        tests_copy = Path(self.tmp.name) / "rocketnet_tests.cpp"
        tests_copy.write_text(TESTS.read_text())
        code, err = run_source_check(header_copy, tests_copy)
        self.assertEqual(code, 0, err)
        self.assertNotIn("rocketnet_fixture_export", check_exports.header_declared_names(header_copy))

    def test_table_entry_without_declaration_fails_naming_it(self):
        tests_copy = Path(self.tmp.name) / "rocketnet_tests.cpp"
        tests_copy.write_text(self.add_table_entry(TESTS.read_text()))
        code, err = run_source_check(HEADER, tests_copy)
        self.assertNotEqual(code, 0)
        self.assertIn("Table name not declared in sdk/rocketnet.h: rocketnet_fixture_export", err)


def run_header_check(header_path, exports_path):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = check_header_exports.check(header_path, exports_path)
    return code, err.getvalue()


class CheckHeaderExportsTest(ExportFixtureTest):
    def test_real_tree_passes(self):
        code, err = run_header_check(HEADER, EXPORTS)
        self.assertEqual(code, 0, err)
        names = check_header_exports.header_export_names(HEADER)
        self.assertIn("rocketnet_assert_function", names)

    def test_extra_header_declaration_fails_naming_it(self):
        header_text = self.add_header_decl(HEADER.read_text())
        header_copy = Path(self.tmp.name) / "rocketnet.h"
        header_copy.write_text(header_text)
        code, err = run_header_check(header_copy, EXPORTS)
        self.assertNotEqual(code, 0)
        self.assertIn("rocketnet_fixture_export", err)

    def test_missing_list_name_fails_naming_it(self):
        exports_text = self.replace_once(EXPORTS.read_text(), "rocketnet_version\n", "")
        exports_copy = Path(self.tmp.name) / "exports.txt"
        exports_copy.write_text(exports_text)
        code, err = run_header_check(HEADER, exports_copy)
        self.assertNotEqual(code, 0)
        self.assertIn("rocketnet_version", err)

    def test_added_declaration_and_list_entry_pass(self):
        header_copy = Path(self.tmp.name) / "rocketnet.h"
        header_copy.write_text(self.add_header_decl(HEADER.read_text()))
        exports_text = EXPORTS.read_text()
        self.assertNotIn("rocketnet_fixture_export", exports_text)
        exports_copy = Path(self.tmp.name) / "exports.txt"
        exports_copy.write_text(exports_text + "\nrocketnet_fixture_export\n")
        code, err = run_header_check(header_copy, exports_copy)
        self.assertEqual(code, 0, err)
        self.assertIn("rocketnet_fixture_export", check_header_exports.header_export_names(header_copy))

    def test_dev_block_declaration_ignored(self):
        header_copy = Path(self.tmp.name) / "rocketnet.h"
        header_copy.write_text(self.add_header_decl(HEADER.read_text(), development=True))
        code, err = run_header_check(header_copy, EXPORTS)
        self.assertEqual(code, 0, err)
        self.assertNotIn("rocketnet_fixture_export", check_header_exports.header_export_names(header_copy))


if __name__ == "__main__":
    unittest.main()
