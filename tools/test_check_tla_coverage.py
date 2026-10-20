#!/usr/bin/env python3
"""Tests for the coverage checker in tools/check_tla_coverage.py."""

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_tla_coverage

REPO_ROOT = Path(__file__).resolve().parent.parent

HEAD = "\t".join(["machine", "code", "module", "status", "owed_by", "note"])
GOOD_NOTE = (
    "RNSendBudget models the send budget in sdk/rocketnet_send_budget.h "
    "(rocketnet_send_budget_init): Init."
)
HEADER_TEXT = "// tla/RNSendBudget.tla\nvoid rocketnet_send_budget_init(void);\n"
MODULE_TEXT = "MODULE RNSendBudget\n"


def run_check(root, argv=None):
    check_tla_coverage.ROOT = root
    check_tla_coverage.COVERAGE = root / "tla" / "COVERAGE.tsv"
    check_tla_coverage.TLA_DIR = root / "tla"
    old_argv = sys.argv
    sys.argv = ["check_tla_coverage.py"] + (argv or [])
    out = io.StringIO()
    code = 1
    try:
        with contextlib.redirect_stdout(out):
            code = check_tla_coverage.main()
    finally:
        sys.argv = old_argv
    return code, out.getvalue()


def write_row(tmp, row):
    line = "\t".join([row["machine"], row["code"], row["module"],
                      row["status"], row["owed_by"], row["note"]])
    (tmp / "tla" / "COVERAGE.tsv").write_text(HEAD + "\n" + line + "\n")


class CheckTlaCoverageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=REPO_ROOT)

    def tearDown(self):
        self.tmp.cleanup()

    def plant_basic_repo(self):
        root = Path(self.tmp.name)
        (root / "tla").mkdir()
        (root / "sdk").mkdir()
        (root / "tla" / "RNSendBudget.tla").write_text(MODULE_TEXT)
        (root / "sdk" / "rocketnet_send_budget.h").write_text(HEADER_TEXT)
        return root

    def test_comma_joined_path_is_refused(self):
        root = self.plant_basic_repo()
        for sep in [", ", ","]:
            write_row(root, {
                "machine": "rn-send-budget",
                "code": "sdk/rocketnet_send_budget.h" + sep + "sdk/rocketnet_endpoint.cpp",
                "module": "RNSendBudget",
                "status": "current",
                "owed_by": "-",
                "note": GOOD_NOTE,
            })
            code, out = run_check(root)
            self.assertNotEqual(code, 0)
            self.assertIn("rn-send-budget", out)
            self.assertIn("code column must hold one path", out)

    def test_good_one_path_row_passes(self):
        root = self.plant_basic_repo()
        write_row(root, {
            "machine": "rn-send-budget",
            "code": "sdk/rocketnet_send_budget.h",
            "module": "RNSendBudget",
            "status": "current",
            "owed_by": "-",
            "note": GOOD_NOTE,
        })
        code, out = run_check(root)
        self.assertEqual(code, 0, out)
        self.assertIn("COVERAGE OK 1 rows", out)

    def test_owed_row_whose_module_sdk_cites_is_refused(self):
        root = self.plant_basic_repo()
        write_row(root, {
            "machine": "rn-send-budget",
            "code": "-",
            "module": "RNSendBudget",
            "status": "owed",
            "owed_by": "some-card",
            "note": "RNSendBudget models the send budget; no code implements it yet.",
        })
        code, out = run_check(root)
        self.assertNotEqual(code, 0, out)
        self.assertIn("RNSendBudget: rn-send-budget is owed but "
                      "sdk/rocketnet_send_budget.h cites tla/RNSendBudget.tla", out)

    def test_owed_row_no_sdk_file_cites_passes(self):
        root = self.plant_basic_repo()
        (root / "sdk" / "rocketnet_send_budget.h").write_text("void rocketnet_send_budget_init(void);\n")
        write_row(root, {
            "machine": "rn-send-budget",
            "code": "-",
            "module": "RNSendBudget",
            "status": "owed",
            "owed_by": "some-card",
            "note": "RNSendBudget models the send budget; no code implements it yet.",
        })
        code, out = run_check(root)
        self.assertEqual(code, 0, out)

    def test_owed_row_cited_without_the_tla_prefix_is_refused(self):
        root = self.plant_basic_repo()
        (root / "sdk" / "rocketnet_send_budget.h").write_text(
            "// RNSendBudget.tla Init.\nvoid rocketnet_send_budget_init(void);\n")
        write_row(root, {
            "machine": "rn-send-budget",
            "code": "-",
            "module": "RNSendBudget",
            "status": "owed",
            "owed_by": "some-card",
            "note": "RNSendBudget models the send budget; no code implements it yet.",
        })
        code, out = run_check(root)
        self.assertNotEqual(code, 0, out)
        self.assertIn("RNSendBudget: rn-send-budget is owed but "
                      "sdk/rocketnet_send_budget.h cites RNSendBudget.tla", out)

    def test_real_coverage_passes(self):
        code, out = run_check(REPO_ROOT, ["--all"])
        self.assertEqual(code, 0, out)
        self.assertIn("COVERAGE OK", out)


if __name__ == "__main__":
    unittest.main()
