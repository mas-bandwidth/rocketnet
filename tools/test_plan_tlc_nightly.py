#!/usr/bin/env python3
"""Planner controls: selection must not change pinned tlacheck shard identities."""

import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from plan_tlc_nightly import SLICES, choose, day_slice, declared_debt, plan, rerun, slice_key

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
            ("alpha", 3, 0, "z"), ("alpha", 3, 2, "a"), ("beta", 1, 0, "b0")])

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
            for group, count, shard, config in rerun(cases, [], index, SLICES):
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

    def test_declared_debt_lists_cases_whatever_their_records(self):
        cases = [case("a"), case("d", gate="bench", debt="over-budget"), case("b", gate="bench")]
        self.assertEqual(declared_debt(cases), ["d"])


class CommandLineTests(unittest.TestCase):
    def run_tool(self, *args, cases=None, runs=None):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            header = "config\tmodule\texpected\tproperty\tdeadlock\tgroup\tgate\tdebt\n"
            (root / "CASES.tsv").write_text(header + "".join(
                f"{c}\tM.tla\tpass\t-\tcheck\t{g}\t{gate}\t{debt}\n" for c, g, gate, debt in cases))
            (root / "RUNS.tsv").write_text("config\tresult\texit\tseconds\n" + "".join(
                f"{c}\t{r}\t{e}\t1\n" for c, r, e in runs))
            (root / "stale.json").write_text("[]")
            return subprocess.run(
                [sys.executable, str(TOOL), "--cases", str(root / "CASES.tsv"), "--runs", str(root / "RUNS.tsv"),
                 *[str(root / "stale.json") if a == "STALE" else a for a in args]],
                capture_output=True, text=True, check=False)

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
        self.assertEqual((done.returncode, done.stdout), (0, "alpha 5 3 MC3.cfg\n"), done.stderr)
        bad = self.run_tool("--select", "rerun", "--full", "--only", "MCtypo.cfg", cases=cases, runs=[])
        self.assertNotEqual(bad.returncode, 0)
        self.assertIn("--only names no non-debt case: MCtypo.cfg", bad.stderr)

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
        self.assertIn("TLC-DEBT CASES.tsv declares no timeout debt: nothing to run this week", done.stderr)
        self.assertIn("TLC-PLAN cases=0", done.stderr)


if __name__ == "__main__":
    unittest.main()
