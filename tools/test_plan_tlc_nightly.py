#!/usr/bin/env python3
"""Planner controls: selection must not change pinned tlacheck shard identities."""

import unittest

from plan_tlc_nightly import plan


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

    def test_malformed_stale_list_is_refused(self):
        for stale in ["alpha", {}, [None]]:
            with self.subTest(stale=stale), self.assertRaisesRegex(ValueError, "JSON list"):
                plan([case("a")], [], stale)


if __name__ == "__main__":
    unittest.main()
