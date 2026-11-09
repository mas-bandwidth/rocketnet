#!/usr/bin/env python3
"""Unit tests for tools/check_tla_records.py.

Run: python3 tools/test_check_tla_records.py -v
"""

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

    def run_fixture(self, cases, runs):
        with tempfile.TemporaryDirectory() as tmp:
            cases_path = Path(tmp) / "CASES.tsv"
            runs_path = Path(tmp) / "RUNS.tsv"
            write_tsv(cases_path, check_tla_records.CASES_COLUMNS, cases)
            write_tsv(runs_path, check_tla_records.RUNS_COLUMNS, runs)
            out = io.StringIO()
            code = check_tla_records.check(cases_path, runs_path, out, forbid_stale=False)
            return code, out.getvalue()

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
            code = check_tla_records.check(tla / "CASES.tsv", tla / "RUNS.tsv", out)
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


if __name__ == "__main__":
    unittest.main()
