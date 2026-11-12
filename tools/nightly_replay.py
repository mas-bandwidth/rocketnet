#!/usr/bin/env python3
"""Write a recorded GitHub API for the nightly watch (check_nightly_dispositions.py --replay):
the last N scheduled nights of one workflow, each with one job, named --job, that printed
the output read on stdin. The `::warning|error|notice title=T::message` lines in that output
become the job's check-run annotations, as GitHub makes them.

It exists for tools/planted_violations.tsv: the row nightly-watch/void-every-night runs the
real frame-budget check with the host load planted high, replays its VOID output as every
night of the week, and requires the nightly watch's Tell step to go red (NO-MEASUREMENT).

  python3 tools/nightly_replay.py --workflow nightly-frame-budget.yml \\
      --job 'frame budget (main / macos msgx report-only)' --nights 7 < void.log > replay.json

Each night's run is created one minute after that night's cron firing (the file's own
first `cron:` line) and concludes `success`; its one job concludes --conclusion.
"""

import argparse
import datetime
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_nightly_dispositions as watch  # noqa: E402

COMMAND = re.compile(r"^::(warning|error|notice)(?: ([^:]*))?::(.*)$")


def annotations(text):
    """The annotations GitHub makes from the workflow commands in a step's output."""
    notes = []
    for line in text.splitlines():
        match = COMMAND.match(line.strip())
        if not match:
            continue
        level, params, message = match.groups()
        fields = {}
        for part in (params or "").split(","):
            if "=" in part:
                key, value = part.split("=", 1)
                fields[key.strip()] = value.strip()
        notes.append({"annotation_level": {"error": "failure"}.get(level, level),
                      "title": fields.get("title", ""), "message": message})
    return notes


def replay(root, repo, workflow, job, nights, conclusion, output, now):
    crons = dict(watch.scheduled_workflows(root)).get(workflow)
    if not crons:
        raise SystemExit(f"nightly_replay: {workflow} is not a scheduled workflow under {root}")
    notes = annotations(output)
    recorded = {}
    runs = []
    moment = now
    for night in range(nights):
        fired = crons[0].last_at_or_before(moment)
        moment = fired - datetime.timedelta(minutes=1)
        run_id = 9000 + nights - night
        job_id = 90000 + nights - night
        runs.append({"id": run_id, "status": "completed", "conclusion": "success", "head_sha": "0" * 40,
                     "created_at": (fired + datetime.timedelta(minutes=1)).strftime("%Y-%m-%dT%H:%M:%SZ")})
        recorded[f"/repos/{repo}/actions/runs/{run_id}/jobs"] = {
            "total_count": 1,
            "jobs": [{"id": job_id, "name": job, "status": "completed", "conclusion": conclusion}]}
        recorded[f"/repos/{repo}/check-runs/{job_id}/annotations"] = notes
    recorded[f"/repos/{repo}/actions/workflows/{workflow}/runs"] = {"total_count": len(runs), "workflow_runs": runs}
    return recorded


def main(argv=None):
    parser = argparse.ArgumentParser(description="Write a recorded API of N nights for the nightly watch.")
    parser.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    parser.add_argument("--workflow", required=True)
    parser.add_argument("--job", required=True, help="the job's display name, as GitHub prints it")
    parser.add_argument("--nights", type=int, default=watch.MEASURE_NIGHTS)
    parser.add_argument("--conclusion", default="success")
    args = parser.parse_args(argv)
    repo = os.environ.get("GITHUB_REPOSITORY", "mas-bandwidth/rocketnet")
    recorded = replay(args.root, repo, args.workflow, args.job, args.nights, args.conclusion,
                      sys.stdin.read(), datetime.datetime.now(watch.UTC))
    json.dump(recorded, sys.stdout, indent=1, sort_keys=True)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
