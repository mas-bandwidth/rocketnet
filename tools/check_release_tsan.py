#!/usr/bin/env python3
"""Release Check's second check, on the v* tag push: every release gate green for the tagged commit.

tools/release.py judges the release commit against tools/release_gates.txt
BEFORE it creates the tag, and refuses unless every gate in force for the
version is green. This script runs the same judgement (tools/release_gates.py,
the same list, the same rules) again on the tag push, from Release Check's
`threaded tsan run on the release commit` job; the tag run's final job,
`release gates passed on the tag`, is red unless this one succeeded, and
tools/release.py publishes only when both are green.

The gates judged are those in force for the tag's version, for the push and
nightly events: the per-commit push runs on main, Nightly CI, CBMC, TLC
records and the TLC re-run evidence, nightly.yml's `tsan (threaded transport)
(<ref>)` leg on the run's own head commit, MemorySanitizer. It also checks the
commit is on main and sdk/rocketnet.h's shipping version equals the tag.

Environment: GITHUB_REPOSITORY (owner/name), GITHUB_SHA (the tagged commit),
GITHUB_REF_NAME (the tag, the version), GITHUB_TOKEN (actions: read,
contents: read), GITHUB_API_URL (defaults to https://api.github.com).

Exit 0 when every gate in force is green on the tagged commit, 1 when any is
red (each gate is printed with its run and conclusion), 2 when the environment
is incomplete or the API answers with an error.
"""

import os
import sys

import release_gates as rg

ApiError = rg.ApiError


def check(api, repository, sha, version, out=sys.stdout, gates=None, now=None):
    """0 when every gate in force for version is green for sha, else 1."""
    gates = gates if gates is not None else rg.load_gates()
    ok, _, _ = rg.judge_release(api, repository, version, sha, out, "tag", gates, now)
    if ok:
        # zero inputs is an error: no gate in force for the version judges nothing
        judged = [g for g in gates if g.in_force(version) and any(e in g.events for e in ("push", "nightly"))]
        if not judged:
            print(f"FAIL  0 release gates in force for {version} on push or nightly: nothing was judged", file=out)
            ok = False
    if not ok:
        print(f"::error::Release gates are not green for {version} at {sha}; see the FAIL lines above "
              f"(tools/release_gates.txt, docs/RELEASING.md).", file=out)
    return 0 if ok else 1


def main():
    repository = os.environ.get("GITHUB_REPOSITORY", "")
    sha = os.environ.get("GITHUB_SHA", "")
    version = os.environ.get("GITHUB_REF_NAME", "")
    token = os.environ.get("GITHUB_TOKEN", "")
    api = os.environ.get("GITHUB_API_URL", "https://api.github.com")
    if not repository or not sha or not version or not token:
        print("::error::GITHUB_REPOSITORY, GITHUB_SHA, GITHUB_REF_NAME and GITHUB_TOKEN must all be set.")
        return 2
    try:
        return check(rg.Api(token, api), repository, sha, version)
    except (rg.ApiError, rg.GatesError) as error:
        print(f"::error::The release gates could not be judged: {error}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
