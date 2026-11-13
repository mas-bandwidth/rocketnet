#!/usr/bin/env python3
"""Unit tests for tools/check_tla_records.py.

Run: python3 tools/test_check_tla_records.py -v
"""

import csv
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_tla_records  # noqa: E402


def case_row(
    config="MCExample.cfg",
    module="MCExample.tla",
    expected="pass",
    prop="-",
    deadlock="check",
    group="example",
    gate="required",
    debt="-",
):
    return {
        "config": config,
        "module": module,
        "expected": expected,
        "property": prop,
        "deadlock": deadlock,
        "group": group,
        "gate": gate,
        "debt": debt,
    }


def run_row(
    config="MCExample.cfg",
    module="MCExample.tla",
    expected="pass",
    prop="-",
    exit_code="0",
    result="PASS",
    generated="100",
    distinct="25",
    jar_sha256=check_tla_records.PINNED_JAR_SHA256,
    mode="bounded",
):
    return {
        "config": config,
        "module": module,
        "input_sha256": "a" * 64,
        "input_files": "2",
        "jar_sha256": jar_sha256,
        "java_version": "21.0.12.1",
        "host": "linux-amd64",
        "cpus": "64",
        "started_utc": "2026-11-01T00:00:00+00:00",
        "workers": "2",
        "generated": generated,
        "distinct": distinct,
        "seconds": "1.250",
        "exit": exit_code,
        "result": result,
        "expected": expected,
        "property": prop,
        "budget": "110",
        "mode": mode,
    }


def write_tsv(path, columns, rows):
    lines = ["\t".join(columns)]
    for row in rows:
        lines.append("\t".join(str(row[col]) for col in columns))
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


class CheckTlaRecordsTest(unittest.TestCase):

    def run_fixture(self, cases, runs, declared_debt=None):
        """Check a fixture; declared_debt defaults to the fixture's own debt rows."""
        if declared_debt is None:
            declared_debt = frozenset(c["config"] for c in cases if c["debt"] not in ("", "-"))
        with tempfile.TemporaryDirectory() as tmp:
            cases_path = Path(tmp) / "CASES.tsv"
            runs_path = Path(tmp) / "RUNS.tsv"
            write_tsv(cases_path, check_tla_records.CASES_COLUMNS, cases)
            write_tsv(runs_path, check_tla_records.RUNS_COLUMNS, runs)
            out = io.StringIO()
            code = check_tla_records.check(
                cases_path, runs_path, out, forbid_stale=False, declared_debt=declared_debt)
            return code, out.getvalue()

    def test_debt_outside_the_pinned_set_rejected(self):
        # VERIFY-6 A13: a healthy case moved to debt in CASES.tsv alone, and
        # measured honestly under a 1 s budget.
        cases = [
            case_row("MCPause.cfg", "MCPause.tla", "pass", "-", gate="bench", debt="too-slow-today"),
        ]
        runs = [run_row("MCPause.cfg", "MCPause.tla", "pass", "-", "124", "FAIL", generated="-", distinct="-")]
        code, text = self.run_fixture(cases, runs, declared_debt=frozenset())
        self.assertEqual(code, 1)
        self.assertIn("MCPause.cfg: declared timeout debt in CASES.tsv but not in DECLARED_DEBT", text)

    def test_pinned_debt_no_longer_declared_rejected(self):
        cases = [case_row("MCPass.cfg", "MCPass.tla", "pass", "-")]
        runs = [run_row("MCPass.cfg", "MCPass.tla", "pass", "-", "0", "PASS")]
        code, text = self.run_fixture(cases, runs, declared_debt=frozenset({"MCPass.cfg", "MCGone.cfg"}))
        self.assertEqual(code, 1)
        self.assertIn("MCPass.cfg: in DECLARED_DEBT but not declared timeout debt in CASES.tsv", text)
        self.assertIn("MCGone.cfg: in DECLARED_DEBT but not declared timeout debt in CASES.tsv", text)

    def test_record_under_another_budget_rejected(self):
        cases = [case_row("MCPass.cfg", "MCPass.tla", "pass", "-")]
        row = run_row("MCPass.cfg", "MCPass.tla", "pass", "-", "0", "PASS")
        row["budget"] = "1"
        code, text = self.run_fixture(cases, [row])
        self.assertEqual(code, 1)
        self.assertIn("MCPass.cfg: measured under budget '1', not the pinned 110 s", text)

    def test_committed_debt_is_the_pinned_set(self):
        with check_tla_records.DEFAULT_CASES.open(encoding="utf-8", newline="") as handle:
            declared = {
                row["config"]
                for row in csv.DictReader(handle, delimiter="\t")
                if row["debt"] not in ("", "-")
            }
        self.assertEqual(declared, set(check_tla_records.DECLARED_DEBT))

    def stale_fixture(self, edit):
        """A current record of MCEx.cfg, then edit(tla_dir), then the default check."""
        with tempfile.TemporaryDirectory() as tmp:
            tla = Path(tmp)
            (tla / "MCEx.cfg").write_text("SPECIFICATION Spec\n", encoding="utf-8")
            (tla / "MCEx.tla").write_text(
                "---- MODULE MCEx ----\nEXTENDS Base, Naturals\n\\* INSTANCE NotRead\nSpec == Init\n====\n",
                encoding="utf-8",
            )
            (tla / "Base.tla").write_text("---- MODULE Base ----\nInit == TRUE\n====\n", encoding="utf-8")
            write_tsv(tla / "CASES.tsv", check_tla_records.CASES_COLUMNS, [case_row("MCEx.cfg", "MCEx.tla")])
            digest, files = check_tla_records.fingerprint(tla, "MCEx.cfg")
            row = run_row("MCEx.cfg", "MCEx.tla")
            row["input_sha256"], row["input_files"] = digest, str(files)
            write_tsv(tla / "RUNS.tsv", check_tla_records.RUNS_COLUMNS, [row])
            edit(tla)
            out = io.StringIO()
            code = check_tla_records.check(tla / "CASES.tsv", tla / "RUNS.tsv", out, declared_debt=frozenset())
            return code, out.getvalue(), files

    def test_current_record_passes_stale_check(self):
        code, text, files = self.stale_fixture(lambda tla: None)
        self.assertEqual(code, 0, text)
        # MCEx.cfg, MCEx.tla, Base.tla, the CASES.tsv row and the four runner files
        self.assertEqual(files, 8)

    def test_edited_module_makes_record_stale(self):
        def edit(tla):
            (tla / "Base.tla").write_text("---- MODULE Base ----\nInit == FALSE\n====\n", encoding="utf-8")
        code, text, _ = self.stale_fixture(edit)
        self.assertEqual(code, 1)
        self.assertIn("MCEx.cfg: stale record", text)

    def test_edited_config_makes_record_stale(self):
        def edit(tla):
            (tla / "MCEx.cfg").write_text("SPECIFICATION Spec\nCHECK_DEADLOCK FALSE\n", encoding="utf-8")
        code, text, _ = self.stale_fixture(edit)
        self.assertEqual(code, 1)
        self.assertIn("MCEx.cfg: stale record", text)

    def delivery_fixture(self, witness_property="Eventually", witness=True):
        """A property config in a module with an anonymous delivery implication."""
        with tempfile.TemporaryDirectory() as tmp:
            tla = Path(tmp)
            (tla / "MCDelivery.tla").write_text(
                "---- MODULE MCDelivery ----\n"
                "VARIABLE x\nvars == <<x>>\n"
                "Send == x' = x\nRecv == x' = x\n"
                "Fairness == ([]<> <<Send>>_vars) => ([]<> <<Recv>>_vars)\n"
                "Spec == TRUE /\\ [][Send \\/ Recv]_vars /\\ Fairness\n"
                "SpecSafety == TRUE /\\ [][Send \\/ Recv]_vars\n"
                "Eventually == <>TRUE\n"
                "====\n",
                encoding="utf-8",
            )
            (tla / "MCPass.cfg").write_text(
                "SPECIFICATION Spec\nPROPERTIES\n    Eventually\n", encoding="utf-8")
            cases = [case_row("MCPass.cfg", "MCDelivery.tla")]
            runs = [run_row("MCPass.cfg", "MCDelivery.tla")]
            if witness:
                (tla / "MCWitness.cfg").write_text(
                    "SPECIFICATION SpecSafety\nPROPERTIES\n    Eventually\n",
                    encoding="utf-8")
                cases.append(case_row(
                    "MCWitness.cfg", "MCDelivery.tla", "temporal", witness_property))
                runs.append(run_row(
                    "MCWitness.cfg", "MCDelivery.tla", "temporal", witness_property,
                    "13", "PASS"))
            write_tsv(tla / "CASES.tsv", check_tla_records.CASES_COLUMNS, cases)
            write_tsv(tla / "RUNS.tsv", check_tla_records.RUNS_COLUMNS, runs)
            out = io.StringIO()
            code = check_tla_records.check(
                tla / "CASES.tsv", tla / "RUNS.tsv", out, forbid_stale=False, declared_debt=frozenset())
            return code, out.getvalue()

    def test_property_without_reversed_liveness_witness_rejected(self):
        code, text = self.delivery_fixture(witness=False)
        self.assertEqual(code, 1)
        self.assertIn("MCPass.cfg: property Eventually has no reversed liveness witness", text)

    def test_property_witness_for_another_property_rejected(self):
        code, text = self.delivery_fixture(witness_property="SomethingElse")
        self.assertEqual(code, 1)
        self.assertIn("MCPass.cfg: property Eventually has no reversed liveness witness", text)

    def test_property_with_reversed_liveness_witness_passes(self):
        code, text = self.delivery_fixture()
        self.assertEqual(code, 0, text)

    def test_property_without_delivery_implication_needs_no_witness(self):
        with tempfile.TemporaryDirectory() as tmp:
            tla = Path(tmp)
            (tla / "MCPlain.tla").write_text(
                "---- MODULE MCPlain ----\nEventually == <>TRUE\n====\n",
                encoding="utf-8",
            )
            (tla / "MCPlain.cfg").write_text(
                "SPECIFICATION Spec\nPROPERTIES\n    Eventually\n", encoding="utf-8")
            write_tsv(tla / "CASES.tsv", check_tla_records.CASES_COLUMNS,
                      [case_row("MCPlain.cfg", "MCPlain.tla")])
            write_tsv(tla / "RUNS.tsv", check_tla_records.RUNS_COLUMNS,
                      [run_row("MCPlain.cfg", "MCPlain.tla")])
            out = io.StringIO()
            code = check_tla_records.check(
                tla / "CASES.tsv", tla / "RUNS.tsv", out, forbid_stale=False, declared_debt=frozenset())
            self.assertEqual(code, 0, out.getvalue())

    def test_committed_ledgers_pass(self):
        out = io.StringIO()
        code = check_tla_records.check(out=out)
        self.assertEqual(code, 0, out.getvalue())
        self.assertIn("Required outcomes match", out.getvalue())

    def test_synthetic_valid_plan_passes(self):
        cases = [
            case_row("MCPass.cfg", "MCPass.tla", "pass", "-"),
            case_row("MCInv.cfg", "MCInv.tla", "invariant", "NoLeak"),
            case_row("MCAct.cfg", "MCAct.tla", "action", "StepOrdered"),
            case_row("MCTemp.cfg", "MCTemp.tla", "temporal", "EventuallyDone"),
            case_row("MCDebt.cfg", "MCDebt.tla", "pass", "-", gate="bench", debt="state space exceeds 110s"),
        ]
        runs = [
            run_row("MCPass.cfg", "MCPass.tla", "pass", "-", "0", "PASS"),
            run_row("MCInv.cfg", "MCInv.tla", "invariant", "NoLeak", "12", "PASS"),
            run_row("MCAct.cfg", "MCAct.tla", "action", "StepOrdered", "13", "PASS"),
            run_row("MCTemp.cfg", "MCTemp.tla", "temporal", "EventuallyDone", "13", "PASS"),
            run_row("MCDebt.cfg", "MCDebt.tla", "pass", "-", "124", "FAIL", generated="-", distinct="-"),
        ]
        code, text = self.run_fixture(cases, runs)
        self.assertEqual(code, 0, text)
        self.assertIn("Required outcomes match (5 rows)", text)

    def test_zero_cases_or_rows_rejected(self):
        # zero inputs is an error: header-only ledgers never pass
        for cases, runs in (([], []), ([case_row()], []), ([], [run_row()])):
            code, text = self.run_fixture(cases, runs)
            self.assertEqual(code, 1, text)
            self.assertIn("nothing was checked", text)

    def test_pass_row_with_wrong_exit_for_invariant_rejected(self):
        cases = [case_row("MCInv.cfg", "MCInv.tla", "invariant", "NoLeak")]
        runs = [run_row("MCInv.cfg", "MCInv.tla", "invariant", "NoLeak", "0", "PASS")]
        code, text = self.run_fixture(cases, runs)
        self.assertEqual(code, 1)
        self.assertIn("MCInv.cfg: PASS row for expected 'invariant' requires exit '12', got '0'", text)

    def test_pass_row_with_wrong_exit_for_pass_rejected(self):
        cases = [case_row("MCPass.cfg", "MCPass.tla", "pass", "-")]
        runs = [run_row("MCPass.cfg", "MCPass.tla", "pass", "-", "12", "PASS")]
        code, text = self.run_fixture(cases, runs)
        self.assertEqual(code, 1)
        self.assertIn("MCPass.cfg: PASS row for expected 'pass' requires exit '0', got '12'", text)

    def test_pass_row_with_wrong_exit_for_action_and_temporal_rejected(self):
        cases = [
            case_row("MCAct.cfg", "MCAct.tla", "action", "StepOrdered"),
            case_row("MCTemp.cfg", "MCTemp.tla", "temporal", "EventuallyDone"),
        ]
        runs = [
            run_row("MCAct.cfg", "MCAct.tla", "action", "StepOrdered", "12", "PASS"),
            run_row("MCTemp.cfg", "MCTemp.tla", "temporal", "EventuallyDone", "0", "PASS"),
        ]
        code, text = self.run_fixture(cases, runs)
        self.assertEqual(code, 1)
        self.assertIn("MCAct.cfg: PASS row for expected 'action' requires exit '13', got '12'", text)
        self.assertIn("MCTemp.cfg: PASS row for expected 'temporal' requires exit '13', got '0'", text)

    def test_expected_or_property_or_module_mismatch_rejected(self):
        cases = [case_row("MCInv.cfg", "MCInv.tla", "invariant", "NoLeak")]
        runs = [run_row("MCInv.cfg", "Other.tla", "action", "WrongProp", "12", "PASS")]
        code, text = self.run_fixture(cases, runs)
        self.assertEqual(code, 1)
        self.assertIn("MCInv.cfg: module mismatch", text)
        self.assertIn("MCInv.cfg: expected mismatch", text)
        self.assertIn("MCInv.cfg: property mismatch", text)

    def test_invalid_case_declaration_rejected(self):
        cases = [
            case_row("MCPassBad.cfg", "MCPass.tla", "pass", "ShouldBeDash"),
            case_row("MCInvBad.cfg", "MCInv.tla", "invariant", "-"),
        ]
        runs = [
            run_row("MCPassBad.cfg", "MCPass.tla", "pass", "ShouldBeDash", "0", "PASS"),
            run_row("MCInvBad.cfg", "MCInv.tla", "invariant", "-", "12", "PASS"),
        ]
        code, text = self.run_fixture(cases, runs)
        self.assertEqual(code, 1)
        self.assertIn("MCPassBad.cfg: pass case must have property '-'", text)
        self.assertIn("MCInvBad.cfg: invariant case must name a property", text)

    def test_missing_extra_or_duplicate_config_rejected(self):
        cases = [
            case_row("MCA.cfg", "MCA.tla", "pass", "-"),
            case_row("MCB.cfg", "MCB.tla", "pass", "-"),
        ]
        runs = [
            run_row("MCA.cfg", "MCA.tla", "pass", "-", "0", "PASS"),
            run_row("MCA.cfg", "MCA.tla", "pass", "-", "0", "PASS"),
            run_row("MCExtra.cfg", "MCExtra.tla", "pass", "-", "0", "PASS"),
        ]
        code, text = self.run_fixture(cases, runs)
        self.assertEqual(code, 1)
        self.assertIn("MCA.cfg: duplicate config in RUNS.tsv", text)
        self.assertIn("MCExtra.cfg: present in RUNS.tsv but missing from CASES.tsv", text)
        self.assertIn("MCB.cfg: present in CASES.tsv but missing from RUNS.tsv", text)

    def test_unwaived_failure_or_wrong_timeout_exit_rejected(self):
        cases = [
            case_row("MCReqFail.cfg", "MC.tla", "pass", "-", gate="required", debt="-"),
            case_row("MCBenchNoDebt.cfg", "MC.tla", "pass", "-", gate="bench", debt="-"),
            case_row("MCBenchWrongExit.cfg", "MC.tla", "pass", "-", gate="bench", debt="slow"),
        ]
        runs = [
            run_row("MCReqFail.cfg", "MC.tla", "pass", "-", "124", "FAIL", generated="-", distinct="-"),
            run_row("MCBenchNoDebt.cfg", "MC.tla", "pass", "-", "124", "FAIL", generated="-", distinct="-"),
            run_row("MCBenchWrongExit.cfg", "MC.tla", "pass", "-", "12", "FAIL", generated="-", distinct="-"),
        ]
        code, text = self.run_fixture(cases, runs)
        self.assertEqual(code, 1)
        self.assertIn("MCReqFail.cfg: failure without declared timeout debt", text)
        self.assertIn("MCBenchNoDebt.cfg: failure without declared timeout debt", text)
        self.assertIn("MCBenchWrongExit.cfg: failure without declared timeout debt", text)

    def test_wrong_mode_or_jar_or_missing_state_counts_rejected(self):
        cases = [case_row("MCPass.cfg", "MCPass.tla", "pass", "-")]
        runs = [
            run_row(
                "MCPass.cfg",
                "MCPass.tla",
                "pass",
                "-",
                "0",
                "PASS",
                generated="-",
                distinct="0",
                jar_sha256="b" * 64,
                mode="simulation",
            )
        ]
        code, text = self.run_fixture(cases, runs)
        self.assertEqual(code, 1)
        self.assertIn("MCPass.cfg: not a bounded measurement", text)
        self.assertIn("MCPass.cfg: different JAR", text)
        self.assertIn("MCPass.cfg: PASS row requires positive generated/distinct state counts", text)


class NightlyDebtTest(unittest.TestCase):
    """NIGHTLY-DEBT.tsv beside the committed ledgers: green as committed, red for each plant."""

    def check_with(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            tla = Path(tmp)
            committed = check_tla_records.DEFAULT_CASES.parent
            for name in ("CASES.tsv", "RUNS.tsv", "BUDGETS.tsv"):
                if (committed / name).exists():
                    (tla / name).write_bytes((committed / name).read_bytes())
            if text is not None:
                (tla / "NIGHTLY-DEBT.tsv").write_text(text, encoding="utf-8")
            out = io.StringIO()
            code = check_tla_records.check(tla / "CASES.tsv", tla / "RUNS.tsv", out, forbid_stale=False)
            return code, out.getvalue()

    def test_committed_nightly_debt_and_plants(self):
        committed = (check_tla_records.DEFAULT_CASES.parent / "NIGHTLY-DEBT.tsv").read_text(encoding="utf-8")
        code, text = self.check_with(committed)
        self.assertEqual(code, 0, text)
        code, text = self.check_with(None)
        self.assertEqual(code, 0, text)
        header = committed.splitlines()[0] + "\n"
        for row, want in (
                ("MCRNRing.cfg\t900\t12\tbench\t222691\t79377\n", "MCRNRing.cfg: not declared timeout debt"),
                ("MCReplayWindowMid.cfg\t600\t335.6\tbench\t69206063\t3145729\n",
                 "MCReplayWindowMid.cfg: budget_s 600 is under twice measured_s 335.6"),
                ("MCReplayWindowMid.cfg\t900\t335.6\tbench\t0\t3145729\n", "positive counts of a complete run")):
            with self.subTest(want=want):
                code, text = self.check_with(header + row)
                self.assertEqual(code, 1, text)
                self.assertIn(want, text)


class BudgetMarginTest(unittest.TestCase):
    """The budget margin over the committed ledgers: a PASS record over 60% of
    its budget is refused unless BUDGETS.tsv declares a budget of at least
    twice its seconds. Green as committed, red for each plant."""

    def check_with(self, budgets, seconds=None):
        """The committed ledgers, BUDGETS.tsv replaced by `budgets` (None: no
        file), and RUNS.tsv's seconds replaced per config by `seconds`."""
        with tempfile.TemporaryDirectory() as tmp:
            tla = Path(tmp)
            committed = check_tla_records.DEFAULT_CASES.parent
            for name in ("CASES.tsv", "NIGHTLY-DEBT.tsv"):
                if (committed / name).exists():
                    (tla / name).write_bytes((committed / name).read_bytes())
            lines = (committed / "RUNS.tsv").read_text(encoding="utf-8").splitlines(keepends=True)
            column = lines[0].rstrip("\n").split("\t").index("seconds")
            for number, line in enumerate(lines[1:], 1):
                fields = line.rstrip("\n").split("\t")
                if fields[0] in (seconds or {}):
                    fields[column] = seconds[fields[0]]
                    lines[number] = "\t".join(fields) + "\n"
            (tla / "RUNS.tsv").write_text("".join(lines), encoding="utf-8")
            if budgets is not None:
                (tla / "BUDGETS.tsv").write_text(budgets, encoding="utf-8")
            out = io.StringIO()
            code = check_tla_records.check(tla / "CASES.tsv", tla / "RUNS.tsv", out, forbid_stale=False)
            return code, out.getvalue()

    def test_committed_budgets_and_plants(self):
        path = check_tla_records.DEFAULT_CASES.parent / "BUDGETS.tsv"
        committed = path.read_text(encoding="utf-8") if path.exists() else None
        code, text = self.check_with(committed)
        self.assertEqual(code, 0, text)
        header = "config\tbudget_s\tmeasured_s\tslowest_s\tslowest_host\n"
        # MCRNRing.cfg is a committed PASS well inside the margin; plant it at 66.5 s of its 110 s.
        over = {"MCRNRing.cfg": "66.5"}
        code, text = self.check_with(committed, over)
        self.assertEqual(code, 1, text)
        self.assertIn("MCRNRing.cfg: PASS in 66.5 s, over 60% of its 110 s budget (66 s)", text)
        rows = "".join(line + "\n" for line in (committed or header).splitlines()[1:])
        code, text = self.check_with(header + rows + "MCRNRing.cfg\t133\t66.5\t1\thetzner2\n", over)
        self.assertEqual(code, 0, text)
        self.assertIn("TLC-MARGIN MCRNRing.cfg PASS in 66.5 s of its 110 s budget (over 60%): "
                      "declared budget 133 s in BUDGETS.tsv", text)
        for row, want in (
                ("MCRNRing.cfg\t132\t66.5\t1\thetzner2\n", "MCRNRing.cfg: budget_s 132 is under twice measured_s 66.5"),
                ("MCRNRing.cfg\t200\t60\t1\thetzner2\n", "MCRNRing.cfg: measured_s 60 is not its RUNS.tsv record's"),
                ("MCRNRing.cfg\t200\t66.5\t100.5\thetzner2\n", "MCRNRing.cfg: budget_s 200 is under twice slowest_s 100.5"),
                ("MCReplayWindowMid.cfg\t900\t335.6\thetzner1\n", "MCReplayWindowMid.cfg: has no PASS record")):
            with self.subTest(want=want):
                code, text = self.check_with(header + rows + row, over)
                self.assertEqual(code, 1, text)
                self.assertIn(want, text)
        # A declaration the record no longer needs is refused: the list stays the flagged cases.
        code, text = self.check_with(header + rows + "MCRNRing.cfg\t200\t66.5\t1\thetzner2\n", {"MCRNRing.cfg": "66.5"})
        self.assertEqual(code, 0, text)
        code, text = self.check_with(header + rows + "MCRNRing.cfg\t200\t12\t1\thetzner2\n", {"MCRNRing.cfg": "12"})
        self.assertEqual(code, 1, text)
        self.assertIn("MCRNRing.cfg: its record (12 s of 110 s) is within 60% of its budget", text)


if __name__ == "__main__":
    unittest.main()
