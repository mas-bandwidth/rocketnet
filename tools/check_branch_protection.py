#!/usr/bin/env python3
""".github/rulesets/main.json requires exactly the per-commit job names the workflows produce.

docs/branch-protection.md shows the one `gh api` command that applies the
ruleset to main. A required check that names a job no workflow produces
blocks every merge forever; a per-commit job the ruleset leaves out lands
without a verdict. So the required names must EQUAL the names derived from
the workflows (tools/workflow_jobs.py: every job a pull request starts, in
every workflow it starts without a path filter, matrix rows expanded, the
nightly tier's `if: inputs.full` jobs left out). It also refuses a ruleset
that is not strict (head up to date with main), that is not active on the
default branch, that names a check twice, or that lists no check at all.

Usage: check_branch_protection.py [--self-test] [ruleset.json] [workflows directory]
"""

import json
import sys
import tempfile
from pathlib import Path

import workflow_jobs as wj

ROOT = Path(__file__).resolve().parent.parent
RULESET = ROOT / ".github" / "rulesets" / "main.json"
ACTIONS_APP = 15368


def problems(ruleset_path, directory):
    found = []
    try:
        ruleset = json.loads(Path(ruleset_path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        return [f"{ruleset_path}: {error}"]
    if ruleset.get("enforcement") != "active":
        found.append("enforcement is not 'active'")
    if ruleset.get("conditions", {}).get("ref_name", {}).get("include") != ["~DEFAULT_BRANCH"]:
        found.append("the ruleset does not target the default branch (main) alone")
    rules = [r for r in ruleset.get("rules", []) if r.get("type") == "required_status_checks"]
    if len(rules) != 1:
        return found + [f"{len(rules)} required_status_checks rules; want exactly one"]
    parameters = rules[0].get("parameters", {})
    if parameters.get("strict_required_status_checks_policy") is not True:
        found.append("strict_required_status_checks_policy is not true: a head behind main could land")
    checks = parameters.get("required_status_checks", [])
    names = [c.get("context") for c in checks]
    if not names:
        found.append("no required status checks: zero inputs would require nothing")
    for c in checks:
        if c.get("integration_id") != ACTIONS_APP:
            found.append(f"{c.get('context')!r}: integration_id is not {ACTIONS_APP} (GitHub Actions); "
                         "any app or status could satisfy it")
    duplicates = sorted({n for n in names if names.count(n) > 1})
    if duplicates:
        found.append(f"named twice: {duplicates}")
    try:
        derived = wj.required_names(directory)
    except wj.WorkflowError as error:
        return found + [f"the workflows cannot be read: {error}"]
    if not derived:
        found.append(f"{directory}: no per-commit job derived from the workflows")
    for name in sorted(set(derived) - set(names)):
        found.append(f"per-commit job {name!r} is not a required check")
    for name in sorted(set(names) - set(derived)):
        found.append(f"required check {name!r} is no per-commit job of any workflow: it would block every merge")
    return found


def check(ruleset_path, directory, out=sys.stdout):
    found = problems(ruleset_path, directory)
    for line in found:
        print(f"check_branch_protection: {line}", file=out)
    if found:
        print(f"check_branch_protection: {len(found)} problem(s) in {ruleset_path}", file=out)
        return 1
    count = len(wj.required_names(directory))
    print(f"check_branch_protection: the ruleset requires exactly the {count} per-commit jobs", file=out)
    return 0


WORKFLOW = """\
on:
  push:
  pull_request:
jobs:
  build:
    name: ${{ matrix.os }} / unit
    strategy:
      matrix:
        os: ${{ fromJSON(inputs.full && '["linux","macos","windows"]' || '["linux","macos"]') }}
  nightly:
    if: inputs.full
    name: nightly only
  tag:
    if: github.event_name == 'push' && startsWith(github.ref, 'refs/tags/v')
    name: tag only
  pr:
    if: github.event_name == 'pull_request' || startsWith(github.ref, 'refs/heads/')
    name: wire changes
"""

FILTERED = """\
on:
  pull_request:
    paths: [docs/roadmap.sexp]
jobs:
  check:
    name: roadmap
"""


def ruleset(names, strict=True, integration=ACTIONS_APP):
    return json.dumps({
        "enforcement": "active",
        "conditions": {"ref_name": {"include": ["~DEFAULT_BRANCH"], "exclude": []}},
        "rules": [{"type": "required_status_checks", "parameters": {
            "strict_required_status_checks_policy": strict,
            "required_status_checks": [{"context": n, "integration_id": integration} for n in names]}}]})


GOOD = ["linux / unit", "macos / unit", "wire changes"]
BAD = [
    ("a per-commit job left out", ruleset(GOOD[:2])),
    ("a name no job produces", ruleset(GOOD + ["windows / unit"])),
    ("a nightly-only job required", ruleset(GOOD + ["nightly only"])),
    ("a path-filtered workflow required", ruleset(GOOD + ["roadmap"])),
    ("not strict", ruleset(GOOD, strict=False)),
    ("no integration id", ruleset(GOOD, integration=None)),
    ("a name twice", ruleset(GOOD + ["wire changes"])),
    ("zero checks", ruleset([])),
    ("not json", "{"),
]


def self_test():
    failed = 0
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        (root / "wf").mkdir()
        (root / "wf" / "ci.yml").write_text(WORKFLOW, encoding="utf-8")
        (root / "wf" / "roadmap.yml").write_text(FILTERED, encoding="utf-8")
        path = root / "main.json"
        path.write_text(ruleset(GOOD), encoding="utf-8")
        if problems(path, root / "wf"):
            print(f"self-test FAILED: the good ruleset was refused: {problems(path, root / 'wf')}")
            failed = 1
        for label, text in BAD:
            path.write_text(text, encoding="utf-8")
            if not problems(path, root / "wf"):
                print(f"self-test FAILED: {label} was accepted")
                failed = 1
        (root / "empty").mkdir()
        path.write_text(ruleset(GOOD), encoding="utf-8")
        if not problems(path, root / "empty"):
            print("self-test FAILED: no workflows was accepted")
            failed = 1
    if failed:
        return 1
    print(f"check_branch_protection self-test: {len(BAD) + 2} cases pass")
    return 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    ruleset_path = Path(argv[1]) if len(argv) > 1 else RULESET
    directory = Path(argv[2]) if len(argv) > 2 else ROOT / ".github" / "workflows"
    return check(ruleset_path, directory)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
