#!/usr/bin/env python3
"""Check that a release commit has green runs for its required nightly gates.

docs/RELEASING.md requires four nightly-tier gates to be green on the release
commit before a v* tag is pushed:
  - `tsan (threaded transport)` from .github/workflows/nightly.yml
  - `Nightly CI` (.github/workflows/nightly-ci.yml, ci.yml with full: true)
  - `CBMC` (.github/workflows/cbmc.yml, `cbmc (arithmetic leaf proofs)`)
  - `TLC records` (.github/workflows/tlc.yml, `Model records are current`)
(.github/workflows/msan.yml is advisory: its job is continue-on-error.)

Release Check runs this script on a v* tag push. It asks the GitHub Actions API
for the workflow runs whose head commit is the tagged commit:
  - For nightly.yml, it inspects the jobs of each run and requires the job
    `tsan (threaded transport) (<ref>)` concluded `success`, where <ref> is the
    run's own head branch: that leg checks out the run's head commit, so its
    verdict is about the tagged commit. A leg for another ref in the same run
    tested another commit and does not count.
  - For nightly-ci.yml, cbmc.yml, and tlc.yml, it requires a workflow run on the
    tagged commit concluded `success`.

Environment: GITHUB_REPOSITORY (owner/name), GITHUB_SHA (the tagged commit),
GITHUB_TOKEN (needs actions: read), GITHUB_API_URL (defaults to
https://api.github.com).

Exit 0 when every required gate is green on the tagged commit, 1 when any gate
is missing or not green (the runs and legs found are printed), 2 when the
environment is incomplete or the API answers with an error.
"""

import json
import os
import sys
import urllib.error
import urllib.request

WORKFLOW_FILE = "nightly.yml"
JOB_NAME = "tsan (threaded transport)"
REQUIRED_WORKFLOWS = (
    ("nightly-ci.yml", "Nightly CI"),
    ("cbmc.yml", "CBMC"),
    ("tlc.yml", "TLC records"),
)


class ApiError(Exception):
    pass


def http_fetch(api, token):
    """Return a fetch(path) that GETs path from the API and decodes the JSON."""

    def fetch(path):
        request = urllib.request.Request(
            api.rstrip("/") + path,
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": "Bearer " + token,
                "X-GitHub-Api-Version": "2022-11-28",
            },
        )
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                return json.load(response)
        except (urllib.error.URLError, ValueError) as error:
            raise ApiError(f"GET {path}: {error}") from error

    return fetch


def paged(fetch, path, key):
    """Every item under key across the pages of a list endpoint."""
    items = []
    page = 1
    while True:
        separator = "&" if "?" in path else "?"
        answer = fetch(f"{path}{separator}per_page=100&page={page}")
        batch = answer.get(key)
        if not isinstance(batch, list):
            raise ApiError(f"GET {path}: no '{key}' list in the answer")
        items.extend(batch)
        if len(batch) < 100:
            return items
        page += 1


def check_threaded_tsan(fetch, repository, sha, out):
    """Return True when a green threaded TSan leg exists for sha."""
    runs = paged(
        fetch,
        f"/repos/{repository}/actions/workflows/{WORKFLOW_FILE}/runs?head_sha={sha}",
        "workflow_runs",
    )
    seen = []
    for run in runs:
        if run.get("head_sha") != sha:
            continue
        leg = f"{JOB_NAME} ({run.get('head_branch')})"
        jobs = paged(fetch, f"/repos/{repository}/actions/runs/{run['id']}/jobs", "jobs")
        for job in jobs:
            name = job.get("name", "")
            if not name.startswith(JOB_NAME):
                continue
            conclusion = job.get("conclusion") or job.get("status")
            seen.append(f"run {run['id']} ({run.get('event')}, {run.get('head_branch')}): {name}: {conclusion}")
            if name == leg and job.get("conclusion") == "success":
                print(f"ok: {leg} succeeded on {sha} in run {run['id']} ({run.get('html_url', '')})", file=out)
                return True
    print(
        f"::error::No successful '{JOB_NAME}' run exists for {sha}. Dispatch nightly.yml on the "
        f"release commit (docs/RELEASING.md) and tag only after that job is green.",
        file=out,
    )
    if seen:
        print("Threaded TSan legs found for this commit:", file=out)
        for line in seen:
            print("  " + line, file=out)
    else:
        print(f"No {WORKFLOW_FILE} run has this commit as its head.", file=out)
    return False


def check_workflow_run(fetch, repository, sha, workflow_file, label, out):
    """Return True when workflow_file has a successful run on sha."""
    runs = paged(
        fetch,
        f"/repos/{repository}/actions/workflows/{workflow_file}/runs?head_sha={sha}",
        "workflow_runs",
    )
    seen = []
    for run in runs:
        if run.get("head_sha") != sha:
            continue
        conclusion = run.get("conclusion") or run.get("status")
        seen.append(f"run {run['id']} ({run.get('event')}, {run.get('head_branch')}): {conclusion}")
        if run.get("conclusion") == "success":
            print(
                f"ok: {workflow_file} ({label}) succeeded on {sha} in run {run['id']} ({run.get('html_url', '')})",
                file=out,
            )
            return True
    print(
        f"::error::No successful '{workflow_file}' ({label}) run exists for {sha}. Dispatch {workflow_file} "
        f"on the release commit (docs/RELEASING.md) and tag only after it is green.",
        file=out,
    )
    if seen:
        print(f"Runs found for {workflow_file} on this commit:", file=out)
        for line in seen:
            print("  " + line, file=out)
    else:
        print(f"No {workflow_file} run has this commit as its head.", file=out)
    return False


def check(fetch, repository, sha, out=sys.stdout):
    """Return 0 when every required release workflow gate is green for sha, else 1."""
    ok = check_threaded_tsan(fetch, repository, sha, out)
    for workflow_file, label in REQUIRED_WORKFLOWS:
        if not check_workflow_run(fetch, repository, sha, workflow_file, label, out):
            ok = False
    return 0 if ok else 1


def main():
    repository = os.environ.get("GITHUB_REPOSITORY", "")
    sha = os.environ.get("GITHUB_SHA", "")
    token = os.environ.get("GITHUB_TOKEN", "")
    api = os.environ.get("GITHUB_API_URL", "https://api.github.com")
    if not repository or not sha or not token:
        print("::error::GITHUB_REPOSITORY, GITHUB_SHA and GITHUB_TOKEN must all be set.")
        return 2
    try:
        return check(http_fetch(api, token), repository, sha)
    except ApiError as error:
        print(f"::error::The Actions API did not answer: {error}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
