#!/usr/bin/env python3
"""Every job a push or pull request runs is bounded to two minutes.

The per-commit tier finishes within two minutes of the push (Glenn,
2026-10-31: one minute ideally, two at most). GitHub fails a job that runs
past its timeout-minutes, so a bound of two on every per-commit job turns an
overrun into a red check instead of a slower gate. This checker reads every
workflow under .github/workflows and, for each one a push or a pull_request
starts, refuses a job that:

  - has no job-level timeout-minutes;
  - has a timeout-minutes above two;
  - has a timeout-minutes expression other than ${{ inputs.full && N || M }}
    (the nightly tier's bound N, the per-commit bound M), or one with M above
    two;
  - calls a reusable workflow (jobs.<id>.uses takes no timeout-minutes; the
    called workflow's jobs carry the bound and are checked in their file only
    when it is itself started by a push or a pull_request, so a per-commit
    call is refused rather than trusted).

A job whose `if:` is `inputs.full` (or starts with `inputs.full &&`) runs only
in the nightly tier, and a job whose `if:` requires a tag push runs only on a
release; neither is bounded here. Text only, no YAML library: the workflows
are read as lines, two-space job keys under `jobs:` and four-space job keys.

Usage: check_ci_budget.py [--self-test] [workflow files...]
"""

import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUDGET_MINUTES = 2

TOP_KEY = re.compile(r"^([A-Za-z_][\w-]*):(.*)$")
JOB_KEY = re.compile(r"^  ([A-Za-z0-9_][\w-]*):\s*(#.*)?$")
JOB_PROP = re.compile(r"^    ([A-Za-z_][\w-]*):\s*(.*?)\s*$")
TIER_EXPR = re.compile(r"^\$\{\{\s*inputs\.full\s*&&\s*(\d+)\s*\|\|\s*(\d+)\s*\}\}$")
NIGHTLY_IF = re.compile(r"^(\$\{\{\s*)?inputs\.full(\s*&&.*)?(\s*\}\})?$")
TRIGGER = re.compile(r"\b(push|pull_request)\b")


def strip_comment(value):
    """The value with a trailing YAML comment removed (quoted '#' is kept)."""
    out, quote = [], None
    for ch in value:
        if quote:
            if ch == quote:
                quote = None
        elif ch in "'\"":
            quote = ch
        elif ch == "#":
            break
        out.append(ch)
    return "".join(out).strip()


def top_blocks(lines):
    """{top-level key: (inline value, [body lines])}."""
    blocks, key = {}, None
    for line in lines:
        m = TOP_KEY.match(line)
        if m:
            key = m.group(1)
            blocks[key] = (strip_comment(m.group(2)), [])
        elif key is not None:
            blocks[key][1].append(line)
    return blocks


def per_commit(blocks):
    """True when a push or a pull_request starts the workflow."""
    trigger = blocks.get("on") or blocks.get('"on"') or blocks.get("true")
    if trigger is None:
        return False
    inline, body = trigger
    if inline:
        return bool(TRIGGER.search(inline))
    for line in body:
        m = re.match(r"^  ([A-Za-z_][\w-]*):", line)
        if m and m.group(1) in ("push", "pull_request"):
            return True
        m = re.match(r"^  - ([A-Za-z_][\w-]*)\s*$", line)
        if m and m.group(1) in ("push", "pull_request"):
            return True
    return False


def jobs(blocks):
    """[(job id, line offset in the jobs block, {property: value})]."""
    if "jobs" not in blocks:
        return []
    found, current = [], None
    for number, line in enumerate(blocks["jobs"][1]):
        m = JOB_KEY.match(line)
        if m:
            current = (m.group(1), number, {})
            found.append(current)
            continue
        if current is None:
            continue
        m = JOB_PROP.match(line)
        if m:
            current[2][m.group(1)] = strip_comment(m.group(2))
    return found


def exempt(props):
    """The job runs only in the nightly tier or only on a release tag push."""
    cond = props.get("if", "")
    if NIGHTLY_IF.match(cond):
        return True
    return "github.event_name == 'push'" in cond and "startsWith(github.ref, 'refs/tags/" in cond


def violations(name, text):
    """(workflow, job, reason) for each per-commit job that is not bounded."""
    blocks = top_blocks(text.splitlines())
    if not per_commit(blocks):
        return []
    found = []
    for job, _, props in jobs(blocks):
        if exempt(props):
            continue
        if "uses" in props:
            found.append((name, job, "calls a reusable workflow from the per-commit tier"))
            continue
        value = props.get("timeout-minutes")
        if value is None:
            found.append((name, job, "has no timeout-minutes"))
            continue
        value = value.strip("'\"")
        if value.isdigit():
            minutes = int(value)
        else:
            m = TIER_EXPR.match(value)
            if not m:
                found.append((name, job, f"timeout-minutes {value!r} is not N or ${{{{ inputs.full && N || M }}}}"))
                continue
            minutes = int(m.group(2))
        if minutes > BUDGET_MINUTES:
            found.append((name, job, f"per-commit timeout-minutes {minutes} is over the {BUDGET_MINUTES}-minute budget"))
    return found


def check(paths):
    found = []
    for path in paths:
        found += violations(str(path), Path(path).read_text(encoding="utf-8"))
    return found


GOOD = """\
on:
  push:
    branches: [ main ]
  pull_request:
jobs:
  plain:
    runs-on: [self-hosted]
    timeout-minutes: 2
  tiered:
    runs-on: [self-hosted]
    timeout-minutes: ${{ inputs.full && 75 || 2 }}
  nightly:
    if: inputs.full
    runs-on: [self-hosted]
    timeout-minutes: 600
  nightly-and:
    if: inputs.full && github.ref == 'refs/heads/main'
    runs-on: [self-hosted]
  release:
    if: github.event_name == 'push' && startsWith(github.ref, 'refs/tags/v')
    runs-on: [self-hosted]
    timeout-minutes: 30
  under:
    runs-on: [self-hosted]
    timeout-minutes: 1  # comment
    steps:
      - name: a step with its own longer bound is held by the job's
        timeout-minutes: 5
"""

SCHEDULED = """\
on:
  schedule:
    - cron: "0 4 * * *"
  workflow_dispatch:
jobs:
  slow:
    runs-on: [self-hosted]
"""

BAD = [
    ("no bound", "on: [push]\njobs:\n  a:\n    runs-on: [self-hosted]\n"),
    ("over budget", "on:\n  pull_request:\njobs:\n  a:\n    timeout-minutes: 3\n"),
    ("tier over budget", "on:\n  push:\njobs:\n  a:\n    timeout-minutes: ${{ inputs.full && 10 || 3 }}\n"),
    ("unreadable expression", "on:\n  push:\njobs:\n  a:\n    timeout-minutes: ${{ matrix.os == 'windows' && 5 || 2 }}\n"),
    ("reusable call", "on:\n  pull_request:\njobs:\n  a:\n    uses: ./.github/workflows/x.yml\n"),
    ("other if is not exempt", "on:\n  push:\njobs:\n  a:\n    if: github.ref == 'refs/heads/main'\n    timeout-minutes: 5\n"),
    ("list trigger", "on:\n  - pull_request\njobs:\n  a:\n    timeout-minutes: 4\n"),
    ("bound in a step only", "on:\n  push:\njobs:\n  a:\n    steps:\n      - run: x\n        timeout-minutes: 1\n"),
]


def self_test():
    failed = 0
    if violations("good.yml", GOOD):
        print(f"self-test FAILED: bounded per-commit jobs refused: {violations('good.yml', GOOD)}")
        failed = 1
    if violations("scheduled.yml", SCHEDULED):
        print("self-test FAILED: a scheduled workflow was held to the per-commit budget")
        failed = 1
    for label, text in BAD:
        if not violations("bad.yml", text):
            print(f"self-test FAILED: {label} was accepted")
            failed = 1
    with tempfile.TemporaryDirectory() as d:
        bad = Path(d) / "bad.yml"
        bad.write_text(BAD[0][1], encoding="utf-8")
        if not check([bad]):
            print("self-test FAILED: a file on disk with an unbounded job was accepted")
            failed = 1
    if failed:
        return 1
    print(f"check_ci_budget self-test: {len(BAD) + 2} cases pass")
    return 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    paths = [Path(p) for p in argv[1:]] or sorted((ROOT / ".github" / "workflows").glob("*.y*ml"))
    found = check(paths)
    for name, job, reason in found:
        print(f"{name}: job {job} {reason}")
    if found:
        print(f"check_ci_budget: a per-commit job must finish within {BUDGET_MINUTES} minutes "
              "(Glenn 2026-10-31); move what does not fit to the nightly tier (nightly-ci.yml)")
        return 1
    print(f"check_ci_budget: every per-commit job is bounded to {BUDGET_MINUTES} minutes ({len(paths)} workflow files)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
