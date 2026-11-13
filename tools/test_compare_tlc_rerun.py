#!/usr/bin/env python3
"""Controls for the TLC re-run comparer: every difference from the record is red by case name.

No TLC runs here: each case writes the evidence the nightly adapter would.
"""

import io
import json
import tempfile
import unittest
from pathlib import Path

from compare_tlc_rerun import compare, main, read_plan

FINGERPRINT = "f" * 64
JAR = "936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88"


def record(config, expected="invariant", prop="LiveSpansDisjoint", exit="12", workers="1",
           generated="3544", distinct="1544", result="PASS"):
    return dict(config=config, input_sha256=FINGERPRINT, jar_sha256=JAR, workers=workers,
                generated=generated, distinct=distinct, exit=exit, result=result,
                expected=expected, property=prop, seconds="1.5")


def fresh(config, violated=(("Invariant", "LiveSpansDisjoint"),), exit=12, workers=1,
          generated="3544", distinct="1544", result="PASS"):
    return {"case": {"Config": config}, "engine_input_sha256": FINGERPRINT, "jar_sha256": JAR,
            "workers": workers, "generated": generated, "distinct": distinct, "exit": exit,
            "result": result, "violated": [{"kind": k, "name": n} for k, n in violated], "seconds": 1.6}


class ComparerTests(unittest.TestCase):
    def check(self, rec, got, config="MCWitness.cfg", budget="0", nightly=None, budgets=None):
        with tempfile.TemporaryDirectory() as tmp:
            evidence = Path(tmp)
            if got is not None:
                run = evidence / "run-ring-1"
                run.mkdir()
                (run / "result.json").write_text(json.dumps(got))
            out = io.StringIO()
            failed = compare([("ring", 5, 1, config, budget)], {config: rec} if rec else {}, evidence, out, nightly,
                             budgets)
            return failed, out.getvalue()

    def test_declared_budget_is_held_to_its_record(self):
        # A PASS record over the budget margin with its BUDGETS.tsv row: re-run under budget_s, compared as recorded.
        rec = record("MCWitness.cfg", "pass", "-", "0", "2", "5000", "4000")
        budgets = {"MCWitness.cfg": {"config": "MCWitness.cfg", "budget_s": "220"}}
        done = fresh("MCWitness.cfg", (), 0, 2, "5000", "4000")
        failed, text = self.check(rec, done, budget="220", budgets=budgets)
        self.assertEqual(failed, [], text)
        # Plants: a run its declared budget stopped; a budget with no row; a count off by one.
        for got, rows, field in (
                (dict(fresh("MCWitness.cfg", (), 124, 2, "500", "400", "FAIL"), interrupted=True), budgets, "budget"),
                (done, {}, "budget-declaration"),
                (fresh("MCWitness.cfg", (), 0, 2, "5000", "4001"), budgets, "distinct")):
            with self.subTest(field=field):
                failed, text = self.check(rec, got, budget="220", budgets=rows)
                self.assertEqual(failed, ["MCWitness.cfg"], text)
                self.assertIn(f"TLC-RERUN MISMATCH MCWitness.cfg {field}:", text)

    def assertRed(self, rec, got, field):
        failed, text = self.check(rec, got)
        self.assertEqual(failed, ["MCWitness.cfg"], text)
        self.assertIn(f"TLC-RERUN MISMATCH MCWitness.cfg {field}:", text)

    def test_nightly_debt_is_held_to_its_completed_counts(self):
        # The bounded record of a debt case: FAIL/124 with the counts at its 110 s cap.
        rec = record("MCWitness.cfg", "pass", "-", "124", "2", "1000", "900", "FAIL")
        row = {"config": "MCWitness.cfg", "generated": "2837306", "distinct": "474351"}
        done = fresh("MCWitness.cfg", (), 0, 2, "2837306", "474351")
        failed, text = self.check(rec, done, budget="900", nightly={"MCWitness.cfg": row})
        self.assertEqual(failed, [], text)
        # Plants: a count off by one, a run its budget stopped, a budget with no NIGHTLY-DEBT.tsv row,
        # and the same evidence held to the bounded record (budget 0) instead.
        for got, budget, nightly, field in (
                (fresh("MCWitness.cfg", (), 0, 2, "2837306", "474352"), "900", {"MCWitness.cfg": row}, "distinct"),
                (dict(fresh("MCWitness.cfg", (), 124, 2, "500", "400", "FAIL"), interrupted=True), "900",
                 {"MCWitness.cfg": row}, "budget"),
                (done, "900", {}, "budget-declaration"),
                (done, "0", {"MCWitness.cfg": row}, "result")):
            with self.subTest(field=field):
                failed, text = self.check(rec, got, budget=budget, nightly=nightly)
                self.assertEqual(failed, ["MCWitness.cfg"], text)
                self.assertIn(f"TLC-RERUN MISMATCH MCWitness.cfg {field}:", text)

    def test_identical_witness_is_green(self):
        failed, text = self.check(record("MCWitness.cfg"), fresh("MCWitness.cfg"))
        self.assertEqual(failed, [], text)
        self.assertIn("TLC-RERUN MATCH MCWitness.cfg result=PASS exit=12 violated=Invariant:LiveSpansDisjoint "
                      "generated=3544 distinct=1544", text)

    def test_identical_pass_case_is_green(self):
        rec = record("MCWitness.cfg", "pass", "-", "0", "2", "16079032", "1391055")
        got = fresh("MCWitness.cfg", (), 0, 2, "16079032", "1391055")
        self.assertEqual(self.check(rec, got)[0], [])

    def test_witness_that_passes_is_red(self):
        # The A1b plant: FloorAtUPrime = TRUE, the witness violates nothing, TLC
        # completes; the adapter reports FAIL, exit 0, no violation.
        got = fresh("MCWitness.cfg", (), 0, 1, "222691", "79377", "FAIL")
        failed, text = self.check(record("MCWitness.cfg"), got)
        self.assertEqual(failed, ["MCWitness.cfg"])
        for field in ("result", "exit", "violated", "generated", "distinct"):
            self.assertIn(f"TLC-RERUN MISMATCH MCWitness.cfg {field}:", text)

    def test_mismatched_distinct_count_is_red(self):
        self.assertRed(record("MCWitness.cfg", distinct="1545"), fresh("MCWitness.cfg"), "distinct")

    def test_two_worker_pass_case_distinct_drift_is_red(self):
        # The v1.1.9 finding: MCRNChannelNotReceived.cfg, a pass case on two
        # workers, re-ran to 8634 distinct against its record's 8630 under an
        # unsound VIEW. Distinct is exact for every config and mode: a pass
        # case, a witness, a declared-budget case, whatever its name.
        for config, workers in (("MCRNChannelNotReceived.cfg", "2"), ("MCRNChannelSafety.cfg", "2"),
                                ("MCWitness.cfg", "1")):
            rec = record(config, "pass", "-", "0", workers, "21199", "8630")
            for got_distinct in ("8634", "8626", "8631"):
                with self.subTest(config=config, distinct=got_distinct):
                    got = fresh(config, (), 0, int(workers), "21199", got_distinct)
                    failed, text = self.check(rec, got, config=config)
                    self.assertEqual(failed, [config], text)
                    self.assertIn(f"TLC-RERUN MISMATCH {config} distinct: record=8630 fresh={got_distinct}", text)
            self.assertEqual(self.check(rec, fresh(config, (), 0, int(workers), "21199", "8630"), config=config)[0],
                             [])

    def test_mismatched_generated_count_is_red(self):
        self.assertRed(record("MCWitness.cfg"), fresh("MCWitness.cfg", generated="3543"), "generated")

    def test_mismatched_invariant_is_red(self):
        got = fresh("MCWitness.cfg", (("Invariant", "OtherInvariant"),))
        self.assertRed(record("MCWitness.cfg"), got, "violated")

    def test_violation_of_the_wrong_kind_is_red(self):
        got = fresh("MCWitness.cfg", (("Action property", "LiveSpansDisjoint"),))
        self.assertRed(record("MCWitness.cfg"), got, "violated")

    def test_pass_case_with_a_violation_is_red(self):
        rec = record("MCWitness.cfg", "pass", "-", "0", "2")
        got = fresh("MCWitness.cfg", (("Invariant", "Safe"),), 0, 2)
        self.assertRed(rec, got, "violated")

    def test_unnamed_temporal_violation_matches_a_temporal_record(self):
        rec = record("MCWitness.cfg", "temporal", "EventuallyDone", "13")
        self.assertEqual(self.check(rec, fresh("MCWitness.cfg", (("Temporal property", ""),), 13))[0], [])
        self.assertRed(rec, fresh("MCWitness.cfg", (("Temporal property", "Other"),), 13), "violated")

    def test_missing_violated_list_is_red(self):
        got = fresh("MCWitness.cfg")
        del got["violated"]
        self.assertRed(record("MCWitness.cfg"), got, "violated")

    def test_fingerprint_jar_workers_and_exit_differences_are_red(self):
        for field, change in (("fingerprint", {"engine_input_sha256": "0" * 64}), ("jar", {"jar_sha256": "1" * 64}),
                              ("workers", {"workers": 2}), ("exit", {"exit": 13})):
            with self.subTest(field=field):
                got = fresh("MCWitness.cfg")
                got.update(change)
                self.assertRed(record("MCWitness.cfg"), got, field)

    def test_missing_evidence_or_record_or_wrong_case_is_red(self):
        self.assertRed(record("MCWitness.cfg"), None, "evidence")
        self.assertRed(None, fresh("MCWitness.cfg"), "record")
        self.assertRed(record("MCWitness.cfg"), fresh("MCOther.cfg"), "case")

    def test_empty_or_malformed_plan_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            plan = Path(tmp) / "plan"
            plan.write_text("")
            with self.assertRaisesRegex(ValueError, "zero cases"):
                read_plan(plan)
            plan.write_text("ring 5 1 MCWitness.cfg\n")
            with self.assertRaisesRegex(ValueError, "want 'group cardinality shard config budget'"):
                read_plan(plan)

    def test_main_exit_status_and_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "plan").write_text("ring 5 1 MCWitness.cfg 0\n")
            rec = record("MCWitness.cfg")
            (root / "RUNS.tsv").write_text("\t".join(rec) + "\n" + "\t".join(rec.values()) + "\n")
            run = root / "run-ring-1"
            run.mkdir()
            (run / "result.json").write_text(json.dumps(fresh("MCWitness.cfg")))
            args = ["--runs", str(root / "RUNS.tsv"), "--plan", str(root / "plan"), "--evidence", str(root)]
            self.assertEqual(main(args), 0)
            (run / "result.json").write_text(json.dumps(fresh("MCWitness.cfg", distinct="9")))
            self.assertEqual(main(args), 1)


if __name__ == "__main__":
    unittest.main()
