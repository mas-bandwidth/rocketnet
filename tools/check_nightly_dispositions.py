#!/usr/bin/env python3
"""The nightly watch: every red nightly job has a commit range, a bisect line
and an owner, or the watch fails.

For each scheduled nightly workflow (.github/workflows/nightly*.yml and
fuzz-nightly.yml) the watch reads the newest completed scheduled run on main
and its jobs. For every failed job it prints:

  RED <workflow> :: <job> bad=<sha> good=<sha> commits=<n>
  BISECT git bisect start <bad> <good>

where good is the commit of the newest earlier scheduled run in which a job of
the same name succeeded (or "none" when no green is within the window). Then
it reads tools/nightly_dispositions.txt, one owned red per line:

  <workflow file>\t<job name prefix>\t<owner: #PR>\t<cause>

and fails on:

  NEW-RED   a failed job no disposition names (nobody owns it yet)
  STALE     a disposition whose job is green, or whose PR is merged or closed
            while the job is still red (the fix did not hold)

So a known red cannot hide a new one, and the file only ever lists reds that
are red now and owned by an open pull request.

Reads GITHUB_TOKEN and GITHUB_REPOSITORY from the environment; makes only GET
requests to the GitHub REST API.
"""

import json
import os
import sys
import urllib.parse
import urllib.request

API = "https://api.github.com"
WINDOW = 30  # earlier scheduled runs searched for a green job


def get(path, params=None):
    url = API + path
    if params:
        url += "?" + urllib.parse.urlencode(params)
    request = urllib.request.Request(url)
    request.add_header("Accept", "application/vnd.github+json")
    request.add_header("Authorization", "Bearer " + os.environ["GITHUB_TOKEN"])
    request.add_header("X-GitHub-Api-Version", "2022-11-28")
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def nightly_workflows(root):
    directory = os.path.join(root, ".github", "workflows")
    names = []
    for name in sorted(os.listdir(directory)):
        if not (name.startswith("nightly") or name == "fuzz-nightly.yml"):
            continue
        if not name.endswith(".yml") or name == "nightly-watch.yml":
            continue
        with open(os.path.join(directory, name), encoding="utf-8") as f:
            if "schedule:" in f.read():
                names.append(name)
    return names


def scheduled_runs(repo, workflow, count):
    data = get(f"/repos/{repo}/actions/workflows/{workflow}/runs",
               {"event": "schedule", "branch": "main", "status": "completed", "per_page": count})
    return data.get("workflow_runs", [])


def run_jobs(repo, run_id):
    jobs = []
    page = 1
    while True:
        data = get(f"/repos/{repo}/actions/runs/{run_id}/jobs", {"per_page": 100, "page": page})
        jobs += data.get("jobs", [])
        if len(jobs) >= data.get("total_count", 0) or not data.get("jobs"):
            return jobs
        page += 1


def read_dispositions(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) != 4 or not fields[2].startswith("#") or not fields[2][1:].isdigit() or not fields[3]:
                print(f"BAD-ROW {path}:{number}: want <workflow>\\t<job prefix>\\t#<PR>\\t<cause>")
                sys.exit(2)
            rows.append({"workflow": fields[0], "job": fields[1], "pr": int(fields[2][1:]),
                         "cause": fields[3], "line": number, "used": False})
    return rows


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    repo = os.environ["GITHUB_REPOSITORY"]
    rows = read_dispositions(os.path.join(root, "tools", "nightly_dispositions.txt"))
    failures = 0

    for workflow in nightly_workflows(root):
        runs = scheduled_runs(repo, workflow, WINDOW + 1)
        if not runs:
            print(f"NO-RUN {workflow}: no completed scheduled run on main")
            continue
        newest = runs[0]
        jobs = run_jobs(repo, newest["id"])
        print(f"RUN {workflow} run={newest['id']} sha={newest['head_sha'][:12]} conclusion={newest['conclusion']}")
        red = [job for job in jobs if job["conclusion"] == "failure"]
        earlier = None
        for job in red:
            if earlier is None:
                earlier = [(run, run_jobs(repo, run["id"])) for run in runs[1:]]
            good = next((run["head_sha"] for run, run_jobs_list in earlier
                         if any(j["name"] == job["name"] and j["conclusion"] == "success" for j in run_jobs_list)), None)
            bad = newest["head_sha"]
            if good:
                compare = get(f"/repos/{repo}/compare/{good}...{bad}")
                print(f"RED {workflow} :: {job['name']} bad={bad[:12]} good={good[:12]} commits={compare.get('ahead_by', '?')}")
                print(f"BISECT git bisect start {bad[:12]} {good[:12]}")
            else:
                print(f"RED {workflow} :: {job['name']} bad={bad[:12]} good=none (no green in the last {WINDOW} scheduled runs)")
            owner = next((row for row in rows if row["workflow"] == workflow and job["name"].startswith(row["job"])), None)
            if owner is None:
                print(f"NEW-RED {workflow} :: {job['name']}: no disposition in tools/nightly_dispositions.txt")
                failures += 1
                continue
            owner["used"] = True
            pr = get(f"/repos/{repo}/pulls/{owner['pr']}")
            if pr.get("state") != "open":
                print(f"STALE tools/nightly_dispositions.txt:{owner['line']}: #{owner['pr']} is {pr.get('state')}"
                      f"{' (merged)' if pr.get('merged_at') else ''} and {workflow} :: {job['name']} is still red")
                failures += 1
            else:
                print(f"OWNED {workflow} :: {job['name']} by #{owner['pr']}: {owner['cause']}")

    for row in rows:
        if not row["used"]:
            print(f"STALE tools/nightly_dispositions.txt:{row['line']}: {row['workflow']} :: {row['job']} is not red; remove the row")
            failures += 1

    print(f"nightly watch: {failures} unowned or stale")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
