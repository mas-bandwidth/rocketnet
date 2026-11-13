#!/usr/bin/env python3
"""tools/release_gates.txt names only workflows and jobs that exist, and leaves no per-commit workflow out.

Refuses (exit 1, one line each):
  - a malformed or empty gates file (tools/release_gates.py's parser);
  - a workflow named that is not a file under .github/workflows;
  - a job named (job, skip, tag rows; "<ref>" stands for a branch) that no job
    of that workflow can be called, matrix values and reusable calls included;
  - a tlc-rerun row whose prefix no job of the workflow starts with, or a
    workflow with no `TLC re-run full` job and no `TLC re-run slice K/N` job;
  - a skip row for a job with no `if:` (only a job's own `if:` may skip it);
  - a workflow a push or pull request starts that has no per-commit gate row
    (jobs or jobs-if-ran), so a new per-commit workflow cannot go unjudged.

Usage: check_release_gates.py [--self-test] [gates file] [workflows directory]
"""

import re
import sys
import tempfile
from pathlib import Path

import release_gates as rg
import workflow_jobs as wj

ROOT = Path(__file__).resolve().parent.parent


def problems(gates_path, directory):
    found = []
    try:
        gates = rg.load_gates(gates_path)
    except rg.GatesError as error:
        return [str(error)]
    workflows = {}
    for path in sorted(Path(directory).glob("*.y*ml")):
        try:
            workflows[path.name] = wj.Workflow(path)
        except wj.WorkflowError as error:
            found.append(f"{path.name}: cannot be read: {error}")
    if not workflows:
        return found + [f"{directory}: no workflow files"]
    for gate in gates:
        where = f"release_gates.txt:{gate.line}"
        workflow = workflows.get(gate.workflow)
        if workflow is None:
            found.append(f"{where}: workflow {gate.workflow} does not exist in {directory}")
            continue
        if gate.kind in ("job", "skip", "full", "tag"):
            name = gate.job.replace("<ref>", "main")
            jobs = workflow.find(name)
            if not jobs:
                found.append(f"{where}: no job of {gate.workflow} is named {gate.job!r}")
            elif gate.kind == "skip" and not any(j.condition is not None for j in jobs):
                found.append(f"{where}: skip row for {gate.job!r}, which has no if: and so is never skipped")
        if gate.kind == "tlc-rerun":
            prefix = re.escape(gate.job)
            names = [regex for regex, _, _ in workflow.patterns() if regex.startswith(prefix)]
            full = workflow.find(rg.FULL_RERUN)
            slices = [j for _, j, p in workflow.patterns() if (p + str(j.name)).startswith("TLC re-run slice ")]
            if not names or not (full or slices):
                found.append(f"{where}: {gate.workflow} has no job named '{rg.FULL_RERUN}' or 'TLC re-run slice "
                             f"K/N': the re-run evidence the gate reads does not exist")
    judged = {g.workflow for g in gates if g.kind in ("jobs", "jobs-if-ran")}
    for name, workflow in workflows.items():
        if workflow.per_commit() and name not in judged:
            found.append(f"{name}: a push or pull request starts it but no jobs/jobs-if-ran row judges it")
    return found


def check(gates_path, directory, out=sys.stdout):
    found = problems(gates_path, directory)
    for line in found:
        print(f"check_release_gates: {line}", file=out)
    if found:
        print(f"check_release_gates: {len(found)} problem(s)", file=out)
        return 1
    count = len(rg.load_gates(gates_path))
    print(f"check_release_gates: {count} gates name existing workflows and jobs", file=out)
    return 0


WORKFLOW = """\
name: CI
on:
  push:
  pull_request:
jobs:
  build:
    name: ${{ matrix.os }} / unit
    strategy:
      matrix:
        os: [linux, macos]
  nightly:
    name: nightly thing / ${{ matrix.x }}
    if: inputs.full
    strategy:
      matrix:
        x: [a]
  always:
    name: always runs
"""

TLC = """\
name: TLC
on:
  schedule:
    - cron: "0 1 * * *"
jobs:
  stale:
    name: Model records are current
  rerun:
    name: TLC re-run slice ${{ matrix.k }}/7
    strategy:
      matrix:
        k: [0, 1, 2, 3, 4, 5, 6]
"""

GOOD = """\
ci.yml | - | jobs | v1.1.8 | push,pull_request | every job
ci.yml | nightly thing / ${{ matrix.x }} | skip | v1.1.8 | push | nightly
ci.yml | linux / unit | job | v1.1.8 | nightly | a matrix row
tlc.yml | Model records are current | job | v1.1.8 | nightly | records
tlc.yml | TLC re-run | tlc-rerun | v1.1.8 | nightly | re-run
"""

BAD = [
    ("missing workflow", GOOD + "gone.yml | - | run | v1.1.8 | nightly | x\n"),
    ("missing job", GOOD + "ci.yml | linux / functional | job | v1.1.8 | nightly | x\n"),
    ("misspelt skip", GOOD.replace("nightly thing /", "nightly thin /")),
    ("skip of a job with no if", GOOD + "ci.yml | always runs | skip | v1.1.8 | push | x\n"),
    ("per-commit workflow not judged", GOOD.replace("ci.yml | - | jobs", "ci.yml | - | run")),
    ("empty list", "# nothing\n"),
    ("tag job that does not exist", GOOD + "ci.yml | release gates passed on the tag | tag | v1.1.8 | tag | x\n"),
]


def self_test():
    failed = 0
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        (root / "wf").mkdir()
        (root / "wf" / "ci.yml").write_text(WORKFLOW, encoding="utf-8")
        (root / "wf" / "tlc.yml").write_text(TLC, encoding="utf-8")
        gates = root / "gates.txt"
        gates.write_text(GOOD, encoding="utf-8")
        if problems(gates, root / "wf"):
            print(f"self-test FAILED: the good list was refused: {problems(gates, root / 'wf')}")
            failed = 1
        for label, text in BAD:
            gates.write_text(text, encoding="utf-8")
            if not problems(gates, root / "wf"):
                print(f"self-test FAILED: {label} was accepted")
                failed = 1
        # no re-run job in tlc.yml: the tlc-rerun row is refused
        gates.write_text(GOOD, encoding="utf-8")
        (root / "wf" / "tlc.yml").write_text(TLC.split("  rerun:")[0], encoding="utf-8")
        if not problems(gates, root / "wf"):
            print("self-test FAILED: a tlc-rerun row with no re-run job was accepted")
            failed = 1
        # no workflows at all
        (root / "empty").mkdir()
        if not problems(gates, root / "empty"):
            print("self-test FAILED: an empty workflows directory was accepted")
            failed = 1
    if failed:
        return 1
    print(f"check_release_gates self-test: {len(BAD) + 3} cases pass")
    return 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    gates = Path(argv[1]) if len(argv) > 1 else rg.GATES_FILE
    directory = Path(argv[2]) if len(argv) > 2 else ROOT / ".github" / "workflows"
    return check(gates, directory)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
