#!/usr/bin/env python3
"""Cut a rocketnet release: refuse unless every release gate is green for exactly that commit.

  python3 tools/release.py v1.1.8 <commit> [--dry-run] [--resume] [--repo owner/name]

Every gate is a line of tools/release_gates.txt, read through
tools/release_gates.py. In order:

 1. Preconditions: the version is vMAJOR.MINOR.PATCH[-PRERELEASE]; the commit is
    on main (an ancestor of origin/main); sdk/rocketnet.h's shipping
    ROCKETNET_VERSION_FULL at the commit equals the version;
    docs/RELEASE-NOTES-<version>.md exists at the commit; the tag does not exist.
 2. Every gate in force for the version, for exactly that commit, each printed
    with its run id and conclusion: the per-commit push run on main of each
    per-commit workflow, the nightly tier (Nightly CI, CBMC, TLC records and
    the TLC re-run evidence, the threaded TSan leg, MemorySanitizer). Gates in
    force only from a later version are printed as NOT IN FORCE.
 3. Only when all are green: creates the annotated tag on the commit and pushes
    it (the git data API: a tag object, then refs/tags/<version>), waits for
    Release Check on the tag, and requires its tag jobs (the second check,
    `threaded tsan run on the release commit`, and the run's final job) to
    succeed. Then publishes the GitHub release with the notes file, as latest
    (a prerelease is published as a prerelease, not latest).

--dry-run prints the verdict and touches nothing (no tag, no push, no
release). --resume: the tag already exists at this commit (an earlier run
refused to publish); wait for its Release Check again (re-run that job once
the gates are green) and publish. A pushed tag is never moved.

Token: GITHUB_TOKEN or GH_TOKEN, else `gh auth token` (honours GH_CONFIG_DIR).
Exit 0: released (or, with --dry-run, would release); 1: refused; 2: usage or
API error.
"""

import argparse
import datetime
import re
import sys
import time

import release_gates as rg

WAIT_MINUTES = 90
POLL_SECONDS = 30


def wait_for_tag_run(ctx, gates, out, sleep, clock, wait_minutes):
    """Wait for the tag's Release Check run to finish; return True when every tag gate succeeded."""
    tag_gates = [g for g in gates if g.kind == "tag" and g.in_force(ctx.version)]
    if not tag_gates:
        print("FAIL  no tag gate in tools/release_gates.txt: nothing would judge the tag", file=out)
        return False
    workflows = sorted({g.workflow for g in tag_gates})
    deadline = clock() + wait_minutes * 60
    while True:
        ctx.forget()
        runs = {w: ctx.runs(w, "tag") for w in workflows}
        if all(runs[w] and runs[w][0].get("status") == "completed" for w in workflows):
            break
        if clock() >= deadline:
            print(f"FAIL  the tag's {', '.join(workflows)} run did not finish within {wait_minutes} minutes; "
                  f"not publishing (run again with --resume)", file=out)
            return False
        state = ", ".join(f"{w}: {rg.describe(runs[w][0]) + ' ' + runs[w][0].get('status') if runs[w] else 'no run yet'}"
                          for w in workflows)
        print(f"wait  {state}", file=out)
        sleep(POLL_SECONDS)
    verdicts, _ = rg.evaluate(ctx, tag_gates, ("tag",), ctx.version)
    for verdict in verdicts:
        print(verdict.line(), file=out)
    return all(v.ok for v in verdicts)


def release(api, repo, version, ref, dry_run=False, resume=False, out=sys.stdout, gates=None, now=None,
            sleep=time.sleep, clock=time.monotonic, wait_minutes=WAIT_MINUTES):
    gates = gates if gates is not None else rg.load_gates()
    ok, sha, ctx = rg.judge_release(api, repo, version, ref, out, "pre", gates, now)
    tag_exists = False
    if sha is not None:
        try:
            api.get(f"/repos/{repo}/git/ref/tags/{version}")
            tag_exists = True
        except rg.NotFound:
            pass
    if not ok:
        return 1
    if dry_run:
        print("dry run: nothing tagged, pushed or published", file=out)
        return 0
    if tag_exists and not resume:
        print(f"REFUSE: tag {version} exists; a tag is never moved or reused (use --resume to publish an existing "
              f"tag at this commit after its Release Check is green)", file=out)
        return 1
    if not tag_exists:
        tag = api.post(f"/repos/{repo}/git/tags", {
            "tag": version, "message": f"rocketnet {version}", "object": sha, "type": "commit"})
        api.post(f"/repos/{repo}/git/refs", {"ref": f"refs/tags/{version}", "sha": tag["sha"]})
        print(f"tagged {version} -> {sha} (tag object {tag['sha']}) and pushed refs/tags/{version}", file=out)
    if not wait_for_tag_run(ctx, gates, out, sleep, clock, wait_minutes):
        print(f"REFUSE: not publishing {version}: the tag's Release Check is not green", file=out)
        return 1
    notes = rg.file_at(api, repo, f"docs/RELEASE-NOTES-{version}.md", sha)
    prerelease = "-" in version
    answer = api.post(f"/repos/{repo}/releases", {
        "tag_name": version, "name": version, "body": notes, "prerelease": prerelease,
        "make_latest": "false" if prerelease else "true"})
    print(f"published {version}: {answer.get('html_url', '')}" + (" (prerelease)" if prerelease else " (latest)"),
          file=out)
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("version")
    parser.add_argument("commit")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--repo", default=rg.REPOSITORY)
    parser.add_argument("--wait-minutes", type=int, default=WAIT_MINUTES)
    args = parser.parse_args(argv[1:])
    if not re.match(r"^[0-9a-fA-F]{7,40}$", args.commit):
        print(f"commit {args.commit!r}: give a commit id, not a branch or tag name")
        return 2
    try:
        api = rg.Api(rg.token_from_environment())
        return release(api, args.repo, args.version, args.commit, args.dry_run, args.resume,
                       now=datetime.datetime.now(datetime.timezone.utc), wait_minutes=args.wait_minutes)
    except (rg.ApiError, rg.GatesError) as error:
        print(f"ERROR: {error}")
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
