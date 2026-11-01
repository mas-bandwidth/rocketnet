#!/usr/bin/env python3
"""Verdicts of tools/check_unit_case_coverage.py on fixture trees."""

from __future__ import annotations

import shlex
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_unit_case_coverage as coverage  # noqa: E402

SDK_TABLE = """
static const struct rocketnet_unit_test_t rocketnet_unit_tests[] = {
    { "test_sdk_a", rocketnet_test_sdk_a },
#if !ROCKETNET_DEV
    { "test_sdk_production", rocketnet_test_sdk_production },
#endif // #if !ROCKETNET_DEV
};
"""

CHANNELS_TABLE = """
static const channels_test_t channels_tests[] = {
    { "test_channels_a", "ChannelsA", test_channels_a },
};
"""

UNIT_FILE = """
#include "test_unit_registry.h"
// REGISTER_UNIT_TEST( test_in_a_comment ) is not a registration
static void test_file_a() {}
REGISTER_UNIT_TEST( test_file_a );
#if !ROCKETNET_DEV
static void test_file_production() {}
REGISTER_UNIT_TEST_EXPECTED_FAIL( test_file_production, "CARD-1" );
#endif // #if !ROCKETNET_DEV
"""

ALL_CASES = ["test_sdk_a", "test_sdk_production", "test_channels_a", "test_file_a", "test_file_production"]


class CoverageTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.repo = Path(self.temp.name) / "repo"
        (self.repo / "sdk").mkdir(parents=True)
        (self.repo / "tests").mkdir()
        (self.repo / "sdk" / "rocketnet_tests.cpp").write_text(SDK_TABLE)
        (self.repo / "tests" / "test_channels.cpp").write_text(CHANNELS_TABLE)
        (self.repo / "tests" / "test_unit_fixture.cpp").write_text(UNIT_FILE)
        # a registration outside the unit suite's sources is not a unit case
        (self.repo / "tests" / "source_audit_fixture.cpp").write_text("REGISTER_UNIT_TEST( test_source_audit );\n")
        self.lists = Path(self.temp.name) / "lists"
        self.lists.mkdir()

    def tearDown(self) -> None:
        self.temp.cleanup()

    def write_list(self, name: str, cases: list[str]) -> Path:
        path = self.lists / f"{name}.txt"
        path.write_text("".join(f"{case}\r\n" for case in cases))
        return path

    def test_scan_finds_every_registration(self) -> None:
        self.assertEqual(sorted(coverage.registered_cases(self.repo)), sorted(ALL_CASES))

    def test_every_case_in_some_configuration_passes(self) -> None:
        development = self.write_list("Linux-Debug", ["test_sdk_a", "test_channels_a", "test_file_a"])
        production = self.write_list("Linux-Release", ["test_sdk_production", "test_file_a", "test_file_production"])
        self.assertEqual(coverage.check(self.repo, [development, production], ["Linux-Debug", "Linux-Release"]), [])
        self.assertEqual(coverage.main(["--repo", str(self.repo), str(development), str(production)]), 0)

    def test_workflow_command_separates_expected_names_from_list_files(self) -> None:
        # Run the real workflow command against a complete fixture inventory.
        # Without the -- delimiter, --expect consumes the expanded file paths.
        workflow = Path(__file__).resolve().parents[1] / ".github" / "workflows" / "ci.yml"
        lines = iter(workflow.read_text().splitlines())
        line = next(line for line in lines if "python3 tools/check_unit_case_coverage.py --repo ." in line)
        command = []
        while True:
            continued = line.rstrip().endswith('\\')
            command.append(line.strip().removesuffix('\\'))
            if not continued:
                break
            line = next(lines)
        args = shlex.split(" ".join(command))[2:]
        args[args.index("--repo") + 1] = str(self.repo)
        expected = [name for name in args[args.index("--expect") + 1:] if name not in ("--", "unit-cases/*.txt")]
        paths = [str(self.write_list(name, ALL_CASES)) for name in expected]
        pattern = args.index("unit-cases/*.txt")
        args[pattern:pattern + 1] = paths
        self.assertEqual(coverage.main(args), 0)

    def test_cli_without_list_files_fails(self) -> None:
        self.assertEqual(coverage.main(["--repo", str(self.repo), "--expect", "Linux-Debug", "--"]), 1)

    def test_a_case_in_no_configuration_fails(self) -> None:
        # the development-only configuration never compiles the production cases
        development = self.write_list("Linux-Debug", ["test_sdk_a", "test_channels_a", "test_file_a"])
        errors = coverage.check(self.repo, [development], [])
        self.assertEqual(len(errors), 2, errors)
        self.assertIn("test_file_production (tests/test_unit_fixture.cpp) is compiled into no CI configuration", errors[0])
        self.assertIn("test_sdk_production (sdk/rocketnet_tests.cpp) is compiled into no CI configuration", errors[1])
        self.assertEqual(coverage.main(["--repo", str(self.repo), str(development)]), 1)

    def test_a_listed_case_the_scan_missed_fails(self) -> None:
        full = self.write_list("Linux-Debug", ALL_CASES + ["test_from_a_new_registry"])
        errors = coverage.check(self.repo, [full], [])
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("test_from_a_new_registry is listed by Linux-Debug but no registration was found", errors[0])

    def test_a_missing_expected_configuration_fails(self) -> None:
        full = self.write_list("Linux-Debug", ALL_CASES)
        errors = coverage.check(self.repo, [full], ["Linux-Debug", "Windows-a"])
        self.assertEqual(errors, ["no list file for expected configuration Windows-a"])
        self.assertEqual(coverage.main(["--repo", str(self.repo), "--expect", "Linux-Debug", "Windows-a", "--", str(full)]), 1)

    def test_an_empty_list_fails(self) -> None:
        full = self.write_list("Linux-Debug", ALL_CASES)
        empty = self.write_list("macOS-Debug", [])
        errors = coverage.check(self.repo, [full, empty], [])
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("empty list for configuration macOS-Debug", errors[0])

    def test_a_case_registered_twice_fails(self) -> None:
        (self.repo / "tests" / "test_unit_second.cpp").write_text("REGISTER_UNIT_TEST( test_file_a );\n")
        full = self.write_list("Linux-Debug", ALL_CASES)
        errors = coverage.check(self.repo, [full], [])
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("test_file_a is registered in both", errors[0])

    def test_a_missing_table_fails(self) -> None:
        (self.repo / "tests" / "test_channels.cpp").write_text("// no table\n")
        full = self.write_list("Linux-Debug", ALL_CASES)
        errors = coverage.check(self.repo, [full], [])
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("table opener 'channels_tests[] = {' not found", errors[0])


if __name__ == "__main__":
    unittest.main()
