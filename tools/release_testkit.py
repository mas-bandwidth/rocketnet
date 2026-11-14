#!/usr/bin/env python3
"""Fixtures for the release and landing tools' tests: a fake GitHub API, no network.

FakeApi answers the GET, POST and PUT calls tools/release_gates.py,
tools/release.py, tools/land.py and tools/check_release_tsan.py make, from
tables a test fills (runs, jobs, commits, ancestry, files, tags, pull
requests), and records every write in `writes`. Hooks let a test say what a
write does (a tag push starts a Release Check run; a re-run finishes).
"""

import base64
import datetime
import json
import re
import urllib.parse

import release_gates as rg

REPO = "mas-bandwidth/rocketnet"
SHA = "a" * 40
OTHER = "b" * 40
MAIN_SHA = "c" * 40
ANCESTOR = "d" * 40
NOW = datetime.datetime(2026, 11, 20, 12, 0, 0, tzinfo=datetime.timezone.utc)
VERSION = "v1.2.0"  # a minor release: every tier gates it

CI_JOBS = ["linux / Debug / unit", "lint (workflows, format, standard, tool selftests)"]
CI_SKIPPED = ["unit-case coverage", "backend example / ${{ matrix.backend }}"]
STATIC_JOBS = ["static analysis (gcc -fanalyzer)"]
DOCS_JOBS = ["docs (MrDocs HTML reference build)"]
RELEASE_CHECK_JOBS = ["version consistency", "unreleased wire changes"]
RELEASE_CHECK_SKIPPED = ["threaded tsan run on the release commit", "release gates passed on the tag"]
REQUIRED = CI_JOBS + STATIC_JOBS + DOCS_JOBS + RELEASE_CHECK_JOBS


def iso(dt):
    return dt.strftime("%Y-%m-%dT%H:%M:%SZ")


def job(name, conclusion="success", status="completed", secs=60, start=None):
    start = start or NOW - datetime.timedelta(hours=2)
    return {"name": name, "status": status, "conclusion": conclusion if status == "completed" else None,
            "started_at": iso(start), "completed_at": iso(start + datetime.timedelta(seconds=secs))}


def header(version):
    return (f'#if !ROCKETNET_DEV\n    #define ROCKETNET_VERSION_FULL "{version}"\n#else // !ROCKETNET_DEV\n'
            f'    #define ROCKETNET_VERSION_FULL "dev"\n#endif // !ROCKETNET_DEV\n')


def ruleset(names):
    return json.dumps({"rules": [{"type": "required_status_checks", "parameters": {
        "strict_required_status_checks_policy": True,
        "required_status_checks": [{"context": n} for n in names]}}]})


class FakeApi:

    def __init__(self):
        self.runs = []
        self.jobs = {}
        self.commits = {"main": MAIN_SHA, SHA: SHA, SHA[:9]: SHA, MAIN_SHA: MAIN_SHA}
        self.ancestors = {(SHA, MAIN_SHA), (ANCESTOR, SHA), (ANCESTOR, MAIN_SHA), (MAIN_SHA, SHA + "-pr")}
        self.files = {}
        self.tags = {}
        self.pulls = {}
        self.writes = []
        self.gets = []
        self.on_tag = None
        self.on_rerun = None
        self.next_id = 1000

    # ---- building
    def add_run(self, workflow, jobs, sha=SHA, event="push", branch="main", conclusion=None, status="completed",
                attempt=1, started=None, created=None):
        self.next_id += 1
        started = started or NOW - datetime.timedelta(hours=3) + datetime.timedelta(minutes=self.next_id - 1000)
        if conclusion is None and status == "completed":
            bad = [j for j in jobs if j["conclusion"] not in ("success", "skipped")]
            conclusion = "success" if not bad else bad[0]["conclusion"]
        run = {"id": self.next_id, "path": f".github/workflows/{workflow}", "head_sha": sha, "event": event,
               "head_branch": branch, "status": status, "conclusion": conclusion if status == "completed" else None,
               "run_attempt": attempt, "run_started_at": iso(started), "created_at": iso(created or started),
               "name": workflow}
        self.runs.append(run)
        self.jobs[run["id"]] = jobs
        return run

    # ---- answering
    def get(self, path, params=None):
        params = dict(params or {})
        self.gets.append((path, params))
        page = int(params.pop("page", 1))
        params.pop("per_page", None)
        m = re.fullmatch(rf"/repos/{REPO}/actions/workflows/([^/]+)/runs", path)
        if m:
            runs = [r for r in self.runs if r["path"].endswith("/" + m.group(1))]
            if "head_sha" in params:
                runs = [r for r in runs if r["head_sha"] == params["head_sha"]]
            if "event" in params:
                runs = [r for r in runs if r["event"] == params["event"]]
            if "created" in params:
                since = params["created"].lstrip(">=")
                runs = [r for r in runs if r["created_at"][:10] >= since]
            return {"workflow_runs": runs if page == 1 else []}
        m = re.fullmatch(rf"/repos/{REPO}/actions/runs/(\d+)/jobs", path)
        if m:
            if int(m.group(1)) not in self.jobs:
                raise rg.NotFound(path)
            return {"jobs": self.jobs[int(m.group(1))] if page == 1 else []}
        m = re.fullmatch(rf"/repos/{REPO}/actions/runs/(\d+)", path)
        if m:
            return next(r for r in self.runs if r["id"] == int(m.group(1)))
        m = re.fullmatch(rf"/repos/{REPO}/commits/(.+)", path)
        if m:
            if m.group(1) not in self.commits:
                raise rg.NotFound(path)
            return {"sha": self.commits[m.group(1)]}
        m = re.fullmatch(rf"/repos/{REPO}/compare/(.+)\.\.\.(.+)", path)
        if m:
            base, head = m.group(1), m.group(2)
            if base == head:
                return {"status": "identical", "behind_by": 0}
            if (base, head) in self.ancestors:
                return {"status": "ahead", "behind_by": 0}
            return {"status": "diverged", "behind_by": 3}
        m = re.fullmatch(rf"/repos/{REPO}/contents/(.+)", path)
        if m:
            key = (m.group(1), params.get("ref"))
            if key not in self.files:
                raise rg.NotFound(path)
            return {"encoding": "base64", "content": base64.b64encode(self.files[key].encode()).decode()}
        if path == f"/repos/{REPO}/git/matching-refs/tags/v":
            return [{"ref": f"refs/tags/{name}"} for name in sorted(self.tags) if name.startswith("v")]
        m = re.fullmatch(rf"/repos/{REPO}/git/ref/tags/(.+)", path)
        if m:
            if m.group(1) not in self.tags:
                raise rg.NotFound(path)
            return {"object": {"type": "commit", "sha": self.tags[m.group(1)]}}
        m = re.fullmatch(rf"/repos/{REPO}/pulls/(\d+)", path)
        if m:
            return self.pulls[int(m.group(1))]
        raise rg.NotFound(f"fake API has no answer for GET {path} {params}")

    def post(self, path, body):
        self.writes.append(("POST", path, body))
        if path.endswith("/git/tags"):
            return {"sha": "e" * 40}
        if path.endswith("/git/refs"):
            version = body["ref"].rsplit("/", 1)[1]
            self.tags[version] = SHA
            if self.on_tag:
                self.on_tag(self, version)
            return {}
        m = re.search(r"/actions/runs/(\d+)/rerun-failed-jobs$", path)
        if m:
            if self.on_rerun:
                self.on_rerun(self, int(m.group(1)))
            return {}
        if path.endswith("/releases"):
            return {"html_url": "https://example.invalid/release"}
        return {}

    def put(self, path, body):
        self.writes.append(("PUT", path, body))
        return {"sha": "f" * 40, "message": "merged"}


def green_release(api, version=VERSION, sha=SHA, branch_ruleset=True):
    """Every gate green for sha."""
    api.files[("sdk/rocketnet.h", sha)] = header(version)
    api.files[(f"docs/RELEASE-NOTES-{version}.md", sha)] = f"# rocketnet {version}\n"
    if branch_ruleset:
        api.files[(rg.RULESET_PATH, sha)] = ruleset(REQUIRED)
    api.add_run("ci.yml", [job(n) for n in CI_JOBS] + [job(n, "skipped", secs=0) for n in CI_SKIPPED], sha)
    api.add_run("static.yml", [job(n) for n in STATIC_JOBS], sha)
    api.add_run("docs.yml", [job(n) for n in DOCS_JOBS], sha)
    api.add_run("release-check.yml", [job(n) for n in RELEASE_CHECK_JOBS]
                + [job(n, "skipped", secs=0) for n in RELEASE_CHECK_SKIPPED], sha)
    api.add_run("nightly-ci.yml", [job("full / " + n) for n in CI_JOBS + CI_SKIPPED], sha, "workflow_dispatch")
    api.add_run("cbmc.yml", [job("cbmc (arithmetic leaf proofs)")], sha, "workflow_dispatch")
    api.add_run("tlc.yml", [job("Model records are current"), job("TLC re-run full")], sha, "workflow_dispatch")
    api.add_run("nightly.yml", [job("linux / clang"), job("tsan (threaded transport) (main)"),
                                job("linux / Release functional nightly tier (main)"),
                                job("macos / Release functional nightly tier (main)"),
                                job("windows / Release functional nightly tier (main)")], sha,
                "workflow_dispatch")
    api.add_run("msan.yml", [job("MemorySanitizer")], sha, "workflow_dispatch")
    api.add_run("nightly-soak.yml", [job("soak (main)"), job("soak per transport (main / 1800 s per cell)")], sha,
                "workflow_dispatch")
    api.add_run("nightly-leaks.yml", [job("asan + ubsan + lsan (full suite) (main)")], sha, "workflow_dispatch")
    api.add_run("nightly-macos-sanitizers.yml", [job("macOS leaks (main)"), job("macOS tsan (main)")], sha,
                "workflow_dispatch")
    api.add_run("nightly-channels-stress.yml", [job("channels-stress-linux (main)")], sha, "workflow_dispatch")
    return api


def green_pull_request(api, number=7, head=SHA + "-pr"):
    api.commits[head] = head
    api.files[(rg.RULESET_PATH, MAIN_SHA)] = ruleset(REQUIRED)
    api.pulls[number] = {"number": number, "title": "a change", "state": "open", "merged": False,
                         "head": {"sha": head, "ref": "topic"}, "base": {"ref": "main"}}
    runs = {
        "ci.yml": api.add_run("ci.yml", [job(n) for n in CI_JOBS] + [job(n, "skipped", secs=0) for n in CI_SKIPPED],
                              head, "pull_request", "topic"),
        "static.yml": api.add_run("static.yml", [job(n) for n in STATIC_JOBS], head, "pull_request", "topic"),
        "docs.yml": api.add_run("docs.yml", [job(n) for n in DOCS_JOBS], head, "pull_request", "topic"),
        "release-check.yml": api.add_run("release-check.yml", [job(n) for n in RELEASE_CHECK_JOBS]
                                         + [job(n, "skipped", secs=0) for n in RELEASE_CHECK_SKIPPED],
                                         head, "pull_request", "topic"),
    }
    return runs
