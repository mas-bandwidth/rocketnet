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
HEADER_TEXT = "// proofs/tla/RNSendBudget.tla\nvoid rocketnet_send_budget_init(void);\n"
MODULE_TEXT = "MODULE RNSendBudget\n"


def run_check(root, argv=None):
    check_tla_coverage.ROOT = root
    check_tla_coverage.COVERAGE = root / "proofs" / "tla" / "COVERAGE.tsv"
    check_tla_coverage.TLA_DIR = root / "proofs" / "tla"
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
    (tmp / "proofs" / "tla" / "COVERAGE.tsv").write_text(HEAD + "\n" + line + "\n")


class CheckTlaCoverageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=REPO_ROOT)

    def tearDown(self):
        self.tmp.cleanup()

    def plant_basic_repo(self):
        root = Path(self.tmp.name)
        (root / "proofs" / "tla").mkdir(parents=True)
        (root / "sdk").mkdir()
        (root / "proofs" / "tla" / "RNSendBudget.tla").write_text(MODULE_TEXT)
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
                      "sdk/rocketnet_send_budget.h cites proofs/tla/RNSendBudget.tla", out)

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

    def test_planted_wrong_function_note_fails(self):
        root = self.plant_basic_repo()
        write_row(root, {
            "machine": "rn-send-budget",
            "code": "sdk/rocketnet_send_budget.h",
            "module": "RNSendBudget",
            "status": "current",
            "owed_by": "-",
            "note": ("RNSendBudget models the send budget in "
                     "sdk/rocketnet_send_budget.h "
                     "(rocketnet_send_budget_missing): Init."),
        })
        code, out = run_check(root)
        self.assertNotEqual(code, 0, out)
        self.assertIn("rocketnet_send_budget_missing not defined", out)

    def test_space_then_path_absent_name_fails(self):
        root = self.plant_basic_repo()
        write_row(root, {
            "machine": "rn-send-budget",
            "code": "sdk/rocketnet_send_budget.h",
            "module": "RNSendBudget",
            "status": "current",
            "owed_by": "-",
            "note": ("RNSendBudget models the send budget: "
                     "rocketnet_send_budget_missing "
                     "(sdk/rocketnet_send_budget.h) and "
                     "rocketnet_send_budget_init."),
        })
        code, out = run_check(root)
        self.assertNotEqual(code, 0, out)
        self.assertIn("rocketnet_send_budget_missing not defined in "
                      "sdk/rocketnet_send_budget.h", out)

    def test_space_then_path_other_file_passes(self):
        root = self.plant_basic_repo()
        (root / "sdk" / "other.h").write_text(
            "void rocketnet_other_init(void);\n")
        write_row(root, {
            "machine": "rn-send-budget",
            "code": "sdk/rocketnet_send_budget.h",
            "module": "RNSendBudget",
            "status": "current",
            "owed_by": "-",
            "note": ("RNSendBudget models the send budget in "
                     "sdk/rocketnet_send_budget.h "
                     "(rocketnet_send_budget_init) with "
                     "rocketnet_other_init (sdk/other.h): Init."),
        })
        code, out = run_check(root)
        self.assertEqual(code, 0, out)

    def test_citation_inside_named_symbol_passes(self):
        root = self.plant_basic_repo()
        (root / "sdk" / "rocketnet_driver.h").write_text(
            "// rocketnet_send_budget_init "
            "(sdk/rocketnet_send_budget.h:2)\n")
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

    def test_citation_outside_named_symbol_fails(self):
        root = self.plant_basic_repo()
        (root / "sdk" / "rocketnet_send_budget.h").write_text(
            "// proofs/tla/RNSendBudget.tla\n"
            "void rocketnet_send_budget_init(void);\n"
            "void rocketnet_other_init(void)\n{\n}\n")
        (root / "sdk" / "rocketnet_driver.h").write_text(
            "// rocketnet_send_budget_init "
            "(sdk/rocketnet_send_budget.h:4)\n")
        write_row(root, {
            "machine": "rn-send-budget",
            "code": "sdk/rocketnet_send_budget.h",
            "module": "RNSendBudget",
            "status": "current",
            "owed_by": "-",
            "note": GOOD_NOTE,
        })
        code, out = run_check(root)
        self.assertNotEqual(code, 0, out)
        self.assertIn("is inside rocketnet_other_init, not "
                      "rocketnet_send_budget_init", out)

    def test_wrapped_citation_outside_named_symbol_fails(self):
        root = self.plant_basic_repo()
        (root / "sdk" / "rocketnet_send_budget.h").write_text(
            "// proofs/tla/RNSendBudget.tla\n"
            "void rocketnet_send_budget_init(void);\n"
            "void rocketnet_other_init(void)\n{\n}\n")
        (root / "proofs" / "tla" / "ReplayWindow.tla").write_text(
            "(* rocketnet_send_budget_init (sdk/rocketnet_send_budget.h:  *)\n"
            "(* 4) *)\n")
        write_row(root, {
            "machine": "rn-send-budget",
            "code": "sdk/rocketnet_send_budget.h",
            "module": "RNSendBudget",
            "status": "current",
            "owed_by": "-",
            "note": GOOD_NOTE,
        })
        code, out = run_check(root)
        self.assertNotEqual(code, 0)
        self.assertIn("is inside rocketnet_other_init, not "
                      "rocketnet_send_budget_init", out)

    def test_real_coverage_passes(self):
        code, out = run_check(REPO_ROOT, ["--all"])
        self.assertEqual(code, 0, out)
        self.assertIn("COVERAGE OK", out)

    # ------------------------------------------------------------------
    # Per-action citation checks (--actions / --actions-strict).
    # ------------------------------------------------------------------

    ACTION_MODULE = r"""MODULE ActMod
VARIABLES x
ActionA == /\ x = 0 /\ x' = 1
ActionB == /\ x = 1 /\ x' = 0
EnvStep == /\ x = 0 /\ UNCHANGED x
Next == ActionA \/ ActionB \/ EnvStep
"""

    ACTION_NOTE = "ActMod models the demo view of sdk/act.c."

    def plant_action_repo(self, code_text, note=None, module_text=None):
        root = Path(self.tmp.name)
        (root / "proofs" / "tla").mkdir(parents=True)
        (root / "sdk").mkdir()
        (root / "proofs" / "tla" / "ActMod.tla").write_text(module_text or self.ACTION_MODULE)
        (root / "sdk" / "act.c").write_text(code_text)
        write_row(root, {
            "machine": "rn-act",
            "code": "sdk/act.c",
            "module": "ActMod",
            "status": "current",
            "owed_by": "-",
            "note": note or self.ACTION_NOTE,
        })
        return root

    def test_actions_read_from_next(self):
        code_text = ("// proofs/tla/ActMod.tla ActionA\n"
                     "// proofs/tla/ActMod.tla ActionB\n"
                     "// proofs/tla/ActMod.tla EnvStep\n")
        root = self.plant_action_repo(code_text)
        code, out = run_check(root, ["--actions"])
        self.assertEqual(code, 0, out)
        self.assertIn("ACTIONS UNCITED 0", out)

    def test_actions_uncited_action_reported(self):
        code_text = "// proofs/tla/ActMod.tla ActionA\n"
        root = self.plant_action_repo(code_text)
        code, out = run_check(root, ["--actions"])
        self.assertEqual(code, 0, out)
        self.assertIn("ActMod ActionB: sdk/act.c", out)
        self.assertIn("ActMod EnvStep: sdk/act.c", out)
        self.assertIn("ACTIONS UNCITED 2", out)

    def test_actions_citation_spans_comment_lines(self):
        code_text = "// proofs/tla/ActMod.tla ActionA and\n// ActionB\n"
        root = self.plant_action_repo(code_text)
        code, out = run_check(root, ["--actions"])
        self.assertEqual(code, 0, out)
        self.assertIn("ACTIONS UNCITED 1", out)
        self.assertNotIn("ActMod ActionB:", out)

    def test_actions_cited_in_any_current_row_file(self):
        root = Path(self.tmp.name)
        (root / "proofs" / "tla").mkdir(parents=True)
        (root / "sdk").mkdir()
        (root / "proofs" / "tla" / "ActMod.tla").write_text(self.ACTION_MODULE)
        (root / "sdk" / "act.c").write_text(
            "// proofs/tla/ActMod.tla ActionB\n// proofs/tla/ActMod.tla EnvStep\n")
        (root / "sdk" / "act2.c").write_text("// proofs/tla/ActMod.tla ActionA\n")
        head = "\t".join(["machine", "code", "module", "status", "owed_by", "note"])
        lines = [head,
                 "\t".join(["rn-act", "sdk/act.c", "ActMod", "current", "-", self.ACTION_NOTE]),
                 "\t".join(["rn-act-2", "sdk/act2.c", "ActMod", "current", "-", self.ACTION_NOTE])]
        (root / "proofs" / "tla" / "COVERAGE.tsv").write_text("\n".join(lines) + "\n")
        code, out = run_check(root, ["--actions"])
        self.assertEqual(code, 0, out)
        self.assertIn("ACTIONS UNCITED 0", out)

    WITNESS_MODULE = (
        "---- MODULE ActMod ----\n"
        "CONSTANTS Broken\n"
        "VARIABLES x\n"
        "ActionA == x' = 1\n"
        "Defect ==\n"
        "    /\\ \"defect\" \\in Broken /\\ x = 0\n"
        "    /\\ x' = 2\n"
        "Next == ActionA \\/ Defect\n"
        "====\n"
    )

    def plant_witness_cfgs(self, root, passing):
        (root / "proofs" / "tla" / "MCActMod.cfg").write_text("CONSTANTS\n    Broken = %s\n" % passing)
        (root / "proofs" / "tla" / "MCActModBrokenDefect.cfg").write_text(
            "CONSTANTS\n    Broken = {\"defect\"}\n")

    def test_actions_witness_only_exempt_from_cfgs(self):
        root = self.plant_action_repo("// proofs/tla/ActMod.tla ActionA\n",
                                      module_text=self.WITNESS_MODULE)
        self.plant_witness_cfgs(root, "{}")
        code, out = run_check(root, ["--actions"])
        self.assertEqual(code, 0, out)
        self.assertIn("ACTIONS UNCITED 0", out)
        self.assertIn("WITNESS-ONLY ACTIONS", out)
        self.assertIn("ActMod Defect: MCActModBrokenDefect", out)

    def test_actions_witness_guard_enabled_by_passing_cfg_is_cited(self):
        root = self.plant_action_repo("// proofs/tla/ActMod.tla ActionA\n",
                                      module_text=self.WITNESS_MODULE)
        self.plant_witness_cfgs(root, "{\"defect\"}")
        code, out = run_check(root, ["--actions"])
        self.assertEqual(code, 0, out)
        self.assertIn("ActMod Defect: sdk/act.c", out)
        self.assertIn("ACTIONS UNCITED 1", out)
        self.assertIn("ACTIONS WITNESS-ONLY 0", out)

    def test_actions_environment_note_exempts(self):
        code_text = ("// proofs/tla/ActMod.tla ActionA\n"
                     "// proofs/tla/ActMod.tla ActionB\n")
        note = "ActMod models the demo; environment actions EnvStep."
        root = self.plant_action_repo(code_text, note=note)
        code, out = run_check(root, ["--actions"])
        self.assertEqual(code, 0, out)
        self.assertIn("ACTIONS UNCITED 0", out)

    def test_actions_environment_note_names_unknown_action_fails(self):
        code_text = ("// proofs/tla/ActMod.tla ActionA\n"
                     "// proofs/tla/ActMod.tla ActionB\n")
        note = "ActMod models the demo; environment actions EnvStep and Bogus."
        root = self.plant_action_repo(code_text, note=note)
        code, out = run_check(root, ["--actions-strict"])
        self.assertNotEqual(code, 0, out)
        self.assertIn("unknown action Bogus", out)

    def test_actions_strict_fails_on_uncited(self):
        code_text = "// proofs/tla/ActMod.tla ActionA\n"
        root = self.plant_action_repo(code_text)
        code, out = run_check(root, ["--actions-strict"])
        self.assertNotEqual(code, 0, out)
        self.assertIn("ACTIONS UNCITED 2", out)

    def test_actions_model_checking_module_does_not_cite(self):
        code_text = ("// proofs/tla/ActMod.tla ActionA ActionB\n"
                     "\n"
                     "// proofs/tla/MCActMod.tla EnvStep\n")
        root = self.plant_action_repo(code_text)
        code, out = run_check(root, ["--actions-strict"])
        self.assertNotEqual(code, 0, out)
        self.assertIn("ActMod EnvStep: sdk/act.c", out)
        self.assertIn("ACTIONS UNCITED 1", out)

    def test_actions_trailing_comment_cites(self):
        code_text = ("int x = 0;    // proofs/tla/ActMod.tla ActionA\n"
                     "\n"
                     "// proofs/tla/ActMod.tla ActionB and EnvStep\n")
        root = self.plant_action_repo(code_text)
        code, out = run_check(root, ["--actions-strict"])
        self.assertEqual(code, 0, out)
        self.assertIn("ACTIONS UNCITED 0", out)


if __name__ == "__main__":
    unittest.main()
