#!/usr/bin/env python3
"""A required check that is a summary of shard jobs is green only when every shard is.

The summary jobs use it: "linux / Debug / unit" (the four Debug unit shards,
the default names) and the others (lint (0/4) .. (3/4), the functional,
transport matrix, macOS Debug unit and trace check shards, the names given as
arguments).

The Linux Debug unit leg runs as four shard rows (ci.yml build-and-test, shard
0/4 to 3/4) and the summary job debug-unit (named linux / Debug / unit, the
name the ruleset, tools/land.py and tools/release_gates.py require) calls this
with the run's job list as JSON (the GitHub API's actions/runs/<id>/attempts/<n>/jobs
answer, or just its "jobs" array). Every shard must be listed once and
concluded success; a failure, a cancellation, a skipped, a missing or a
duplicated shard is red. An empty or unreadable list is red too: no verdict is
never green.

When ci.yml runs as a called workflow (job full of nightly-ci.yml calls it), the
API lists each of its jobs under the name of the caller job: "full / linux /
Debug / unit (0/4)". The summary jobs pass that as --prefix "full / " (the
job_name_prefix input of ci.yml, empty per commit), and every shard is looked
up as the prefix and its name; a prefix that does not end in " / " is red.

Prints DEBUG-UNIT-SHARDS=FAIL <why> and exits 1, or DEBUG-UNIT-SHARDS=PASS and
exits 0.

Usage: check_debug_unit_shards.py [--prefix "<caller job> / "] <job-list.json> [shard job name ...]
       check_debug_unit_shards.py --self-test
"""

import json
import sys
import tempfile
from pathlib import Path

SHARDS = ("linux / Debug / unit (0/4)", "linux / Debug / unit (1/4)", "linux / Debug / unit (2/4)",
          "linux / Debug / unit (3/4)")


def verdict(text, shards=SHARDS, prefix=""):
    """(ok, reason) for the job list's JSON text; each shard is listed as prefix + its name."""
    try:
        data = json.loads(text)
    except ValueError as error:
        return False, f"the job list is not JSON: {error}"
    jobs = data.get("jobs") if isinstance(data, dict) else data
    if not isinstance(jobs, list) or not jobs:
        return False, "the job list has no jobs"
    for shard in shards:
        name = prefix + shard
        found = [j.get("conclusion") for j in jobs if isinstance(j, dict) and j.get("name") == name]
        if found != ["success"]:
            return False, f"{name}: {found or 'missing'}"
    return True, ""


def run(path, shards=SHARDS, prefix=""):
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError as error:
        print(f"DEBUG-UNIT-SHARDS=FAIL no verdict: {error}")
        return 1
    ok, why = verdict(text, shards, prefix)
    if not ok:
        print(f"DEBUG-UNIT-SHARDS=FAIL {why}")
        return 1
    print("DEBUG-UNIT-SHARDS=PASS")
    return 0


def self_test():
    def jobs(a, b, c="success", extra=(), d="success"):
        rows = [{"name": SHARDS[0], "conclusion": a}, {"name": SHARDS[1], "conclusion": b},
                {"name": SHARDS[2], "conclusion": c}, {"name": SHARDS[3], "conclusion": d}]
        return json.dumps({"jobs": rows + list(extra)})

    cases = [
        (jobs("success", "success"), True),
        (jobs("success", "success", extra=[{"name": "macos / Debug / unit", "conclusion": "failure"}]), True),
        (json.dumps([{"name": name, "conclusion": "success"} for name in SHARDS]), True),
        (jobs("failure", "success"), False),
        (jobs("success", "failure"), False),
        (jobs("success", "cancelled"), False),
        (jobs("success", "success", "cancelled"), False),
        (jobs("success", "success", d="cancelled"), False),
        (jobs("success", None), False),
        (jobs("skipped", "success"), False),
        (jobs("success", "success", extra=[{"name": SHARDS[1], "conclusion": "success"}]), False),
        (json.dumps({"jobs": [{"name": SHARDS[0], "conclusion": "success"}, {"name": SHARDS[1], "conclusion": "success"}]}), False),
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
        # ci.yml as a called workflow (nightly-ci.yml, job full): the API lists every job of the
        # run as "full / <name>", and the summary jobs pass --prefix "full / " (the
        # job_name_prefix input; per commit it is empty and the names are the job names).
        made = []

        def called(rows):
            target = Path(tmp) / ("called%d.json" % len(made))
            target.write_text(json.dumps({"jobs": [{"name": n, "conclusion": c} for n, c in rows]}))
            made.append(target)
            return str(target)

        full = [("full / " + n, "success") for n in SHARDS + four]
        plain = [(n, "success") for n in SHARDS + four]
        for args, want in (
                (["--prefix", "full / ", called(full)], 0),
                (["--prefix", "full / ", called(full)] + list(four), 0),
                ([called(full)], 1),                                    # no prefix: every shard is missing
                (["--prefix", "", called(full)], 1),
                (["--prefix", "nightly / ", called(full)], 1),          # another caller
                (["--prefix", "full / ", called(plain)], 1),            # per-commit names under a prefix
                (["--prefix", "", called(plain)], 0),                   # empty prefix: the per-commit lookup
                ([called(plain)], 0),
                (["--prefix", "full / ", called(full + [full[2]])], 1),  # a shard listed twice
                (["--prefix", "full / ", called(full[:1] + [("full / " + SHARDS[1], "failure")] + full[2:])], 1),
                (["--prefix", "full / ", called(full[:3] + full[4:])], 1),  # shard 3/4 missing
                (["--prefix", "full / ", called(full + plain)], 0),     # the unprefixed names beside it: not read
                (["--prefix", "full/", called([("full/" + n, "success") for n in SHARDS])], 1),  # no " / " at its end
                (["--prefix", "full / "], 2),                           # no job list
                (["--prefix"], 2),
        ):
            if main(["check_debug_unit_shards.py"] + args) != want:
                print("self-test called workflow: %r expected exit %d" % (args, want))
                bad += 1
    if bad:
        return 1
    print("check_debug_unit_shards: self-test ok")
    return 0


def main(argv):
    args = argv[1:]
    if args == ["--self-test"]:
        return self_test()
    prefix = ""
    if args[:1] == ["--prefix"]:
        if len(args) < 2:
            print(__doc__)
            return 2
        prefix, args = args[1], args[2:]
    if not args:
        print(__doc__)
        return 2
    if prefix and not prefix.endswith(" / "):
        print(f"DEBUG-UNIT-SHARDS=FAIL no verdict: the prefix {prefix!r} is not a caller job name and \" / \"")
        return 1
    return run(args[0], tuple(args[1:]) or SHARDS, prefix)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
