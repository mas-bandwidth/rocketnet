#!/usr/bin/env python3
"""Drive tools/check_release_tsan.py with fake Actions API answers.

Run: python3 tools/test_check_release_tsan.py -v
No network: every API answer is a fixture.
"""

import io
import os
import sys
import unittest
import unittest.mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_release_tsan  # noqa: E402

REPO = "mas-bandwidth/rocketnet"
SHA = "77cd38b0a4f879f3ae04ee6681ac7d94bfd63151"
OTHER = "389baeef00000000000000000000000000000000"
RUNS = f"/repos/{REPO}/actions/workflows/nightly.yml/runs?head_sha={SHA}"
NIGHTLY_CI_RUNS = f"/repos/{REPO}/actions/workflows/nightly-ci.yml/runs?head_sha={SHA}"
CBMC_RUNS = f"/repos/{REPO}/actions/workflows/cbmc.yml/runs?head_sha={SHA}"
TLC_RUNS = f"/repos/{REPO}/actions/workflows/tlc.yml/runs?head_sha={SHA}"


def run(run_id, branch="main", event="workflow_dispatch", sha=SHA, conclusion="success", status="completed"):
    return {
        "id": run_id,
        "head_sha": sha,
        "head_branch": branch,
        "event": event,
        "conclusion": conclusion,
        "status": status,
    }


def job(name, conclusion, status="completed"):
    return {"name": name, "conclusion": conclusion, "status": status}


class FakeApi:
    """Answers GETs from a table keyed by path without the paging query."""

    def __init__(self, runs, jobs, nightly_ci_runs=None, cbmc_runs=None, tlc_runs=None):
        if nightly_ci_runs is None:
            nightly_ci_runs = [run(100)]
        if cbmc_runs is None:
            cbmc_runs = [run(200)]
        if tlc_runs is None:
            tlc_runs = [run(300)]
        self.answers = {
            RUNS: {"workflow_runs": runs},
            NIGHTLY_CI_RUNS: {"workflow_runs": nightly_ci_runs},
            CBMC_RUNS: {"workflow_runs": cbmc_runs},
            TLC_RUNS: {"workflow_runs": tlc_runs},
        }
        for run_id, run_jobs in jobs.items():
            self.answers[f"/repos/{REPO}/actions/runs/{run_id}/jobs"] = {"jobs": run_jobs}
        self.paths = []

    def __call__(self, path):
        self.paths.append(path)
        base, _, page = path.rpartition("per_page=100&page=")
        base = base.rstrip("?&")
        if page != "1" or base not in self.answers:
            raise check_release_tsan.ApiError(f"GET {path}: 404")
        return self.answers[base]


def verdict(api):
    out = io.StringIO()
    code = check_release_tsan.check(api, REPO, SHA, out)
    return code, out.getvalue()


class CheckReleaseTsan(unittest.TestCase):

    def test_no_run_fails(self):
        code, text = verdict(FakeApi([], {}))
        self.assertEqual(code, 1)
        self.assertIn("::error::No successful 'tsan (threaded transport)' run exists for " + SHA, text)
        self.assertIn("No nightly.yml run has this commit as its head.", text)

    def test_failed_run_fails(self):
        api = FakeApi([run(1, event="schedule")],
                      {1: [job("linux / clang (main)", "success"),
                           job("tsan (threaded transport) (main)", "failure")]})
        code, text = verdict(api)
        self.assertEqual(code, 1)
        self.assertIn("run 1 (schedule, main): tsan (threaded transport) (main): failure", text)

    def test_successful_run_passes(self):
        api = FakeApi([run(1, event="schedule"), run(2)],
                      {1: [job("tsan (threaded transport) (main)", "failure")],
                       2: [job("tsan (threaded transport) (main)", "success")]})
        code, text = verdict(api)
        self.assertEqual(code, 0)
        self.assertIn("ok: tsan (threaded transport) (main) succeeded on " + SHA + " in run 2", text)
        self.assertIn("ok: nightly-ci.yml (Nightly CI) succeeded on " + SHA + " in run 100", text)
        self.assertIn("ok: cbmc.yml (CBMC) succeeded on " + SHA + " in run 200", text)
        self.assertIn("ok: tlc.yml (TLC records) succeeded on " + SHA + " in run 300", text)

    def test_in_progress_run_fails(self):
        api = FakeApi([run(1)], {1: [job("tsan (threaded transport) (main)", None, "in_progress")]})
        code, text = verdict(api)
        self.assertEqual(code, 1)
        self.assertIn("tsan (threaded transport) (main): in_progress", text)

    def test_leg_for_another_ref_does_not_count(self):
        # a scheduled run with an extra ref in its matrix: that leg checked out
        # the other ref, not the run's head commit
        api = FakeApi([run(1, event="schedule")],
                      {1: [job("tsan (threaded transport) (main)", "failure"),
                           job("tsan (threaded transport) (rowan/feature)", "success")]})
        code, _ = verdict(api)
        self.assertEqual(code, 1)

    def test_run_for_another_commit_does_not_count(self):
        api = FakeApi([run(1, sha=OTHER)], {1: [job("tsan (threaded transport) (main)", "success")]})
        code, _ = verdict(api)
        self.assertEqual(code, 1)

    def test_dispatch_on_a_tag_counts(self):
        api = FakeApi([run(1, branch="v1.0.0-preview-6")],
                      {1: [job("tsan (threaded transport) (v1.0.0-preview-6)", "success")]})
        code, _ = verdict(api)
        self.assertEqual(code, 0)

    def test_missing_nightly_ci_run_fails(self):
        api = FakeApi(
            [run(1)],
            {1: [job("tsan (threaded transport) (main)", "success")]},
            nightly_ci_runs=[],
        )
        code, text = verdict(api)
        self.assertEqual(code, 1)
        self.assertIn("::error::No successful 'nightly-ci.yml' (Nightly CI) run exists for " + SHA, text)
        self.assertIn("No nightly-ci.yml run has this commit as its head.", text)

    def test_failed_nightly_ci_run_fails(self):
        api = FakeApi(
            [run(1)],
            {1: [job("tsan (threaded transport) (main)", "success")]},
            nightly_ci_runs=[run(10, conclusion="failure")],
        )
        code, text = verdict(api)
        self.assertEqual(code, 1)
        self.assertIn("run 10 (workflow_dispatch, main): failure", text)

    def test_in_progress_nightly_ci_run_fails(self):
        api = FakeApi(
            [run(1)],
            {1: [job("tsan (threaded transport) (main)", "success")]},
            nightly_ci_runs=[run(10, conclusion=None, status="in_progress")],
        )
        code, text = verdict(api)
        self.assertEqual(code, 1)
        self.assertIn("run 10 (workflow_dispatch, main): in_progress", text)

    def test_nightly_ci_run_for_another_commit_does_not_count(self):
        api = FakeApi(
            [run(1)],
            {1: [job("tsan (threaded transport) (main)", "success")]},
            nightly_ci_runs=[run(10, sha=OTHER, conclusion="success")],
        )
        code, _ = verdict(api)
        self.assertEqual(code, 1)

    def test_missing_cbmc_run_fails(self):
        api = FakeApi(
            [run(1)],
            {1: [job("tsan (threaded transport) (main)", "success")]},
            cbmc_runs=[],
        )
        code, text = verdict(api)
        self.assertEqual(code, 1)
        self.assertIn("::error::No successful 'cbmc.yml' (CBMC) run exists for " + SHA, text)
        self.assertIn("No cbmc.yml run has this commit as its head.", text)

    def test_failed_cbmc_run_fails(self):
        api = FakeApi(
            [run(1)],
            {1: [job("tsan (threaded transport) (main)", "success")]},
            cbmc_runs=[run(20, conclusion="failure")],
        )
        code, text = verdict(api)
        self.assertEqual(code, 1)
        self.assertIn("run 20 (workflow_dispatch, main): failure", text)

    def test_in_progress_cbmc_run_fails(self):
        api = FakeApi(
            [run(1)],
            {1: [job("tsan (threaded transport) (main)", "success")]},
            cbmc_runs=[run(20, conclusion=None, status="in_progress")],
        )
        code, text = verdict(api)
        self.assertEqual(code, 1)
        self.assertIn("run 20 (workflow_dispatch, main): in_progress", text)

    def test_cbmc_run_for_another_commit_does_not_count(self):
        api = FakeApi(
            [run(1)],
            {1: [job("tsan (threaded transport) (main)", "success")]},
            cbmc_runs=[run(20, sha=OTHER, conclusion="success")],
        )
        code, _ = verdict(api)
        self.assertEqual(code, 1)

    def test_missing_tlc_run_fails(self):
        api = FakeApi(
            [run(1)],
            {1: [job("tsan (threaded transport) (main)", "success")]},
            tlc_runs=[],
        )
        code, text = verdict(api)
        self.assertEqual(code, 1)
        self.assertIn("::error::No successful 'tlc.yml' (TLC records) run exists for " + SHA, text)
        self.assertIn("No tlc.yml run has this commit as its head.", text)

    def test_failed_tlc_run_fails(self):
        api = FakeApi(
            [run(1)],
            {1: [job("tsan (threaded transport) (main)", "success")]},
            tlc_runs=[run(30, conclusion="failure")],
        )
        code, text = verdict(api)
        self.assertEqual(code, 1)
        self.assertIn("run 30 (workflow_dispatch, main): failure", text)

    def test_in_progress_tlc_run_fails(self):
        api = FakeApi(
            [run(1)],
            {1: [job("tsan (threaded transport) (main)", "success")]},
            tlc_runs=[run(30, conclusion=None, status="in_progress")],
        )
        code, text = verdict(api)
        self.assertEqual(code, 1)
        self.assertIn("run 30 (workflow_dispatch, main): in_progress", text)

    def test_tlc_run_for_another_commit_does_not_count(self):
        api = FakeApi(
            [run(1)],
            {1: [job("tsan (threaded transport) (main)", "success")]},
            tlc_runs=[run(30, sha=OTHER, conclusion="success")],
        )
        code, _ = verdict(api)
        self.assertEqual(code, 1)

    def test_all_four_workflows_succeed_after_retries(self):
        api = FakeApi(
            [run(1, conclusion="failure"), run(2, conclusion="success")],
            {
                1: [job("tsan (threaded transport) (main)", "failure")],
                2: [job("tsan (threaded transport) (main)", "success")],
            },
            nightly_ci_runs=[run(10, conclusion="failure"), run(11, conclusion="success")],
            cbmc_runs=[run(20, conclusion="failure"), run(21, conclusion="success")],
            tlc_runs=[run(30, conclusion="failure"), run(31, conclusion="success")],
        )
        code, text = verdict(api)
        self.assertEqual(code, 0)
        self.assertIn("ok: tsan (threaded transport) (main) succeeded on " + SHA + " in run 2", text)
        self.assertIn("ok: nightly-ci.yml (Nightly CI) succeeded on " + SHA + " in run 11", text)
        self.assertIn("ok: cbmc.yml (CBMC) succeeded on " + SHA + " in run 21", text)
        self.assertIn("ok: tlc.yml (TLC records) succeeded on " + SHA + " in run 31", text)

    def test_api_error_propagates(self):
        api = FakeApi([run(1)], {})
        with self.assertRaises(check_release_tsan.ApiError):
            verdict(api)

    def test_main_refuses_an_incomplete_environment(self):
        saved = dict(os.environ)
        try:
            for name in ("GITHUB_REPOSITORY", "GITHUB_SHA", "GITHUB_TOKEN"):
                os.environ.pop(name, None)
            with unittest.mock.patch("sys.stdout", new_callable=io.StringIO):
                self.assertEqual(check_release_tsan.main(), 2)
        finally:
            os.environ.clear()
            os.environ.update(saved)

    def test_pages_are_followed(self):
        filler = [job(f"other {i}", "success") for i in range(100)]
        pages = {1: filler, 2: [job("tsan (threaded transport) (main)", "success")]}

        def fetch(path):
            if "/runs?head_sha=" in path:
                return {"workflow_runs": [run(7)] if path.endswith("page=1") else []}
            page = int(path.rsplit("page=", 1)[1])
            return {"jobs": pages.get(page, [])}

        code, _ = verdict(fetch)
        self.assertEqual(code, 0)


if __name__ == "__main__":
    unittest.main()
