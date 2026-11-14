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
HEADER_TEXT = ("// proofs/tla/RNSendBudget.tla\nvoid rocketnet_send_budget_init(void);\n"
               "void rocketnet_send_budget_init(void)\n{\n}\n")
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

    def test_zero_rows_fails(self):
        # zero inputs is an error: a header-only, empty or missing table checks nothing
        root = self.plant_basic_repo()
        coverage = root / "proofs" / "tla" / "COVERAGE.tsv"
        for text in (HEAD + "\n", ""):
            coverage.write_text(text)
            code, out = run_check(root)
            self.assertEqual(code, 1, out)
            self.assertIn("0 rows", out)
            self.assertNotIn("COVERAGE OK", out)
        coverage.unlink()
        code, out = run_check(root)
        self.assertEqual(code, 1, out)
        self.assertIn("0 rows", out)

    def test_malformed_row_width_fails_after_a_valid_row(self):
        root = self.plant_basic_repo()
        fields = ["malformed", "sdk/rocketnet_send_budget.h", "RNSendBudget",
                  "current", "-", GOOD_NOTE]
        for malformed in [fields[:-1], fields + ["unexpected"]]:
            with self.subTest(columns=len(malformed)):
                write_row(root, {
                    "machine": "rn-send-budget",
                    "code": "sdk/rocketnet_send_budget.h",
                    "module": "RNSendBudget",
                    "status": "current",
                    "owed_by": "-",
                    "note": GOOD_NOTE,
                })
                coverage = root / "proofs" / "tla" / "COVERAGE.tsv"
                with coverage.open("a") as out:
                    out.write("\t".join(malformed) + "\n")
                code, out = run_check(root)
                self.assertEqual(code, 1, out)
                self.assertIn(f"proofs/tla/COVERAGE.tsv:3: expected 6 columns, "
                              f"found {len(malformed)}", out)
                self.assertNotIn("COVERAGE OK", out)

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

    def test_path_then_symbol_citation_passes(self):
        root = self.plant_basic_repo()
        (root / "sdk" / "rocketnet_driver.h").write_text(
            "// rocketnet_send_budget_init "
            "(sdk/rocketnet_send_budget.h)\n"
            "// sdk/rocketnet_send_budget.h (rocketnet_send_budget_init)\n")
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

    def test_line_citation_in_code_is_refused(self):
        root = self.plant_basic_repo()
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
        self.assertIn("sdk/rocketnet_driver.h:1: line citation "
                      "sdk/rocketnet_send_budget.h:4; cite the symbol", out)

    def test_line_citation_in_test_file_is_refused(self):
        root = self.plant_basic_repo()
        (root / "tests").mkdir()
        (root / "tests" / "test_unit_budget.cpp").write_text(
            "// see sdk/rocketnet_send_budget.h:2-4\n")
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
        self.assertIn("tests/test_unit_budget.cpp:1: line citation "
                      "sdk/rocketnet_send_budget.h:2", out)

    def test_coverage_tsv_line_citation_is_refused(self):
        root = self.plant_basic_repo()
        write_row(root, {
            "machine": "rn-send-budget",
            "code": "sdk/rocketnet_send_budget.h",
            "module": "RNSendBudget",
            "status": "current",
            "owed_by": "-",
            "note": ("RNSendBudget models the send budget in "
                     "rocketnet_send_budget_init "
                     "(sdk/rocketnet_send_budget.h:4): Init."),
        })
        code, out = run_check(root)
        self.assertNotEqual(code, 0, out)
        self.assertIn("proofs/tla/COVERAGE.tsv:2: line citation "
                      "sdk/rocketnet_send_budget.h:4", out)

    def test_wrapped_line_citation_is_refused(self):
        root = self.plant_basic_repo()
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
        self.assertIn("proofs/tla/ReplayWindow.tla:1: line citation "
                      "sdk/rocketnet_send_budget.h:4", out)

    def test_bare_line_reference_is_refused(self):
        root = self.plant_basic_repo()
        (root / "proofs" / "tla" / "README-RNSendBudget.md").write_text(
            "| `rocketnet_send_budget_init`, `sdk/rocketnet_send_budget.h`, `:340` |\n"
            "which stores STOPPING at :2091 (:1685).\n"
            "the server at [::1]:40000 and 127.0.0.1:40000 is an address, not a line.\n")
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
        for ref in [":340", ":2091", ":1685"]:
            self.assertIn(f"bare line reference {ref};", out)
        self.assertNotIn(":40000", out)

    def test_line_citation_pinned_to_a_commit_passes(self):
        root = self.plant_basic_repo()
        (root / "proofs" / "tla" / "README-RNSendBudget.md").write_text(
            "(`sdk/rocketnet_send_budget.h:2` at `4e1be4e8`), and\n"
            "(`:2362`, over `rocketnet_send_budget_init`,\n"
            "`sdk/rocketnet_send_budget.h:1998`) are cited at `14993f6`.\n")
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

    def test_citation_of_a_file_that_does_not_name_the_symbol_fails(self):
        root = self.plant_basic_repo()
        (root / "sdk" / "rocketnet_driver.h").write_text(
            "// sdk/rocketnet_send_budget.h (rocketnet_send_budget_gone)\n")
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
        self.assertIn("sdk/rocketnet_driver.h:1: citation sdk/rocketnet_send_budget.h "
                      "(rocketnet_send_budget_gone): sdk/rocketnet_send_budget.h "
                      "does not name rocketnet_send_budget_gone", out)

    def test_citation_of_a_missing_file_fails(self):
        root = self.plant_basic_repo()
        (root / "sdk" / "rocketnet_driver.h").write_text(
            "// rocketnet_send_budget_init (sdk/rocketnet_gone.h)\n")
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
        self.assertIn("citation sdk/rocketnet_gone.h (rocketnet_send_budget_init) "
                      "names no file", out)

    def test_note_function_only_called_not_defined_fails(self):
        # VERIFY-6 A4: the definition renamed away, a call site left behind.
        root = self.plant_basic_repo()
        (root / "sdk" / "rocketnet_send_budget.h").write_text(
            "// proofs/tla/RNSendBudget.tla\n"
            "void rocketnet_send_budget_init_renamed(void)\n{\n}\n"
            "void rocketnet_send_budget_reset(void)\n{\n"
            "    rocketnet_send_budget_init();\n}\n")
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
        self.assertIn("rocketnet_send_budget_init not defined in "
                      "sdk/rocketnet_send_budget.h nor elsewhere under sdk/", out)

    def test_empty_citation_scan_fails(self):
        root = Path(self.tmp.name)
        check_tla_coverage.ROOT = root
        failures = check_tla_coverage.check_citations()
        self.assertTrue(any("no file to scan for citations" in f for f in failures), failures)

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
        # the wrapper exists (a citation of a missing module fails on its own)
        (root / "proofs" / "tla" / "MCActMod.tla").write_text(
            "---- MODULE MCActMod ----\nEXTENDS ActMod\n====\n")
        code, out = run_check(root, ["--actions-strict"])
        self.assertNotEqual(code, 0, out)
        self.assertIn("ActMod EnvStep: sdk/act.c", out)
        self.assertIn("ACTIONS UNCITED 1", out)

    def test_model_citation_reads_a_wrapped_extends_list(self):
        # an EXTENDS list that wraps after a comma extends every module named
        # on its later lines too (Astra, gates review of 1b98c9bb6)
        code_text = ("// proofs/tla/ActMod.tla ActionA ActionB EnvStep\n"
                     "// proofs/tla/MCActMod.tla ExtraStep\n")
        root = self.plant_action_repo(code_text)
        (root / "proofs" / "tla" / "Extra.tla").write_text("---- MODULE Extra ----\nExtraStep == TRUE\n====\n")
        (root / "proofs" / "tla" / "MCActMod.tla").write_text(
            "---- MODULE MCActMod ----\nEXTENDS Naturals,\n        ActMod,\n        Extra\n====\n")
        code, out = run_check(root)
        self.assertEqual(code, 0, out)

    def test_model_citation_of_a_missing_module_fails(self):
        # the code cited proofs/tla/RNMessageOpen.tla, which was never written (FG-A4)
        code_text = ("// proofs/tla/ActMod.tla ActionA ActionB EnvStep\n"
                     "// one open per connection (proofs/tla/NeverWritten.tla)\n")
        root = self.plant_action_repo(code_text)
        code, out = run_check(root)
        self.assertEqual(code, 1, out)
        self.assertIn("sdk/act.c:2: cites proofs/tla/NeverWritten.tla, which is not a model in proofs/tla", out)

    def test_model_citation_of_an_undeclared_operator_fails(self):
        for cited in ("ActMod.tla ActionGone", "proofs/tla/ActMod.tla's ActionA, ActionGone",
                      "proofs/tla/ActMod.tla ActionA and ActionGone"):
            code_text = "// proofs/tla/ActMod.tla ActionA ActionB EnvStep\n// (" + cited + ")\n"
            root = self.plant_action_repo(code_text)
            code, out = run_check(root)
            self.assertEqual(code, 1, (cited, out))
            self.assertIn("cites ActMod.tla ActionGone, which ActMod does not declare", out)
            self.tearDown()
            self.setUp()

    def test_model_citation_of_operators_constants_and_prose_passes(self):
        module = ("---- MODULE ActMod ----\nCONSTANTS Cap,\n    BrokenFlag\nVARIABLES x\n"
                  "Init == x = 0\nActionA == x' = x + 1\nActionB == x' = x\nEnvStep == x' = 0\n"
                  "Next == ActionA \\/ ActionB \\/ EnvStep\n====\n")
        (Path(self.tmp.name) / "proofs" / "tla").mkdir(parents=True)
        (Path(self.tmp.name) / "proofs" / "tla" / "Other.tla").write_text(
            "---- MODULE Other ----\nOtherStep == TRUE\n====\n")
        code_text = ("// proofs/tla/ActMod.tla ActionA ActionB EnvStep\n"
                     "// proofs/tla/ActMod.tla BrokenFlag TRUE, Cap 3\n"
                     "// proofs/tla/ActMod.tla The counter moves once\n"
                     "// proofs/tla/ActMod.tla ActionA, Other.tla OtherStep\n"
                     "// (proofs/tla/ActMod.tla Init, MCActMod.cfg)\n")
        root = Path(self.tmp.name)
        (root / "sdk").mkdir()
        (root / "proofs" / "tla" / "ActMod.tla").write_text(module)
        (root / "sdk" / "act.c").write_text(code_text)
        write_row(root, {"machine": "rn-act", "code": "sdk/act.c", "module": "ActMod", "status": "current",
                         "owed_by": "-", "note": self.ACTION_NOTE})
        code, out = run_check(root)
        self.assertNotIn("cites", out)

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
