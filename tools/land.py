#!/usr/bin/env python3
"""Land a pull request into main only when every per-commit check on its exact head is green.

  python3 tools/land.py <pr number> [--dry-run] [--repo owner/name]

A per-commit job that runs past its two-minute timeout-minutes concludes
`cancelled`, not `failure`, so a red mark never appears and a merge that only
looks for failures goes ahead without a verdict. This tool merges only when:

  - the pull request is open, into main, and its head is up to date with
    main's current head (main is an ancestor of the head: strict);
  - for every `pull_request` gate in tools/release_gates.txt, on exactly the
    head commit, the newest completed run (its newest attempt) has every job
    `success`, except jobs a `skip` row names (skipped by their own `if:`);
    cancelled, timed out, skipped without a row, missing, queued, in progress
    and failure all refuse, by name;
  - every required job name in .github/rulesets/main.json (main's) appears.

One re-run is allowed, done by the tool and recorded: when the only red jobs
are `cancelled` after running at least the cap minus a few seconds (the
two-minute cap fired; a job cancelled sooner was superseded or cancelled by
hand and refuses) on a run's first attempt, the tool re-runs that run's failed
and cancelled jobs once, prints the line and posts it as a pull request
comment, waits for the runs, and judges again. A job cancelled again (attempt
2 or later) refuses. Then it merges with the head sha pinned (a merge commit).

--dry-run prints the verdict and what it would do, and touches nothing: no
re-run, no comment, no merge. Exit 0: merged (dry run: would merge now);
1: refused (dry run: would refuse, or would re-run first); 2: usage or API error.
"""

import argparse
import json
import sys
import time
from pathlib import Path

import release_gates as rg

CAP_SECONDS = 120
CAP_SLACK_SECONDS = 5
WAIT_MINUTES = 30
POLL_SECONDS = 20
MERGE_METHOD = "merge"


def judge(api, repo, number, out, gates, cap, slack, local_ruleset=None):
    """Print the verdict; return (ok, plan) where plan lists cap-cancelled reruns or None."""
    pr = api.get(f"/repos/{repo}/pulls/{number}")
    sha = pr["head"]["sha"]
    print(f"pull request #{number} '{pr.get('title', '')}' head {pr['head'].get('ref')} at {sha}", file=out)
    red = []

    def say(ok, text):
        print(f"{'PASS' if ok else 'FAIL'}  {text}", file=out)
        if not ok:
            red.append(text)

    say(pr.get("state") == "open" and not pr.get("merged"),
        f"pull request is open (state {pr.get('state')}, merged {bool(pr.get('merged'))})")
    say(pr["base"]["ref"] == rg.MAIN, f"base is {rg.MAIN} (base {pr['base']['ref']})")
    main_sha = rg.resolve(api, repo, rg.MAIN)
    compare = api.get(f"/repos/{repo}/compare/{main_sha}...{sha}")
    up_to_date = compare.get("status") in ("ahead", "identical")
    say(up_to_date, f"head is up to date with {rg.MAIN} {main_sha[:10]} (compare: {compare.get('status')}, "
                    f"{compare.get('behind_by', '?')} behind)")

    ctx = rg.Context(api, repo, sha)
    contexts = rg.required_contexts(api, repo, main_sha)
    label = f"{rg.RULESET_PATH} on {rg.MAIN}"
    if contexts is None and local_ruleset is not None and local_ruleset.exists():
        contexts = rg.contexts_of(json.loads(local_ruleset.read_text(encoding="utf-8")))
        label = f"{rg.RULESET_PATH} of this checkout"
        print(f"NOTE  {rg.MAIN} has no {rg.RULESET_PATH} yet; holding the head to this checkout's", file=out)
    if contexts is None:
        say(False, f"{rg.RULESET_PATH} is missing on {rg.MAIN}: no list of required job names to hold the head to")
    verdicts, _ = rg.evaluate(ctx, gates, ("pull_request",), None, contexts, label)
    rerun, other = {}, []
    for verdict in verdicts:
        print(verdict.line(), file=out)
        if verdict.ok:
            continue
        if not verdict.red:
            other.append(verdict.line())
            continue
        for name, conclusion, secs, run in verdict.red:
            if conclusion == "cancelled" and secs is not None and secs >= cap - slack and run is not None:
                if run.get("run_attempt", 1) >= 2:
                    other.append(f"{name}: cancelled again at the cap ({secs} s) on attempt "
                                 f"{run.get('run_attempt')} of run {run['id']}: the one re-run is spent")
                else:
                    rerun.setdefault(run["id"], (run, []))[1].append((name, secs))
            elif conclusion == "cancelled":
                other.append(f"{name}: cancelled after {secs} s, before the {cap} s cap (superseded or by hand): "
                             f"not a cap overrun, no re-run")
            else:
                other.append(f"{name}: {conclusion}")
    for text in other:
        red.append(text)
    for run_id, (run, jobs) in rerun.items():
        print(f"CAP   run {run_id}: cancelled at the {cap} s cap: "
              + "; ".join(f"{n} ({s} s)" for n, s in jobs), file=out)
    if red:
        print(f"VERDICT: REFUSE #{number} at {sha[:10]}: {len(red) + sum(len(j) for _, j in rerun.values())} red",
              file=out)
        for text in red:
            print(f"  red: {text}", file=out)
        for run_id, (run, jobs) in rerun.items():
            for name, secs in jobs:
                print(f"  red: {name}: cancelled at the cap ({secs} s) in run {run_id}", file=out)
        return False, None, sha
    if rerun:
        return False, rerun, sha
    print(f"VERDICT: LAND #{number} at {sha[:10]}: every per-commit check on the head is success", file=out)
    return True, None, sha


def rerun_text(number, sha, rerun, cap):
    lines = [f"land.py: one re-run of jobs cancelled at the {cap} s cap on head {sha[:10]} of #{number}:"]
    for run_id, (run, jobs) in sorted(rerun.items()):
        lines.append(f"- run {run_id} ({run.get('name', '')}, attempt {run.get('run_attempt', 1)}): "
                     + "; ".join(f"{n} ({s} s)" for n, s in jobs))
    lines.append("A second cancel refuses the landing.")
    return "\n".join(lines)


def land(api, repo, number, dry_run=False, out=sys.stdout, gates=None, sleep=time.sleep, clock=time.monotonic,
         cap=CAP_SECONDS, slack=CAP_SLACK_SECONDS, wait_minutes=WAIT_MINUTES, local_ruleset=None):
    gates = gates if gates is not None else rg.load_gates()
    ok, rerun, sha = judge(api, repo, number, out, gates, cap, slack, local_ruleset)
    if rerun:
        text = rerun_text(number, sha, rerun, cap)
        if dry_run:
            print("VERDICT: REFUSE now; the only red jobs were cancelled at the cap on a first attempt.", file=out)
            print("dry run: would re-run once, post this comment, wait and judge again:", file=out)
            print(text, file=out)
            return 1
        before = {run_id: rerun[run_id][0].get("run_attempt", 1) for run_id in rerun}
        for run_id in sorted(rerun):
            api.post(f"/repos/{repo}/actions/runs/{run_id}/rerun-failed-jobs", {})
        print("RERUN " + text.replace("\n", "\nRERUN "), file=out)
        api.post(f"/repos/{repo}/issues/{number}/comments", {"body": text})
        deadline = clock() + wait_minutes * 60
        while True:
            # the new attempt must have started and finished: right after the
            # request the run can still read completed on its old attempt
            states = [api.get(f"/repos/{repo}/actions/runs/{run_id}") for run_id in sorted(rerun)]
            if all(s.get("status") == "completed" and s.get("run_attempt", 1) > before[s["id"]] for s in states):
                break
            if clock() >= deadline:
                print(f"REFUSE: the re-run did not finish within {wait_minutes} minutes", file=out)
                return 1
            sleep(POLL_SECONDS)
        ok, rerun, sha2 = judge(api, repo, number, out, gates, cap, slack, local_ruleset)
        if rerun or sha2 != sha:
            if rerun:
                print("REFUSE: cancelled again after the one re-run", file=out)
            return 1
    if not ok:
        return 1
    if dry_run:
        print(f"dry run: would merge #{number} at {sha} ({MERGE_METHOD})", file=out)
        return 0
    answer = api.put(f"/repos/{repo}/pulls/{number}/merge", {"sha": sha, "merge_method": MERGE_METHOD})
    print(f"merged #{number} at {sha}: {answer.get('sha', '')} {answer.get('message', '')}", file=out)
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("number", type=int)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--repo", default=rg.REPOSITORY)
    args = parser.parse_args(argv[1:])
    try:
        api = rg.Api(rg.token_from_environment())
        return land(api, args.repo, args.number, args.dry_run, local_ruleset=rg.ROOT / rg.RULESET_PATH)
    except (rg.ApiError, rg.GatesError) as error:
        print(f"ERROR: {error}")
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
