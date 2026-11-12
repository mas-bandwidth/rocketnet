#!/usr/bin/env python3
"""Planner controls: selection must not change pinned tlacheck shard identities."""

import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from plan_tlc_nightly import (BUDGETS_COLUMNS, NIGHTLY_DEBT_COLUMNS, SLICES, choose, day_slice, declared_budgets,
                              declared_debt, margin, nightly_debt, plan, rerun, slice_key)

TOOL = Path(__file__).resolve().parent / "plan_tlc_nightly.py"


def case(config, group="alpha", gate="required", debt="-"):
    return dict(config=config, group=group, gate=gate, debt=debt)


def run(config, result="PASS", exit="0"):
    return dict(config=config, result=result, exit=exit)


class PlannerTests(unittest.TestCase):
    def test_empty_plan(self):
        self.assertEqual(plan([], [], []), [])

    def test_current_passes_create_no_shards(self):
        cases = [case("required"), case("bench", gate="bench", debt="over-budget")]
        self.assertEqual(plan(cases, [run("required"), run("bench")], []), [])

    def test_stale_selects_all_cases_in_declaration_order(self):
        cases = [case("z"), case("unrelated", "beta"), case("a")]
        self.assertEqual(plan(cases, [], ["alpha"]), [
            ("alpha", 2, 0, "z", "stale"), ("alpha", 2, 1, "a", "stale")])

    def test_debt_selects_only_its_original_shard(self):
        cases = [case("z"), case("debt", gate="bench", debt="over-budget"), case("a")]
        self.assertEqual(plan(cases, [run("debt", "FAIL", "124")], []), [
            ("alpha", 3, 1, "debt", "debt")])

    def test_stale_and_debt_overlap_runs_once(self):
        cases = [case("debt", gate="bench", debt="over-budget"), case("other")]
        self.assertEqual(plan(cases, [run("debt", "FAIL", "124")], ["alpha", "alpha"]), [
            ("alpha", 2, 0, "debt", "stale+debt"), ("alpha", 2, 1, "other", "stale")])

    def test_mixed_groups_keep_full_cardinality_and_indices(self):
        cases = [case("z"), case("b0", "beta"), case("debt", gate="bench", debt="over-budget"),
                 case("b1", "beta"), case("a")]
        self.assertEqual(plan(cases, [run("debt", "FAIL", "124")], ["beta"]), [
            ("alpha", 3, 1, "debt", "debt"), ("beta", 2, 0, "b0", "stale"),
            ("beta", 2, 1, "b1", "stale")])

    def test_only_declared_timeout_debt_is_added(self):
        cases = [case("required"), case("no-debt", gate="bench"),
                 case("empty-debt", gate="bench", debt=""),
                 case("other-failure", gate="bench", debt="over-budget"),
                 case("missing", gate="bench", debt="over-budget")]
        runs = [run("required", "FAIL", "124"), run("no-debt", "FAIL", "124"),
                run("empty-debt", "FAIL", "124"), run("other-failure", "FAIL", "1")]
        # Outcome validation remains a separate gate; these are not waived as debt.
        self.assertEqual(plan(cases, runs, []), [])

    def test_duplicate_case_or_record_is_refused(self):
        with self.assertRaisesRegex(ValueError, "CASES.tsv: duplicate"):
            plan([case("a"), case("a")], [], [])
        with self.assertRaisesRegex(ValueError, "RUNS.tsv: duplicate"):
            plan([case("a")], [run("a"), run("a")], [])

    def test_unknown_stale_group_or_record_is_refused(self):
        with self.assertRaisesRegex(ValueError, "unknown stale groups"):
            plan([case("a")], [], ["typo"])
        with self.assertRaisesRegex(ValueError, "undeclared records"):
            plan([case("a")], [run("typo")], [])

    def test_gate_runs_stale_cases_and_never_debt(self):
        cases = [case("debt", gate="bench", debt="over-budget"), case("other"),
                 case("idle-debt", "beta", gate="bench", debt="over-budget")]
        runs = [run("debt", "FAIL", "124"), run("idle-debt", "FAIL", "124")]
        self.assertEqual(choose(plan(cases, runs, ["alpha"]), "stale"), [
            ("alpha", 2, 1, "other", "stale")])

    def test_debt_watch_runs_every_debt_case_and_nothing_else(self):
        cases = [case("debt", gate="bench", debt="over-budget"), case("other"),
                 case("idle-debt", "beta", gate="bench", debt="over-budget")]
        runs = [run("debt", "FAIL", "124"), run("idle-debt", "FAIL", "124")]
        self.assertEqual(choose(plan(cases, runs, ["alpha"]), "debt"), [
            ("alpha", 2, 0, "debt", "stale+debt"), ("beta", 1, 0, "idle-debt", "debt")])

    def test_malformed_stale_list_is_refused(self):
        for stale in ["alpha", {}, [None]]:
            with self.subTest(stale=stale), self.assertRaisesRegex(ValueError, "JSON list"):
                plan([case("a")], [], stale)


def many(count, group="alpha"):
    return [case(f"MC{i}.cfg", group) for i in range(count)]


class RerunSliceTests(unittest.TestCase):
    def test_full_rerun_is_every_non_debt_case_in_declaration_order(self):
        cases = [case("z"), case("debt", gate="bench", debt="over-budget"), case("b0", "beta"), case("a")]
        self.assertEqual(rerun(cases, [run("debt", "FAIL", "124")]), [
            ("alpha", 3, 0, "z", "0"), ("alpha", 3, 2, "a", "0"), ("beta", 1, 0, "b0", "0")])

    def test_every_case_is_in_exactly_one_slice(self):
        cases = many(200) + [case(f"MCb{i}.cfg", "beta") for i in range(50)]
        full = set(rerun(cases, []))
        for slices in (1, 3, SLICES):
            with self.subTest(slices=slices):
                seen = []
                for index in range(slices):
                    seen.extend(rerun(cases, [], index, slices))
                self.assertEqual(sorted(seen), sorted(full))
                self.assertEqual(len(seen), len(set(seen)))

    def test_slice_key_is_the_case_id_never_its_line(self):
        # Plant: a key of the row's position would move every case below an
        # inserted row into another slice.
        cases = many(100)
        before = {entry[3]: index for index in range(SLICES) for entry in rerun(cases, [], index, SLICES)}
        moved = [case("MCnew.cfg")] + list(reversed(cases))
        after = {entry[3]: index for index in range(SLICES) for entry in rerun(moved, [], index, SLICES)}
        self.assertEqual({k: v for k, v in after.items() if k != "MCnew.cfg"}, before)
        self.assertEqual(slice_key("alpha", "MC1.cfg"),
                         int(hashlib.sha256(b"alpha/MC1.cfg").hexdigest()[:16], 16))
        # The group is part of the id: the same file name in another group is another case.
        self.assertNotEqual(slice_key("alpha", "MC1.cfg"), slice_key("beta", "MC1.cfg"))

    def test_slice_keeps_the_full_group_shard_index(self):
        cases = many(40)
        for index in range(SLICES):
            for group, count, shard, config, _ in rerun(cases, [], index, SLICES):
                self.assertEqual((count, config), (40, f"MC{shard}.cfg"))

    def test_day_slice_is_the_utc_day_number_mod_n(self):
        self.assertEqual(day_slice(20404), 20404 % SLICES)
        self.assertEqual([day_slice(d, 7) for d in range(14)], list(range(7)) * 2)
        with self.assertRaises(ValueError):
            day_slice(-1)
        with self.assertRaises(ValueError):
            day_slice(1, 0)

    def test_a_week_covers_every_case(self):
        cases = many(300)
        covered = set()
        for day in range(20404, 20404 + SLICES):
            covered.update(rerun(cases, [], day_slice(day)))
        self.assertEqual(covered, set(rerun(cases, [])))

    def test_zero_cases_is_an_error(self):
        with self.assertRaisesRegex(ValueError, "holds zero cases"):
            rerun([], [])
        with self.assertRaisesRegex(ValueError, "holds zero cases"):
            rerun([case("debt", gate="bench", debt="over-budget")], [run("debt", "FAIL", "124")])
        # A slice that happens to hold none of a tiny inventory is red, not green.
        lone = [case("MC1.cfg")]
        empty = [i for i in range(SLICES) if slice_key("alpha", "MC1.cfg") % SLICES != i][0]
        with self.assertRaisesRegex(ValueError, f"slice {empty}/{SLICES} holds zero cases"):
            rerun(lone, [], empty)

    def test_out_of_range_slice_is_refused(self):
        for index in (-1, SLICES):
            with self.subTest(index=index), self.assertRaisesRegex(ValueError, "outside"):
                rerun(many(10), [], index)

    def test_nightly_debt_refuses_every_problem_at_once(self):
        cases = [case("a"), case("d", gate="bench", debt="over-budget"), case("e", gate="bench", debt="over-budget")]
        runs = [run("a"), run("d", "FAIL", "124"), run("e", "PASS", "0")]

        def row(config, budget="900", measured="300"):
            return dict(config=config, budget_s=budget, measured_s=measured, host="bench", generated="10", distinct="5")

        good = nightly_debt([row("d")], cases, runs)
        self.assertEqual(list(good), ["d"])
        with self.assertRaises(ValueError) as caught:
            nightly_debt([row("a"), row("e"), row("d", "500"), row("d")], cases, runs)
        text = str(caught.exception)
        for want in ("a: not declared timeout debt", "e: its RUNS.tsv record is not the bounded FAIL/124",
                     "d: budget_s 500 is under twice measured_s 300", "d: listed twice"):
            self.assertIn(want, text)
        with self.assertRaisesRegex(ValueError, "columns"):
            nightly_debt([], cases, runs, ("config", "budget_s"))

    def test_margin_refuses_a_pass_over_sixty_percent_without_a_declared_budget(self):
        def rec(config, seconds, result="PASS", budget="110"):
            return dict(config=config, result=result, exit="0", seconds=seconds, budget=budget)

        # 66 s is 60% of 110 s: on the line is within it, a millisecond over is not.
        runs = [rec("on.cfg", "66"), rec("over.cfg", "66.001"), rec("debt.cfg", "110.2", "FAIL")]
        flagged, problems = margin(runs, {})
        self.assertEqual(flagged, [])
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("over.cfg: PASS in 66.001 s, over 60% of its 110 s budget (66 s)", problems[0])
        row = dict(config="over.cfg", budget_s="133", measured_s="66.001", slowest_s="66.001", slowest_host="record")
        flagged, problems = margin(runs, declared_budgets([row], runs))
        self.assertEqual(problems, [])
        self.assertEqual(flagged, ["TLC-MARGIN over.cfg PASS in 66.001 s of its 110 s budget (over 60%): "
                                   "declared budget 133 s in BUDGETS.tsv"])
        # A PASS record that cannot be measured against its budget is refused, not skipped.
        self.assertIn("must be positive numbers", margin([rec("bad.cfg", "-")], {})[1][0])

    def test_declared_budgets_refuse_every_problem_at_once(self):
        runs = [dict(config=c, result=r, exit=e, seconds=s, budget="110")
                for c, r, e, s in (("a", "PASS", "0", "100"), ("b", "PASS", "0", "50"),
                                   ("d", "FAIL", "124", "110.1"), ("e", "PASS", "0", "90"),
                                   ("g", "PASS", "0", "100"))]

        def row(config, budget="200", measured="100", host="hetzner2", slowest="100"):
            return dict(config=config, budget_s=budget, measured_s=measured, slowest_s=slowest, slowest_host=host)

        self.assertEqual(list(declared_budgets([row("a")], runs)), ["a"])
        with self.assertRaises(ValueError) as caught:
            declared_budgets([row("a", "199"), row("a"), row("b", "200", "50"), row("d", "900", "110.1"),
                              row("e", "200", "80", "-"), row("f"), row("g", slowest="101")], runs)
        text = str(caught.exception)
        for want in ("a: budget_s 199 is under twice measured_s 100", "a: listed twice",
                     "b: its record (50 s of 110 s) is within 60% of its budget: remove the declaration",
                     "d: has no PASS record", "e: measured_s 80 is not its RUNS.tsv record's seconds 90",
                     "e: slowest_host must name the machine", "f: has no PASS record",
                     "g: budget_s 200 is under twice slowest_s 101"):
            self.assertIn(want, text)
        with self.assertRaisesRegex(ValueError, "columns"):
            declared_budgets([], runs, ("config", "budget_s"))

    def test_declared_budget_runs_under_its_budget_in_its_slice(self):
        cases = [case(f"MC{i}.cfg") for i in range(12)]
        budgets = {"MC3.cfg": {"budget_s": "220"}}
        full = rerun(cases, [], budgets=budgets)
        self.assertEqual([entry[4] for entry in full if entry[3] == "MC3.cfg"], ["220"])
        self.assertTrue(all(entry[4] == "0" for entry in full if entry[3] != "MC3.cfg"))
        own = slice_key("alpha", "MC3.cfg") % SLICES
        for k in range(SLICES):
            picked = [entry[3] for entry in rerun(cases, [], k, budgets=budgets)]
            self.assertEqual("MC3.cfg" in picked, k == own)

    def test_declared_debt_lists_cases_whatever_their_records(self):
        cases = [case("a"), case("d", gate="bench", debt="over-budget"), case("b", gate="bench")]
        self.assertEqual(declared_debt(cases), ["d"])


class CommandLineTests(unittest.TestCase):
    def run_tool(self, *args, cases=None, runs=None, nightly=None, budgets=None):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            header = "config\tmodule\texpected\tproperty\tdeadlock\tgroup\tgate\tdebt\n"
            (root / "CASES.tsv").write_text(header + "".join(
                f"{c}\tM.tla\tpass\t-\tcheck\t{g}\t{gate}\t{debt}\n" for c, g, gate, debt in cases))
            (root / "RUNS.tsv").write_text("config\tresult\texit\tseconds\tbudget\n" + "".join(
                f"{c}\t{r}\t{e}\t{s[0] if s else 1}\t110\n" for c, r, e, *s in runs))
            (root / "stale.json").write_text("[]")
            if nightly is not None:
                (root / "NIGHTLY-DEBT.tsv").write_text("\t".join(NIGHTLY_DEBT_COLUMNS) + "\n" + "".join(
                    "\t".join(row) + "\n" for row in nightly))
            if budgets is not None:
                (root / "BUDGETS.tsv").write_text("\t".join(BUDGETS_COLUMNS) + "\n" + "".join(
                    "\t".join(row) + "\n" for row in budgets))
            return subprocess.run(
                [sys.executable, str(TOOL), "--cases", str(root / "CASES.tsv"), "--runs", str(root / "RUNS.tsv"),
                 "--nightly-debt", str(root / "NIGHTLY-DEBT.tsv"), "--budgets", str(root / "BUDGETS.tsv"),
                 *[str(root / "stale.json") if a == "STALE" else a for a in args]],
                capture_output=True, text=True, check=False)

    def test_nightly_debt_runs_in_every_slice_first_and_leaves_the_weekly_watch(self):
        cases = [(f"MC{i}.cfg", "alpha", "required", "-") for i in range(30)]
        cases += [("Fast.cfg", "alpha", "bench", "over-budget"), ("Slow.cfg", "alpha", "bench", "over-budget")]
        runs = [("Fast.cfg", "FAIL", "124"), ("Slow.cfg", "FAIL", "124")]
        nightly = [("Fast.cfg", "900", "380.3", "bench", "2837306", "474351")]
        for k in range(SLICES):
            done = self.run_tool("--select", "rerun", "--day", str(k), cases=cases, runs=runs, nightly=nightly)
            self.assertEqual(done.returncode, 0, done.stderr)
            lines = done.stdout.splitlines()
            self.assertEqual(lines[0], "alpha 32 30 Fast.cfg 900")
            self.assertNotIn("Slow.cfg", done.stdout)
            self.assertTrue(all(line.endswith(" 0") for line in lines[1:]), lines)
            self.assertIn("(1 nightly debt, 0 declared budget)", done.stderr)
        # Plant: without the file the case is not re-run at all, and is weekly debt again.
        bare = self.run_tool("--select", "rerun", "--full", cases=cases, runs=runs)
        self.assertNotIn("Fast.cfg", bare.stdout)
        weekly = self.run_tool("--select", "debt", "--stale", "STALE", cases=cases, runs=runs, nightly=nightly)
        self.assertEqual((weekly.returncode, weekly.stdout), (0, "alpha 32 31\n"), weekly.stderr)
        self.assertIn("TLC-DEBT Fast.cfg is nightly debt (tlc.yml re-run, budget 900 s)", weekly.stderr)
        both = self.run_tool("--select", "debt", "--stale", "STALE", cases=cases, runs=runs)
        self.assertEqual(both.stdout, "alpha 32 30\nalpha 32 31\n")

    def test_all_debt_nightly_leaves_the_weekly_watch_green_with_a_line(self):
        cases = [("a.cfg", "alpha", "required", "-"), ("d.cfg", "alpha", "bench", "over-budget")]
        done = self.run_tool("--select", "debt", "--stale", "STALE", cases=cases,
                             runs=[("a.cfg", "PASS", "0"), ("d.cfg", "FAIL", "124")],
                             nightly=[("d.cfg", "900", "300", "bench", "10", "5")])
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("TLC-DEBT CASES.tsv declares no weekly timeout debt: nothing to run this week", done.stderr)

    def test_nightly_debt_refusals_are_named_and_stop_the_plan(self):
        cases = [("a.cfg", "alpha", "required", "-"), ("d.cfg", "alpha", "bench", "over-budget")]
        runs = [("a.cfg", "PASS", "0"), ("d.cfg", "FAIL", "124")]
        for row, want in (
                (("a.cfg", "900", "300", "bench", "10", "5"), "a.cfg: not declared timeout debt"),
                (("d.cfg", "599", "300", "bench", "10", "5"), "d.cfg: budget_s 599 is under twice measured_s 300"),
                (("d.cfg", "900", "-", "bench", "10", "5"), "must be positive numbers of seconds"),
                (("d.cfg", "900", "300", "bench", "0", "5"), "must be the positive counts of a complete run")):
            for select in (("--select", "rerun", "--full"), ("--select", "debt", "--stale", "STALE")):
                with self.subTest(row=row, select=select):
                    done = self.run_tool(*select, cases=cases, runs=runs, nightly=[row])
                    self.assertNotEqual(done.returncode, 0)
                    self.assertIn(want, done.stderr)

    def test_declared_budget_reaches_the_plan_and_refusals_stop_it(self):
        cases = [("a.cfg", "alpha", "required", "-"), ("b.cfg", "alpha", "required", "-")]
        runs = [("a.cfg", "PASS", "0", "100"), ("b.cfg", "PASS", "0", "10")]
        done = self.run_tool("--select", "rerun", "--full", cases=cases, runs=runs,
                             budgets=[("a.cfg", "200", "100", "1", "hetzner2")])
        self.assertEqual((done.returncode, done.stdout), (0, "alpha 2 0 a.cfg 200\nalpha 2 1 b.cfg 0\n"), done.stderr)
        self.assertIn("TLC-PLAN rerun a.cfg (alpha shard 0/2) budget 200 s", done.stderr)
        self.assertIn("(0 nightly debt, 1 declared budget)", done.stderr)
        # Plant: without the file the case runs with no budget.
        bare = self.run_tool("--select", "rerun", "--full", cases=cases, runs=runs)
        self.assertEqual(bare.stdout, "alpha 2 0 a.cfg 0\nalpha 2 1 b.cfg 0\n")
        for row, want in ((("a.cfg", "199", "100", "1", "hetzner2"), "a.cfg: budget_s 199 is under twice measured_s 100"),
                          (("b.cfg", "200", "10", "1", "hetzner2"), "b.cfg: its record (10 s of 110 s) is within 60%")):
            with self.subTest(row=row):
                refused = self.run_tool("--select", "rerun", "--full", cases=cases, runs=runs, budgets=[row])
                self.assertNotEqual(refused.returncode, 0)
                self.assertIn(want, refused.stderr)

    def test_outputs_name_the_job(self):
        cases = [(f"MC{i}.cfg", "alpha", "required", "-") for i in range(60)]
        day = 20404
        done = self.run_tool("--select", "rerun", "--day", str(day), "--outputs", cases=cases, runs=[])
        self.assertEqual(done.returncode, 0, done.stderr)
        k = day % SLICES
        self.assertIn(f"title=TLC re-run slice {k}/{SLICES}\n", done.stdout)
        self.assertIn(f"TLC-RERUN slice {k}/{SLICES} day={day} cases=", done.stderr)
        full = self.run_tool("--select", "rerun", "--full", "--outputs", cases=cases, runs=[])
        self.assertIn("title=TLC re-run full\n", full.stdout)
        self.assertIn("count=60\n", full.stdout)

    def test_only_names_cases_and_refuses_unknown_ones(self):
        cases = [(f"MC{i}.cfg", "alpha", "required", "-") for i in range(5)]
        done = self.run_tool("--select", "rerun", "--full", "--only", "MC3.cfg", cases=cases, runs=[])
        self.assertEqual((done.returncode, done.stdout), (0, "alpha 5 3 MC3.cfg 0\n"), done.stderr)
        bad = self.run_tool("--select", "rerun", "--full", "--only", "MCtypo.cfg", cases=cases, runs=[])
        self.assertNotEqual(bad.returncode, 0)
        self.assertIn("--only names no re-run case: MCtypo.cfg", bad.stderr)

    def test_empty_rerun_exits_non_zero(self):
        done = self.run_tool("--select", "rerun", "--full", cases=[], runs=[])
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("holds zero cases", done.stderr)

    def test_debt_watch_with_declared_debt_and_no_plan_is_red(self):
        # The debt is declared but its record is no longer FAIL/124: nothing would run.
        cases = [("d.cfg", "alpha", "bench", "over-budget")]
        done = self.run_tool("--select", "debt", "--stale", "STALE", cases=cases, runs=[("d.cfg", "PASS", "0")])
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("zero cases planned while CASES.tsv declares timeout debt: d.cfg", done.stderr)

    def test_debt_watch_with_no_declared_debt_says_so_and_is_green(self):
        done = self.run_tool("--select", "debt", "--stale", "STALE",
                             cases=[("a.cfg", "alpha", "required", "-")], runs=[("a.cfg", "PASS", "0")])
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("TLC-DEBT CASES.tsv declares no weekly timeout debt: nothing to run this week", done.stderr)
        self.assertIn("TLC-PLAN cases=0", done.stderr)


if __name__ == "__main__":
    unittest.main()
