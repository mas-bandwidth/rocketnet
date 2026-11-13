#!/usr/bin/env python3
"""A required check that is a summary of shard jobs is green only when every shard is.

Two summary jobs use it: "linux / Debug / unit" (the two Debug unit shards, the
default names) and the lint summary (lint (0/4) .. (3/4), the names given as
arguments).

The Linux Debug unit leg runs as two shard rows (ci.yml build-and-test, shard 0/2
and 1/2) and the summary job debug-unit (named linux / Debug / unit, the name
the ruleset, tools/land.py and tools/release_gates.py require) calls this with
the run's job list as JSON (the GitHub API's actions/runs/<id>/attempts/<n>/jobs
answer, or just its "jobs" array). Both shards must be listed once each and
concluded success; a failure, a cancellation, a skipped, a missing or a
duplicated shard is red. An empty or unreadable list is red too: no verdict is
never green.

Prints DEBUG-UNIT-SHARDS=FAIL <why> and exits 1, or DEBUG-UNIT-SHARDS=PASS and
exits 0.

Usage: check_debug_unit_shards.py [--self-test] <job-list.json> [shard job name ...]
"""

import json
import sys
import tempfile
from pathlib import Path

SHARDS = ("linux / Debug / unit (0/2)", "linux / Debug / unit (1/2)")


def verdict(text, shards=SHARDS):
    """(ok, reason) for the job list's JSON text."""
    try:
        data = json.loads(text)
    except ValueError as error:
        return False, f"the job list is not JSON: {error}"
    jobs = data.get("jobs") if isinstance(data, dict) else data
    if not isinstance(jobs, list) or not jobs:
        return False, "the job list has no jobs"
    for name in shards:
        found = [j.get("conclusion") for j in jobs if isinstance(j, dict) and j.get("name") == name]
        if found != ["success"]:
            return False, f"{name}: {found or 'missing'}"
    return True, ""


def run(path, shards=SHARDS):
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError as error:
        print(f"DEBUG-UNIT-SHARDS=FAIL no verdict: {error}")
        return 1
    ok, why = verdict(text, shards)
    if not ok:
        print(f"DEBUG-UNIT-SHARDS=FAIL {why}")
        return 1
    print("DEBUG-UNIT-SHARDS=PASS")
    return 0


def self_test():
    def jobs(a, b, extra=()):
        rows = [{"name": SHARDS[0], "conclusion": a}, {"name": SHARDS[1], "conclusion": b}]
        return json.dumps({"jobs": rows + list(extra)})

    cases = [
        (jobs("success", "success"), True),
        (jobs("success", "success", [{"name": "macos / Debug / unit", "conclusion": "failure"}]), True),
        (json.dumps([{"name": SHARDS[0], "conclusion": "success"}, {"name": SHARDS[1], "conclusion": "success"}]), True),
        (jobs("failure", "success"), False),
        (jobs("success", "failure"), False),
        (jobs("success", "cancelled"), False),
        (jobs("success", None), False),
        (jobs("skipped", "success"), False),
        (jobs("success", "success", [{"name": SHARDS[1], "conclusion": "success"}]), False),
        (json.dumps({"jobs": [{"name": SHARDS[0], "conclusion": "success"}]}), False),
        (json.dumps({"jobs": []}), False),
        ("", False),
        ("not json", False),
    ]
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for number, (text, want) in enumerate(cases):
            path = Path(tmp) / f"case{number}.json"
            path.write_text(text, encoding="utf-8")
            if (run(path) == 0) != want:
                print(f"self-test case {number}: expected {'PASS' if want else 'FAIL'}")
                bad += 1
        if run(Path(tmp) / "absent.json") != 1:
            bad += 1
        four = tuple("lint (%d/4)" % n for n in range(4))
        path = Path(tmp) / "four.json"
        for conclusions, want in ((("success",) * 4, True), (("success", "failure", "success", "success"), False),
                                  (("success", "success", "success", "cancelled"), False),
                                  (("success", "success", "success", None), False)):
            path.write_text(json.dumps({"jobs": [{"name": n, "conclusion": c} for n, c in zip(four, conclusions)]}))
            if (run(path, four) == 0) != want:
                print("self-test four shards: %r expected %s" % (conclusions, want))
                bad += 1
        path.write_text(json.dumps({"jobs": [{"name": n, "conclusion": "success"} for n in four[:3]]}))
        if run(path, four) != 1:
            print("self-test four shards: a missing shard was not red")
            bad += 1
    if bad:
        return 1
    print("check_debug_unit_shards: self-test ok")
    return 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    if len(argv) < 2:
        print(__doc__)
        return 2
    return run(argv[1], tuple(argv[2:]) or SHARDS)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
