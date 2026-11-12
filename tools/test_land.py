#!/usr/bin/env python3
"""tools/land.py against fixtures: it merges only an up-to-date head with every per-commit job success.

Run: python3 tools/test_land.py -v
No network, no clock: tools/release_testkit.py's FakeApi answers and records
every write; nothing is merged, re-run or commented anywhere real.
"""

import io
import itertools
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import land  # noqa: E402
from release_testkit import MAIN_SHA, SHA, FakeApi, green_pull_request, job  # noqa: E402

REPO = "mas-bandwidth/rocketnet"
PR = 7
HEAD = SHA + "-pr"


def run_land(api, dry_run=True):
    out = io.StringIO()
    ticks = itertools.count(0, 60)   # a minute per look: a wait that never ends hits the deadline
    code = land.land(api, REPO, PR, dry_run=dry_run, out=out, sleep=lambda s: None, clock=lambda: next(ticks))
    return code, out.getvalue()


def cap_cancel(api, runs, workflow="ci.yml", index=0, secs=124):
    run = runs[workflow]
    name = api.jobs[run["id"]][index]["name"]
    api.jobs[run["id"]][index] = job(name, "cancelled", secs=secs)
    run["conclusion"] = "cancelled"
    return run, name


def finish_rerun(conclusion):
    def on_rerun(api, run_id):
        run = next(r for r in api.runs if r["id"] == run_id)
        run["run_attempt"] += 1
        api.jobs[run_id] = [job(j["name"], conclusion if j["conclusion"] == "cancelled" else j["conclusion"],
                                secs=124 if conclusion == "cancelled" else 60)
                            for j in api.jobs[run_id]]
        run["conclusion"] = "success" if conclusion == "success" else conclusion
    return on_rerun


class Land(unittest.TestCase):

    def test_green_dry_run_would_merge_and_touches_nothing(self):
        api = FakeApi()
        green_pull_request(api)
        code, text = run_land(api)
        self.assertEqual(code, 0, text)
        self.assertIn("VERDICT: LAND #7", text)
        self.assertIn("would merge", text)
        self.assertEqual(api.writes, [])

    def test_green_merges_with_the_head_pinned(self):
        api = FakeApi()
        green_pull_request(api)
        code, text = run_land(api, dry_run=False)
        self.assertEqual(code, 0, text)
        self.assertEqual(api.writes, [("PUT", f"/repos/{REPO}/pulls/7/merge", {"sha": HEAD, "merge_method": "merge"})])

    def test_cap_cancelled_job_refuses_then_one_rerun_then_lands(self):
        api = FakeApi()
        runs = green_pull_request(api)
        run, name = cap_cancel(api, runs)
        api.on_rerun = finish_rerun("success")
        code, text = run_land(api, dry_run=False)
        self.assertEqual(code, 0, text)
        self.assertIn(f"CAP   run {run['id']}: cancelled at the 120 s cap: {name} (124 s)", text)
        self.assertIn(f"RERUN land.py: one re-run of jobs cancelled at the 120 s cap", text)
        self.assertIn("VERDICT: LAND #7", text)
        kinds = [(w[0], w[1]) for w in api.writes]
        self.assertEqual(kinds, [
            ("POST", f"/repos/{REPO}/actions/runs/{run['id']}/rerun-failed-jobs"),
            ("POST", f"/repos/{REPO}/issues/7/comments"),
            ("PUT", f"/repos/{REPO}/pulls/7/merge"),
        ])
        self.assertIn(name, api.writes[1][2]["body"])

    def test_cap_cancelled_dry_run_refuses_and_prints_the_comment(self):
        api = FakeApi()
        runs = green_pull_request(api)
        _, name = cap_cancel(api, runs)
        code, text = run_land(api)
        self.assertEqual(code, 1)
        self.assertIn("dry run: would re-run once", text)
        self.assertIn(name, text)
        self.assertEqual(api.writes, [])

    def test_second_cancel_after_the_rerun_refuses(self):
        api = FakeApi()
        runs = green_pull_request(api)
        cap_cancel(api, runs)
        api.on_rerun = finish_rerun("cancelled")
        code, text = run_land(api, dry_run=False)
        self.assertEqual(code, 1)
        self.assertIn("cancelled again at the cap", text)
        self.assertNotIn("PUT", [w[0] for w in api.writes])

    def test_cancel_on_a_rerun_attempt_refuses_without_another_rerun(self):
        api = FakeApi()
        runs = green_pull_request(api)
        run, _ = cap_cancel(api, runs)
        run["run_attempt"] = 2
        code, text = run_land(api, dry_run=False)
        self.assertEqual(code, 1)
        self.assertIn("the one re-run is spent", text)
        self.assertEqual(api.writes, [])

    def test_short_cancel_is_not_a_cap_overrun(self):
        api = FakeApi()
        runs = green_pull_request(api)
        cap_cancel(api, runs, secs=40)
        code, text = run_land(api, dry_run=False)
        self.assertEqual(code, 1)
        self.assertIn("before the 120 s cap", text)
        self.assertEqual(api.writes, [])

    def test_failure_refuses_without_rerun(self):
        api = FakeApi()
        runs = green_pull_request(api)
        api.jobs[runs["static.yml"]["id"]][0] = job("static analysis (gcc -fanalyzer)", "failure")
        code, text = run_land(api, dry_run=False)
        self.assertEqual(code, 1)
        self.assertIn("red: static analysis (gcc -fanalyzer): failure", text)
        self.assertEqual(api.writes, [])

    def test_cap_cancel_beside_a_failure_refuses_without_rerun(self):
        api = FakeApi()
        runs = green_pull_request(api)
        cap_cancel(api, runs)
        api.jobs[runs["docs.yml"]["id"]][0] = job("docs (MrDocs HTML reference build)", "failure")
        code, text = run_land(api, dry_run=False)
        self.assertEqual(code, 1)
        self.assertEqual(api.writes, [])

    def test_missing_job_refuses(self):
        api = FakeApi()
        runs = green_pull_request(api)
        del api.jobs[runs["ci.yml"]["id"]][1]
        code, text = run_land(api, dry_run=False)
        self.assertEqual(code, 1)
        self.assertIn("red: lint (workflows, format, standard, tool selftests): missing", text)
        self.assertEqual(api.writes, [])

    def test_missing_workflow_run_refuses(self):
        api = FakeApi()
        runs = green_pull_request(api)
        api.runs.remove(runs["docs.yml"])
        code, text = run_land(api, dry_run=False)
        self.assertEqual(code, 1)
        self.assertIn("MISSING: no pull_request run of docs.yml", text)

    def test_unexpected_skip_refuses(self):
        api = FakeApi()
        runs = green_pull_request(api)
        api.jobs[runs["ci.yml"]["id"]][0] = job("linux / Debug / unit", "skipped", secs=0)
        code, text = run_land(api, dry_run=False)
        self.assertEqual(code, 1)
        self.assertIn("skipped (no skip row", text)

    def test_in_progress_refuses(self):
        api = FakeApi()
        runs = green_pull_request(api)
        api.jobs[runs["ci.yml"]["id"]][0] = job("linux / Debug / unit", status="in_progress")
        runs["ci.yml"]["status"] = "in_progress"
        code, text = run_land(api, dry_run=False)
        self.assertEqual(code, 1)
        self.assertIn("IN PROGRESS", text)

    def test_stale_base_refuses(self):
        api = FakeApi()
        green_pull_request(api)
        api.ancestors.discard((MAIN_SHA, HEAD))
        code, text = run_land(api, dry_run=False)
        self.assertEqual(code, 1)
        self.assertIn("FAIL  head is up to date with main", text)
        self.assertEqual(api.writes, [])

    def test_closed_pull_request_refuses(self):
        api = FakeApi()
        green_pull_request(api)
        api.pulls[PR]["state"] = "closed"
        code, _ = run_land(api, dry_run=False)
        self.assertEqual(code, 1)
        self.assertEqual(api.writes, [])

    def test_other_base_refuses(self):
        api = FakeApi()
        green_pull_request(api)
        api.pulls[PR]["base"]["ref"] = "sprint/rocketnet-proof"
        code, _ = run_land(api, dry_run=False)
        self.assertEqual(code, 1)

    def test_no_ruleset_on_main_refuses(self):
        api = FakeApi()
        green_pull_request(api)
        del api.files[(".github/rulesets/main.json", MAIN_SHA)]
        code, text = run_land(api, dry_run=False)
        self.assertEqual(code, 1)
        self.assertIn("no list of required job names", text)


if __name__ == "__main__":
    unittest.main()
