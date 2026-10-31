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


def run(run_id, branch="main", event="workflow_dispatch", sha=SHA):
    return {"id": run_id, "head_sha": sha, "head_branch": branch, "event": event}


def job(name, conclusion, status="completed"):
    return {"name": name, "conclusion": conclusion, "status": status}


class FakeApi:
    """Answers GETs from a table keyed by path without the paging query."""

    def __init__(self, runs, jobs):
        self.answers = {RUNS: {"workflow_runs": runs}}
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
            if path.startswith(RUNS):
                return {"workflow_runs": [run(7)] if path.endswith("page=1") else []}
            page = int(path.rsplit("page=", 1)[1])
            return {"jobs": pages.get(page, [])}

        code, _ = verdict(fetch)
        self.assertEqual(code, 0)


if __name__ == "__main__":
    unittest.main()
