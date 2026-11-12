#!/usr/bin/env python3
"""Drive tools/check_nightly_dispositions.py (the nightly watch) with a fake
GitHub API: each night that gave no verdict must turn the watch red, and a
clean all-green night must leave it green.

Run: python3 tools/test_check_nightly_dispositions.py -v
No network, no clock, no git: the API, the time, the file ages and the sleep
between retries are all injected.
"""

import datetime
import io
import json
import os
import sys
import tempfile
import unittest
import unittest.mock
import urllib.error
import urllib.parse
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_nightly_dispositions as watch  # noqa: E402
import nightly_replay  # noqa: E402

REPO = "mas-bandwidth/rocketnet"
UTC = datetime.timezone.utc
NOW = datetime.datetime(2026, 11, 12, 14, 0, tzinfo=UTC)  # a Thursday
OLD = datetime.datetime(2026, 10, 1, tzinfo=UTC)
SHA_BAD = "b" * 40
SHA_GOOD = "a" * 40

NIGHTLY = """name: nightly
on:
  schedule:
    - cron: "40 8 * * *"
  workflow_dispatch:
jobs:
  unit:
    name: ${{ matrix.os }} / unit (main)
    strategy:
      matrix:
        os: [linux, macos]
"""
CBMC = """name: CBMC
on:
  schedule:
    - cron: '10 5 * * *'   # daily
jobs:
  cbmc:
    name: cbmc
"""
WEEKLY = """on:
  schedule:
    - cron: "0 6 * * 6"   # weekly, Saturday
jobs:
  weekly:
    name: weekly
"""
CI = """on:
  push:
  # schedule: is only in a comment here
"""
WATCH = """on:
  schedule:
    - cron: "30 13 * * *"
"""


def run(run_id, created, conclusion="success", status="completed", sha=SHA_BAD):
    return {"id": run_id, "created_at": created, "status": status,
            "conclusion": conclusion if status == "completed" else None, "head_sha": sha}


def job(name, conclusion="success", status="completed", job_id=0):
    return {"id": job_id, "name": name, "conclusion": conclusion, "status": status}


class FakeApi:
    """GET router: /actions/workflows/<f>/runs, /actions/runs/<id>/jobs,
    /pulls/<n>, /compare/... . `broken` maps a path to how many times it
    answers HTTP 503 before answering (a large number: always)."""

    def __init__(self, runs=None, jobs=None, pulls=None, broken=None, errors=None, everything=None,
                 notes=None):
        self.runs = runs or {}
        # check-run (job) id -> its annotations
        self.notes = notes or {}
        self.jobs = jobs or {}
        self.pulls = pulls or {}
        self.broken = dict(broken or {})
        # path -> exceptions raised in order, one per GET, before the path answers
        self.errors = {k: list(v) for k, v in (errors or {}).items()}
        # an exception every GET raises (the API is unreachable)
        self.everything = everything
        self.calls = []

    def __call__(self, url):
        parsed = urllib.parse.urlparse(url)
        path = parsed.path
        self.calls.append(path)
        if self.everything is not None:
            raise self.everything
        if self.errors.get(path):
            raise self.errors[path].pop(0)
        if self.broken.get(path, 0) > 0:
            self.broken[path] -= 1
            raise urllib.error.HTTPError(url, 503, "Service Unavailable", {}, None)
        prefix = f"/repos/{REPO}"
        assert path.startswith(prefix), path
        rest = path[len(prefix):]
        query = urllib.parse.parse_qs(parsed.query)
        if rest.startswith("/actions/workflows/"):
            name = rest.split("/")[3]
            assert query.get("event") == ["schedule"] and query.get("branch") == ["main"], url
            return {"workflow_runs": self.runs.get(name, [])}
        if rest.startswith("/actions/runs/"):
            run_id = int(rest.split("/")[3])
            found = self.jobs.get(run_id, [])
            return {"total_count": len(found), "jobs": found}
        if rest.startswith("/pulls/"):
            return self.pulls[int(rest.split("/")[2])]
        if rest.startswith("/compare/"):
            return {"ahead_by": 7}
        if rest.startswith("/check-runs/") and rest.endswith("/annotations"):
            return list(self.notes.get(int(rest.split("/")[2]), []))
        raise AssertionError(f"unexpected GET {url}")


class WatchCase(unittest.TestCase):
    def setUp(self):
        # the runner sets GITHUB_API_URL; the cases name github.com's root
        environment = unittest.mock.patch.dict(os.environ)
        environment.start()
        self.addCleanup(environment.stop)
        os.environ.pop("GITHUB_API_URL", None)
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        os.makedirs(os.path.join(self.root, ".github", "workflows"))
        os.makedirs(os.path.join(self.root, "tools"))
        self.workflows({"nightly.yml": NIGHTLY, "ci.yml": CI, "nightly-watch.yml": WATCH})
        self.rows("")
        self.since = {}

    def tearDown(self):
        self.tmp.cleanup()

    def workflows(self, files):
        directory = os.path.join(self.root, ".github", "workflows")
        for name in os.listdir(directory):
            os.remove(os.path.join(directory, name))
        for name, text in files.items():
            with open(os.path.join(directory, name), "w", encoding="utf-8") as f:
                f.write(text)

    def rows(self, text):
        with open(os.path.join(self.root, "tools", "nightly_dispositions.txt"), "w", encoding="utf-8") as f:
            f.write("# header\n" + text)

    def watch(self, api, *extra):
        out = io.StringIO()
        sleeps = []
        code = watch.main(["--root", self.root, *extra], fetch=api, now=NOW, sleep=sleeps.append,
                          since=lambda name: self.since.get(name, OLD), out=out)
        self.sleeps = sleeps
        return code, out.getvalue()

    def assertExit(self, expected, code, output, *lines):
        self.assertEqual(expected, code, output)
        for line in lines:
            self.assertIn(line, output)
        print(f"\n--- exit {code}\n{output}", end="", file=sys.stderr)


GREEN_RUNS = {"nightly.yml": [run(101, "2026-11-12T08:45:00Z"), run(100, "2026-11-11T08:44:00Z", sha=SHA_GOOD)]}
GREEN_JOBS = {101: [job("linux / unit (main)"), job("macos / unit (main)")],
              100: [job("linux / unit (main)"), job("macos / unit (main)")]}


class Controls(WatchCase):
    def test_clean_all_green_is_green(self):
        code, out = self.watch(FakeApi(GREEN_RUNS, GREEN_JOBS))
        self.assertExit(0, code, out, "nightly watch: green: 0 ", "1 green")

    def test_failed_job_without_row_is_new_red(self):
        jobs = dict(GREEN_JOBS)
        jobs[101] = [job("linux / unit (main)", "failure"), job("macos / unit (main)")]
        code, out = self.watch(FakeApi(GREEN_RUNS, jobs))
        self.assertExit(1, code, out, "NEW-RED nightly.yml :: linux / unit (main)",
                        f"BISECT git bisect start {SHA_BAD[:12]} {SHA_GOOD[:12]}")


class NoVerdictPlants(WatchCase):
    """The seven nights the old watch passed (VERIFY-2 A1), each now red."""

    def test_no_run_at_all(self):
        code, out = self.watch(FakeApi({}, {}))
        self.assertExit(1, code, out, "NO-VERDICT nightly.yml :: (run) NO-RUN: no scheduled run on main at all")

    def test_stale_newest_run_23_days_old(self):
        runs = {"nightly.yml": [run(90, "2026-10-20T08:45:00Z")]}
        code, out = self.watch(FakeApi(runs, {90: [job("linux / unit (main)")]}))
        self.assertExit(1, code, out, "NO-VERDICT nightly.yml :: (run) stale: newest scheduled run 90 was created "
                        "2026-10-20 08:45Z, before the firing due 2026-11-12 08:40Z")

    def test_cancelled_run_every_job_cancelled(self):
        runs = {"nightly.yml": [run(101, "2026-11-12T08:45:00Z", "cancelled")]}
        jobs = {101: [job("linux / unit (main)", "cancelled"), job("macos / unit (main)", "cancelled")]}
        code, out = self.watch(FakeApi(runs, jobs))
        self.assertExit(1, code, out, "NO-VERDICT nightly.yml :: linux / unit (main) conclusion=cancelled",
                        "NO-VERDICT nightly.yml :: macos / unit (main) conclusion=cancelled")

    def test_timed_out_job(self):
        runs = {"nightly.yml": [run(101, "2026-11-12T08:45:00Z", "timed_out")]}
        jobs = {101: [job("linux / unit (main)", "timed_out"), job("macos / unit (main)")]}
        code, out = self.watch(FakeApi(runs, jobs))
        self.assertExit(1, code, out, "NO-VERDICT nightly.yml :: linux / unit (main) conclusion=timed_out")

    def test_in_progress_for_four_days_older_green_stands_in(self):
        runs = {"nightly.yml": [run(101, "2026-11-08T08:45:00Z", status="in_progress"),
                                run(100, "2026-11-07T08:45:00Z")]}
        jobs = {101: [job("linux / unit (main)", None, "in_progress")], 100: [job("linux / unit (main)")]}
        code, out = self.watch(FakeApi(runs, jobs))
        self.assertExit(1, code, out, "NO-VERDICT nightly.yml :: (run 101) in_progress since 2026-11-08 08:45Z, "
                        "past the next firing 2026-11-09 08:40Z plus grace")

    def test_startup_failure_no_jobs(self):
        runs = {"nightly.yml": [run(101, "2026-11-12T08:45:00Z", "startup_failure")]}
        code, out = self.watch(FakeApi(runs, {101: []}))
        self.assertExit(1, code, out, "NO-VERDICT nightly.yml :: (run 101) conclusion=startup_failure with zero jobs")

    def test_every_job_skipped(self):
        runs = {"nightly.yml": [run(101, "2026-11-12T08:45:00Z", "success")]}
        jobs = {101: [job("linux / unit (main)", "skipped"), job("macos / unit (main)", "skipped")]}
        code, out = self.watch(FakeApi(runs, jobs))
        self.assertExit(1, code, out, "NO-VERDICT nightly.yml :: (run 101) every one of its 2 jobs was skipped")

    def test_one_job_skipped_without_declaration(self):
        jobs = dict(GREEN_JOBS)
        jobs[101] = [job("linux / unit (main)"), job("macos / unit (main)", "skipped")]
        code, out = self.watch(FakeApi(GREEN_RUNS, jobs))
        self.assertExit(1, code, out, "NO-VERDICT nightly.yml :: macos / unit (main) conclusion=skipped")

    def test_run_failed_with_every_job_green(self):
        runs = {"nightly.yml": [run(101, "2026-11-12T08:45:00Z", "failure")]}
        code, out = self.watch(FakeApi(runs, {101: [job("linux / unit (main)")]}))
        self.assertExit(1, code, out, "NO-VERDICT nightly.yml :: (run 101) conclusion=failure and no job explains it")


class NotRedYet(WatchCase):
    def test_young_run_in_progress_previous_night_stands(self):
        runs = {"nightly.yml": [run(102, "2026-11-12T08:45:00Z", status="in_progress"),
                                run(101, "2026-11-11T08:45:00Z")]}
        jobs = {102: [job("linux / unit (main)", None, "in_progress")], 101: [job("linux / unit (main)")]}
        code, out = self.watch(FakeApi(runs, jobs))
        self.assertExit(0, code, out, "RUNNING nightly.yml run=102", "RUN nightly.yml run=101")

    def test_young_run_in_progress_but_previous_night_missing(self):
        runs = {"nightly.yml": [run(102, "2026-11-12T08:45:00Z", status="in_progress"),
                                run(101, "2026-11-09T08:45:00Z")]}
        jobs = {102: [], 101: [job("linux / unit (main)")]}
        code, out = self.watch(FakeApi(runs, jobs))
        self.assertExit(1, code, out, "NO-VERDICT nightly.yml :: (run) stale: newest completed scheduled run 101")

    def test_new_file_before_its_first_firing_is_pending(self):
        self.workflows({"tlc-debt.yml": WEEKLY})
        self.since = {"tlc-debt.yml": datetime.datetime(2026, 11, 10, 20, 10, tzinfo=UTC)}
        code, out = self.watch(FakeApi({}, {}))
        self.assertExit(0, code, out, "PENDING tlc-debt.yml: on main since 2026-11-10 20:10Z, "
                        "no firing due yet (next 2026-11-14 06:00Z)")

    def test_weekly_file_older_than_a_week_with_no_run_is_red(self):
        self.workflows({"tlc-debt.yml": WEEKLY})
        code, out = self.watch(FakeApi({}, {}))
        self.assertExit(1, code, out, "NO-VERDICT tlc-debt.yml :: (run) NO-RUN")

    def test_unknown_file_age_counts_as_old(self):
        self.since = {"nightly.yml": None}
        code, out = self.watch(FakeApi({}, {}))
        self.assertExit(1, code, out, "NO-VERDICT nightly.yml :: (run) NO-RUN")


def http_error(code, headers=None):
    return urllib.error.HTTPError("https://api.github.com/x", code, "planted", headers or {}, None)


NIGHTLY_RUNS_PATH = f"/repos/{REPO}/actions/workflows/nightly.yml/runs"
CBMC_RUNS_PATH = f"/repos/{REPO}/actions/workflows/cbmc.yml/runs"


class ApiErrors(WatchCase):
    """The HTTP layer is injected: each case raises what GitHub or the network would."""

    def test_503_forever_four_tries_then_could_not_read_and_listing_continues(self):
        self.workflows({"cbmc.yml": CBMC, "nightly.yml": NIGHTLY})
        api = FakeApi(GREEN_RUNS, GREEN_JOBS, broken={CBMC_RUNS_PATH: 99})
        code, out = self.watch(api)
        self.assertExit(1, code, out,
                        "API-ERROR could not read run state: GET https://api.github.com/repos/mas-bandwidth/"
                        "rocketnet/actions/workflows/cbmc.yml/runs?",
                        "HTTP 503 after 4 tries", "RUN nightly.yml run=101", "API-ERROR 1",
                        "nightly watch: RED: could not read run state for 1 request(s)")
        self.assertEqual(4, api.calls.count(CBMC_RUNS_PATH))
        self.assertEqual([5, 10, 20], self.sleeps)
        self.assertNotIn("nightly watch: green", out)

    def test_503_three_times_then_answer_is_green(self):
        code, out = self.watch(FakeApi(GREEN_RUNS, GREEN_JOBS, broken={NIGHTLY_RUNS_PATH: 3}))
        self.assertExit(0, code, out, "nightly watch: green")
        self.assertEqual([5, 10, 20], self.sleeps)

    def test_pull_request_lookup_503_is_could_not_read_not_a_crash(self):
        # nightly-watch run 43228458260: a 503 on GET /pulls/<n> for a disposition row
        # ended the old watch with a traceback before it read nightly.yml's last jobs
        self.rows("nightly.yml\tlinux / unit\t#12\t2026-11-10\tknown cause\n")
        jobs = {101: [job("linux / unit (main)", "failure"), job("macos / unit (main)")],
                100: [job("linux / unit (main)"), job("macos / unit (main)")]}
        api = FakeApi(GREEN_RUNS, jobs, broken={f"/repos/{REPO}/pulls/12": 99})
        code, out = self.watch(api)
        self.assertExit(1, code, out, f"API-ERROR could not read run state: GET https://api.github.com/repos/"
                        f"{REPO}/pulls/12: HTTP 503 after 4 tries", "nightly watch: RED")

    def test_rate_limit_429_waits_what_github_asks(self):
        api = FakeApi(GREEN_RUNS, GREEN_JOBS, errors={NIGHTLY_RUNS_PATH: [http_error(429, {"Retry-After": "45"})]})
        code, out = self.watch(api)
        self.assertExit(0, code, out, "nightly watch: green")
        self.assertEqual([45], self.sleeps)

    def test_rate_limit_403_waits_for_the_reset_capped(self):
        reset = int(NOW.timestamp()) + 30
        limited = http_error(403, {"X-RateLimit-Remaining": "0", "X-RateLimit-Reset": str(reset)})
        far = http_error(403, {"X-RateLimit-Remaining": "0", "X-RateLimit-Reset": str(reset + 3600)})
        api = FakeApi(GREEN_RUNS, GREEN_JOBS, errors={NIGHTLY_RUNS_PATH: [limited, far]})
        code, out = self.watch(api)
        self.assertExit(0, code, out, "nightly watch: green")
        self.assertEqual([30, watch.MAX_WAIT], self.sleeps)

    def test_a_short_wait_github_names_never_cuts_the_backoff(self):
        api = FakeApi(GREEN_RUNS, GREEN_JOBS, errors={NIGHTLY_RUNS_PATH: [http_error(503, {"Retry-After": "1"})]})
        code, out = self.watch(api)
        self.assertExit(0, code, out, "nightly watch: green")
        self.assertEqual([5], self.sleeps)

    def test_403_that_is_not_a_rate_limit_is_not_retried(self):
        api = FakeApi(GREEN_RUNS, GREEN_JOBS, errors={NIGHTLY_RUNS_PATH: [http_error(403)]})
        code, out = self.watch(api)
        self.assertExit(1, code, out, "API-ERROR could not read run state:", "HTTP 403, not retried")
        self.assertEqual(1, api.calls.count(NIGHTLY_RUNS_PATH))
        self.assertEqual([], self.sleeps)

    def test_404_is_not_retried(self):
        jobs_path = f"/repos/{REPO}/actions/runs/101/jobs"
        api = FakeApi(GREEN_RUNS, GREEN_JOBS, errors={jobs_path: [http_error(404)]})
        code, out = self.watch(api)
        self.assertExit(1, code, out, "HTTP 404, not retried", "API-ERROR 1")
        self.assertEqual(1, api.calls.count(jobs_path))
        self.assertEqual([], self.sleeps)

    def test_a_workflow_github_does_not_know_yet_is_pending_when_new(self):
        # planted-violations.yml on the night it lands: GitHub answers 404 for its runs
        self.since = {"nightly.yml": NOW - datetime.timedelta(hours=2)}
        api = FakeApi(GREEN_RUNS, GREEN_JOBS, errors={NIGHTLY_RUNS_PATH: [http_error(404)]})
        code, out = self.watch(api)
        self.assertExit(0, code, out, "PENDING nightly.yml", "API-ERROR 0")

    def test_a_workflow_github_does_not_know_is_no_run_when_old(self):
        api = FakeApi(GREEN_RUNS, GREEN_JOBS, errors={NIGHTLY_RUNS_PATH: [http_error(404)]})
        code, out = self.watch(api)
        self.assertExit(1, code, out, "NO-VERDICT nightly.yml :: (run) NO-RUN", "API-ERROR 0")

    def test_connection_refused_and_bad_json_are_retried(self):
        errors = [ConnectionRefusedError(111, "Connection refused"), ValueError("Expecting value")]
        api = FakeApi(GREEN_RUNS, GREEN_JOBS, errors={NIGHTLY_RUNS_PATH: errors})
        code, out = self.watch(api)
        self.assertExit(0, code, out, "nightly watch: green")
        self.assertEqual([5, 10], self.sleeps)

    def test_api_down_costs_one_backoff_then_one_try_per_request(self):
        self.workflows({"cbmc.yml": CBMC, "nightly.yml": NIGHTLY})
        api = FakeApi(everything=urllib.error.URLError(ConnectionRefusedError(111, "Connection refused")))
        code, out = self.watch(api)
        self.assertExit(1, code, out, "after 4 tries", "after 1 try (the API was down for the request before)",
                        "API-ERROR 2", "nightly watch: RED: could not read run state for 2 request(s)")
        self.assertEqual(5, len(api.calls))
        self.assertEqual([5, 10, 20], self.sleeps)

    def test_the_runner_api_root_is_used(self):
        os.environ["GITHUB_API_URL"] = "https://ghe.invalid/api/v3"
        seen = []

        def fetch(url):
            seen.append(url)
            raise http_error(500)
        code, out = self.watch(fetch)
        self.assertExit(1, code, out, "GET https://ghe.invalid/api/v3/repos/")
        self.assertTrue(all(u.startswith("https://ghe.invalid/api/v3/") for u in seen), seen)

    def test_an_answer_ends_the_down_state(self):
        self.workflows({"cbmc.yml": CBMC, "nightly.yml": NIGHTLY})
        api = FakeApi(GREEN_RUNS, GREEN_JOBS, broken={CBMC_RUNS_PATH: 99, NIGHTLY_RUNS_PATH: 1})
        code, out = self.watch(api)
        # cbmc runs out of tries; nightly's one try (API down) fails too; nothing answered in between
        self.assertExit(1, code, out, "API-ERROR 2")
        api = FakeApi(GREEN_RUNS, GREEN_JOBS, broken={CBMC_RUNS_PATH: 4})
        code, out = self.watch(api)
        # cbmc's four tries all fail; nightly answers on its one try and the jobs GET gets full tries again
        self.assertExit(1, code, out, "API-ERROR 1", "RUN nightly.yml run=101")


class Selection(WatchCase):
    def test_selects_by_schedule_content_not_name(self):
        self.workflows({"cbmc.yml": CBMC, "nightly.yml": NIGHTLY, "ci.yml": CI,
                        "nightly-watch.yml": WATCH, "tlc-debt.yml": WEEKLY})
        code, out = self.watch(FakeApi(), "--list")
        self.assertExit(0, code, out)
        self.assertEqual(["cbmc.yml", "nightly.yml", "tlc-debt.yml"], out.split())

    def test_zero_scheduled_workflows_is_an_error(self):
        self.workflows({"ci.yml": CI, "nightly-watch.yml": WATCH})
        code, out = self.watch(FakeApi())
        self.assertExit(2, code, out, "NO-WORKFLOWS")

    def test_no_workflows_directory_is_an_error(self):
        self.workflows({})
        os.rmdir(os.path.join(self.root, ".github", "workflows"))
        for listing in ((), ("--list",)):
            code, out = self.watch(FakeApi(), *listing)
            self.assertExit(2, code, out, "NO-WORKFLOWS")

    def test_schedule_without_readable_cron_is_red(self):
        self.workflows({"odd.yml": "on:\n  schedule: [{cron: '0 4 * * *'}]\n"})
        code, out = self.watch(FakeApi())
        self.assertExit(1, code, out, "NO-VERDICT odd.yml :: (run) has `schedule:` but no cron line")

    def test_unquoted_and_commented_cron_lines_are_read(self):
        self.workflows({"plain.yml": "on:\n  schedule:\n    - cron: 0 4 * * 1   # Mondays\n    - cron: '5 6 * * *'\n"})
        (name, crons), = watch.scheduled_workflows(self.root)
        self.assertEqual(["0 4 * * 1", "5 6 * * *"], [c.text for c in crons])

    def test_the_repository_selection_has_the_four_the_old_watch_missed(self):
        root = str(Path(__file__).resolve().parent.parent)
        names = [name for name, _ in watch.scheduled_workflows(root)]
        for name in ("cbmc.yml", "msan.yml", "tlc.yml", "tlc-debt.yml", "nightly.yml"):
            self.assertIn(name, names)
        self.assertNotIn("nightly-watch.yml", names)
        self.assertNotIn("ci.yml", names)


class Dispositions(WatchCase):
    RED_JOBS = {101: [job("linux / unit (main)", "failure"), job("macos / unit (main)")],
                100: [job("linux / unit (main)"), job("macos / unit (main)")]}

    def test_owned_by_open_pr_recent_row_is_green(self):
        self.rows("nightly.yml\tlinux / unit\t#12\t2026-11-10\tknown cause\n")
        api = FakeApi(GREEN_RUNS, self.RED_JOBS, pulls={12: {"state": "open"}})
        code, out = self.watch(api)
        self.assertExit(0, code, out, "OWNED nightly.yml :: linux / unit (main) (failure) by #12")

    def test_owned_by_name_is_green(self):
        self.rows("nightly.yml\tlinux / unit\tglenn\t2026-11-12\tknown cause\n")
        code, out = self.watch(FakeApi(GREEN_RUNS, self.RED_JOBS))
        self.assertExit(0, code, out, "OWNED nightly.yml :: linux / unit (main) (failure) by glenn")

    def test_row_can_own_a_cancelled_job(self):
        self.rows("nightly.yml\tlinux / unit\tglenn\t2026-11-12\tcancelled by a dispatch\n")
        jobs = {101: [job("linux / unit (main)", "cancelled"), job("macos / unit (main)")]}
        code, out = self.watch(FakeApi(GREEN_RUNS, jobs))
        self.assertExit(0, code, out, "OWNED nightly.yml :: linux / unit (main) (cancelled) by glenn")

    def test_old_row_still_red_is_stale(self):
        self.rows("nightly.yml\tlinux / unit\tglenn\t2026-10-20\tknown cause\n")
        code, out = self.watch(FakeApi(GREEN_RUNS, self.RED_JOBS))
        self.assertExit(1, code, out, "STALE tools/nightly_dispositions.txt:2: row dated 2026-10-20 is 23 days old "
                        "(limit 14)")

    def test_merged_pr_still_red_is_stale(self):
        self.rows("nightly.yml\tlinux / unit\t#12\t2026-11-10\tknown cause\n")
        api = FakeApi(GREEN_RUNS, self.RED_JOBS, pulls={12: {"state": "closed", "merged_at": "x"}})
        code, out = self.watch(api)
        self.assertExit(1, code, out, "STALE tools/nightly_dispositions.txt:2: #12 is closed (merged)")

    def test_row_for_green_job_is_stale(self):
        self.rows("nightly.yml\tlinux / unit\tglenn\t2026-11-12\tknown cause\n")
        code, out = self.watch(FakeApi(GREEN_RUNS, GREEN_JOBS))
        self.assertExit(1, code, out, "STALE tools/nightly_dispositions.txt:2: nightly.yml :: linux / unit is not red")

    def test_row_for_unscheduled_workflow_is_stale(self):
        self.rows("gone.yml\tlinux\tglenn\t2026-11-12\tknown cause\n")
        code, out = self.watch(FakeApi(GREEN_RUNS, GREEN_JOBS))
        self.assertExit(1, code, out, "STALE tools/nightly_dispositions.txt:2: gone.yml is not a scheduled workflow")

    def test_expected_skip_makes_a_skip_green(self):
        self.rows("nightly.yml\tmacos / unit\tglenn\t2026-11-12\texpected-skip: no macOS runner on this ref\n")
        jobs = dict(GREEN_JOBS)
        jobs[101] = [job("linux / unit (main)"), job("macos / unit (main)", "skipped")]
        code, out = self.watch(FakeApi(GREEN_RUNS, jobs))
        self.assertExit(0, code, out, "SKIP-EXPECTED nightly.yml :: macos / unit (main)")

    def test_owner_needed_is_a_bad_row(self):
        self.rows("nightly.yml\tlinux / unit\towner-needed\t2026-11-12\tknown cause\n")
        code, out = self.watch(FakeApi(GREEN_RUNS, self.RED_JOBS))
        self.assertExit(2, code, out, "BAD-ROW")

    def test_old_four_field_row_is_a_bad_row(self):
        self.rows("nightly.yml\tlinux / unit\t#12\tknown cause\n")
        code, out = self.watch(FakeApi(GREEN_RUNS, self.RED_JOBS))
        self.assertExit(2, code, out, "BAD-ROW")

    def test_future_date_is_a_bad_row(self):
        self.rows("nightly.yml\tlinux / unit\tglenn\t2026-12-01\tknown cause\n")
        code, out = self.watch(FakeApi(GREEN_RUNS, self.RED_JOBS))
        self.assertExit(2, code, out, "BAD-ROW")

    def test_repository_dispositions_file_parses(self):
        root = str(Path(__file__).resolve().parent.parent)
        rows = watch.read_dispositions(os.path.join(root, "tools", "nightly_dispositions.txt"),
                                       datetime.date(2099, 1, 1), io.StringIO())
        self.assertIsNotNone(rows)


BUDGET = """name: frame budget
on:
  schedule:
    - cron: "40 8 * * *"
jobs:
  macos-frame-budget:
    name: macos frame budget
    env:
      ROCKETNET_MAY_NOT_MEASURE: 'report-only; a loaded host voids the run'
  linux-frame-budget:
    name: linux frame budget
  windows-frame-budget:
    name: windows frame budget
    env:
      ROCKETNET_REPORT_ONLY: 'the Windows runner carries other work'
"""
MAC = "macos frame budget"
LINUX = "linux frame budget"
WIN = "windows frame budget"
FAIL_LINE = ("frame_budget transport=portable P=32 total_us_p99_median=180.25 runs=180.25,181.00,179.50 "
             "gate=100 verdict=FAIL")
REPORT_FAIL = [{"title": "REPORT-ONLY-FAIL", "message": FAIL_LINE}]
VOID = [{"title": "UNMEASURED", "message": "VOID: host load 6.50 over the 4.00 gate; nothing measured"}]


def nights(count):
    """`count` nightly runs of nightly-frame-budget.yml, newest first: ids 101, 100, ...,
    created 08:45Z on 2026-11-12, 11-11, ...; each has the two jobs, job id = run id * 10 (+1)."""
    runs = [run(101 - n, (NOW - datetime.timedelta(days=n)).strftime("%Y-%m-%dT08:45:00Z")) for n in range(count)]
    jobs = {r["id"]: [job(MAC, job_id=r["id"] * 10), job(LINUX, job_id=r["id"] * 10 + 1),
                       job(WIN, job_id=r["id"] * 10 + 2)] for r in runs}
    return {"nightly-frame-budget.yml": runs}, jobs


class Unmeasured(WatchCase):
    """A job that declares ROCKETNET_MAY_NOT_MEASURE and ends green without measuring
    (frame budget VOID on a loaded host, low-tick DISCARDED) is never counted green."""

    def setUp(self):
        super().setUp()
        self.workflows({"nightly-frame-budget.yml": BUDGET, "nightly-watch.yml": WATCH})

    def test_derived_from_the_job_env(self):
        found = watch.may_not_measure(self.root, "nightly-frame-budget.yml")
        self.assertEqual(1, len(found), found)
        regex, why = found[0]
        self.assertRegex(MAC, "^" + regex + "$")
        self.assertNotRegex(LINUX, "^" + regex + "$")
        self.assertEqual("report-only; a loaded host voids the run", why)

    def test_void_tonight_after_measured_nights_is_unmeasured_not_green(self):
        runs, jobs = nights(7)
        code, out = self.watch(FakeApi(runs, jobs, notes={1010: VOID}))
        self.assertExit(0, code, out,
                        "MEASURED nightly-frame-budget.yml :: macos frame budget: measured 6 of the last 7",
                        "UNMEASURED nightly-frame-budget.yml :: macos frame budget: VOID: host load 6.50",
                        "UNMEASURED 1, NO-MEASUREMENT 0", "nightly watch: UNMEASURED: 0 ")
        self.assertIn(", 0 green,", out)
        self.assertNotIn("nightly watch: green", out)

    def test_void_every_night_for_seven_nights_is_red(self):
        runs, jobs = nights(7)
        notes = {r["id"] * 10: VOID for r in runs["nightly-frame-budget.yml"]}
        code, out = self.watch(FakeApi(runs, jobs, notes=notes))
        self.assertExit(1, code, out, "measured 0 of the last 7 scheduled nights (window 7)",
                        "NO-MEASUREMENT nightly-frame-budget.yml :: macos frame budget: no measurement in 7 nights; "
                        "tonight: VOID: host load 6.50", "nightly watch: RED: 1 ")

    def test_void_every_night_but_fewer_than_seven_runs_is_not_yet_red(self):
        runs, jobs = nights(6)
        notes = {r["id"] * 10: VOID for r in runs["nightly-frame-budget.yml"]}
        code, out = self.watch(FakeApi(runs, jobs, notes=notes))
        self.assertExit(0, code, out, "measured 0 of the last 6 scheduled nights (window 7)",
                        "UNMEASURED nightly-frame-budget.yml :: macos frame budget")

    def test_cancelled_earlier_night_does_not_count_as_measured(self):
        runs, jobs = nights(7)
        notes = {r["id"] * 10: VOID for r in runs["nightly-frame-budget.yml"][:-1]}
        jobs[95][0]["conclusion"] = "cancelled"
        code, out = self.watch(FakeApi(runs, jobs, notes=notes))
        self.assertExit(1, code, out, "measured 0 of the last 7", "NO-MEASUREMENT nightly-frame-budget.yml")

    def test_measured_tonight_prints_the_history_and_is_green(self):
        runs, jobs = nights(7)
        notes = {r["id"] * 10: VOID for r in runs["nightly-frame-budget.yml"][1:]}
        code, out = self.watch(FakeApi(runs, jobs, notes=notes))
        self.assertExit(0, code, out, "MEASURED nightly-frame-budget.yml :: macos frame budget: measured 1 of the "
                        "last 7 scheduled nights (window 7); may not measure because: report-only",
                        "nightly watch: green: 0 ", "UNMEASURED 0, NO-MEASUREMENT 0")

    def test_gated_job_failing_with_a_measurement_is_new_red(self):
        runs, jobs = nights(7)
        jobs[101][0]["conclusion"] = "failure"
        code, out = self.watch(FakeApi(runs, jobs))
        self.assertExit(1, code, out, "NEW-RED nightly-frame-budget.yml :: macos frame budget")

    def test_ungated_job_is_not_asked_for_annotations(self):
        runs, jobs = nights(2)
        api = FakeApi(runs, jobs)
        code, out = self.watch(api)
        self.assertExit(0, code, out, "nightly watch: green: 0 ")
        self.assertNotIn(f"/repos/{REPO}/check-runs/1011/annotations", api.calls)
        self.assertIn(f"/repos/{REPO}/check-runs/1010/annotations", api.calls)

    def test_annotations_unreadable_is_api_error(self):
        runs, jobs = nights(7)
        broken = {f"/repos/{REPO}/check-runs/1010/annotations": 99}
        code, out = self.watch(FakeApi(runs, jobs, broken=broken))
        self.assertExit(1, code, out, "API-ERROR could not read run state: GET ", "check-runs/1010/annotations")

    def test_replay_of_a_void_output_every_night_is_red(self):
        # the planted-violations row nightly-watch/void-every-night, without the frame-budget run
        output = ("VOID load=16.00 cores=16\n"
                  "::warning title=UNMEASURED::frame budget VOID: host load 16.00 over 4; nothing measured\n")
        self.assertEqual([{"annotation_level": "warning", "title": "UNMEASURED",
                           "message": "frame budget VOID: host load 16.00 over 4; nothing measured"}],
                         nightly_replay.annotations(output))
        recorded = nightly_replay.replay(self.root, REPO, "nightly-frame-budget.yml", MAC, 7, "success", output, NOW)
        replay_file = os.path.join(self.root, "replay.json")
        with open(replay_file, "w", encoding="utf-8") as f:
            json.dump(recorded, f)
        out = io.StringIO()
        code = watch.main(["--root", self.root, "--replay", replay_file], now=NOW, sleep=lambda s: None,
                          since=lambda name: OLD, out=out)
        self.assertExit(1, code, out.getvalue(), "measured 0 of the last 7",
                        "NO-MEASUREMENT nightly-frame-budget.yml :: macos frame budget: no measurement in 7 nights; "
                        "tonight: frame budget VOID: host load 16.00")

    def test_report_only_fail_is_red(self):
        runs, jobs = nights(2)
        code, out = self.watch(FakeApi(runs, jobs, notes={1012: REPORT_FAIL}))
        self.assertExit(1, code, out, "REPORT-FAIL nightly-frame-budget.yml :: windows frame budget: " + FAIL_LINE +
                        " (report-only, so the job exited 0: the Windows runner carries other work)",
                        "REPORT-FAIL 1", "nightly watch: RED: 1 ")

    def test_report_only_pass_is_green(self):
        runs, jobs = nights(2)
        api = FakeApi(runs, jobs)
        code, out = self.watch(api)
        self.assertExit(0, code, out, "nightly watch: green: 0 ", "REPORT-FAIL 0")
        self.assertIn(f"/repos/{REPO}/check-runs/1012/annotations", api.calls)

    def test_report_only_fail_owned_by_a_row_is_owned(self):
        self.rows("nightly-frame-budget.yml\twindows frame budget\tglenn\t2026-11-12\tWindows runner contention\n")
        runs, jobs = nights(2)
        code, out = self.watch(FakeApi(runs, jobs, notes={1012: REPORT_FAIL}))
        self.assertExit(0, code, out, "OWNED nightly-frame-budget.yml :: windows frame budget (report-only FAIL: "
                        + FAIL_LINE + ") by glenn")

    def test_replay_of_a_report_only_fail_is_red(self):
        # the planted-violations row nightly-watch/report-only-fail, without the frame-budget run
        output = FAIL_LINE + "\n::warning title=REPORT-ONLY-FAIL::" + FAIL_LINE + "\n"
        recorded = nightly_replay.replay(self.root, REPO, "nightly-frame-budget.yml", WIN, 1, "success", output, NOW)
        replay_file = os.path.join(self.root, "replay.json")
        with open(replay_file, "w", encoding="utf-8") as f:
            json.dump(recorded, f)
        out = io.StringIO()
        code = watch.main(["--root", self.root, "--replay", replay_file], now=NOW, sleep=lambda s: None,
                          since=lambda name: OLD, out=out)
        self.assertExit(1, code, out.getvalue(), "REPORT-FAIL nightly-frame-budget.yml :: windows frame budget: "
                        + FAIL_LINE)

    def test_every_gated_job_in_the_repository_names_a_reason(self):
        root = str(Path(__file__).resolve().parent.parent)
        total = 0
        for name, _ in watch.scheduled_workflows(root):
            for regex, why in watch.may_not_measure(root, name):
                self.assertTrue(why.strip(), (name, regex))
                total += 1
        self.assertGreater(total, 0, "no workflow declares ROCKETNET_MAY_NOT_MEASURE")
        reports = [why for name, _ in watch.scheduled_workflows(root)
                   for _, why in watch.may_not_measure(root, name, watch.REPORT_ONLY)]
        self.assertEqual(2, len(reports), "the macOS and Windows frame-budget legs are report-only")
        self.assertTrue(all(why.strip() for why in reports), reports)


class CronParser(unittest.TestCase):
    def t(self, text):
        return datetime.datetime.fromisoformat(text).replace(tzinfo=UTC)

    def test_daily(self):
        cron = watch.Cron("40 8 * * *")
        self.assertEqual(self.t("2026-11-12T08:40"), cron.last_at_or_before(self.t("2026-11-12T10:00")))
        self.assertEqual(self.t("2026-11-11T08:40"), cron.last_at_or_before(self.t("2026-11-12T08:39")))
        self.assertEqual(self.t("2026-11-13T08:40"), cron.first_after(self.t("2026-11-12T08:40")))

    def test_weekly_saturday(self):
        cron = watch.Cron("0 6 * * 6")
        self.assertEqual(self.t("2026-11-07T06:00"), cron.last_at_or_before(self.t("2026-11-12T14:00")))
        self.assertEqual(self.t("2026-11-14T06:00"), cron.first_after(self.t("2026-11-12T14:00")))

    def test_sunday_is_zero_and_seven(self):
        self.assertEqual(watch.Cron("0 0 * * 0").weekdays, watch.Cron("0 0 * * 7").weekdays)

    def test_steps_and_lists(self):
        cron = watch.Cron("*/15 1,13 * * *")
        self.assertEqual([0, 15, 30, 45], cron.minutes)
        self.assertEqual([1, 13], cron.hours)
        self.assertEqual(self.t("2026-11-12T13:45"), cron.last_at_or_before(self.t("2026-11-12T14:00")))

    def test_every_other_day_of_month(self):
        cron = watch.Cron("0 12 */2 * *")
        self.assertEqual(self.t("2026-11-11T12:00"), cron.last_at_or_before(self.t("2026-11-12T14:00")))
        self.assertEqual(self.t("2026-11-13T12:00"), cron.first_after(self.t("2026-11-12T14:00")))

    def test_ranges(self):
        cron = watch.Cron("0 9 * * 1-5")
        self.assertEqual(self.t("2026-11-13T09:00"), cron.first_after(self.t("2026-11-12T14:00")))
        self.assertEqual(self.t("2026-11-16T09:00"), cron.first_after(self.t("2026-11-13T14:00")))

    def test_both_day_fields_restricted_either_matches(self):
        cron = watch.Cron("0 0 1 * 1")  # the 1st, or any Monday
        self.assertEqual(self.t("2026-11-09T00:00"), cron.last_at_or_before(self.t("2026-11-12T00:00")))

    def test_bad_lines_refused(self):
        for text in ("0 4 * *", "61 4 * * *", "0 4 * * 8", "*/0 4 * * *"):
            with self.assertRaises(ValueError, msg=text):
                watch.Cron(text)

    def test_every_cron_in_the_repository_parses(self):
        root = str(Path(__file__).resolve().parent.parent)
        for name, crons in watch.scheduled_workflows(root):
            self.assertTrue(crons, name)


if __name__ == "__main__":
    unittest.main()
