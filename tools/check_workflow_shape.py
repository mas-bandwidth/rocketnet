#!/usr/bin/env python3
"""Every workflow is written in the one shape the workflow checkers read.

check-runs-on.sh, check-main-concurrency.sh and check_ci_budget.py read
.github/workflows as lines, with no YAML library: bare top-level keys, job
ids two spaces in, job properties four spaces in. GitHub accepts many other
spellings of the same workflow (quoted keys, four-space indentation, a job
written as a flow mapping), and each checker passed every one of them
unread: a hosted runner, an unbounded per-commit job or a shared main group
went through (VERIFY-3 A-1, 19 of 26 plants). This check refuses any
workflow not in the canonical shape, so the three checkers read everything
GitHub would run. It refuses:

  - a tab in indentation;
  - a top-level key that is not a bare name (`"on":`, `'jobs':`, `true:`),
    a top-level key given twice, or a workflow without `on:` and `jobs:`;
  - a top-level block whose first line is not two spaces in;
  - under jobs: a job line that is not `  <id>:` alone (a flow mapping, an
    inline value), a job id given twice, a job whose first property is not
    four spaces in, a property that is not a bare key, a merge key (`<<:`)
    or an anchor or alias at job or property level, or a line one, three or
    five spaces in;
  - a concurrency (top level or job level) that is not a block with one
    `group:` line just inside it.

Block scalars (`run: |`) are skipped. Zero workflow files is a failure.

Usage: check_workflow_shape.py [--self-test] [workflow files...]
"""

import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

BARE_TOP = re.compile(r"^[A-Za-z_][\w-]*:(\s|$)")
JOB_LINE = re.compile(r"^  [A-Za-z0-9_][\w-]*:\s*(#.*)?$")
PROPERTY = re.compile(r"^    [A-Za-z_][\w-]*:(\s|$)")
BLOCK_SCALAR = re.compile(r":\s*[|>][-+0-9]*\s*(#.*)?$")
ANCHORED = re.compile(r"^\s*(?:-\s+)?[^\s#][^:#]*:\s*[&*]")


def indent(line):
    return len(line) - len(line.lstrip(" "))


def content_lines(text):
    """(line number, line) for every line outside a block scalar, comments and blank lines left out."""
    out = []
    scalar_indent = None
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if scalar_indent is not None:
            if not stripped or indent(line) > scalar_indent:
                continue
            scalar_indent = None
        if not stripped or stripped.startswith("#"):
            continue
        out.append((number, line))
        if BLOCK_SCALAR.search(line):
            scalar_indent = indent(line)
    return out


def violations(name, text):
    """(workflow, line number, reason) for each departure from the canonical shape."""
    found = []
    lines = content_lines(text)
    for number, line in enumerate(text.splitlines(), 1):
        lead = line[:len(line) - len(line.lstrip(" \t"))]
        if "\t" in lead:
            found.append((name, number, "a tab in indentation"))
    tops = {}
    top = None
    expect_child = None
    job = None
    job_ids = {}
    expect_property = False
    concurrency = None  # (line number, indent of the key, saw group)

    def close_concurrency():
        if concurrency is not None and not concurrency[2]:
            found.append((name, concurrency[0], "concurrency without a `group:` line just inside it"))

    for number, line in lines:
        if line.startswith("---") or line.startswith("..."):
            continue
        level = indent(line)
        if concurrency is not None and level <= concurrency[1]:
            close_concurrency()
            concurrency = None
        if level == 0:
            if expect_child is not None:
                found.append((name, expect_child, "a top-level block with nothing two spaces in"))
                expect_child = None
            if not BARE_TOP.match(line):
                found.append((name, number, "a top-level key that is not a bare name: %s" % line.strip()))
                top = None
                continue
            top = line.split(":", 1)[0]
            if top in tops:
                found.append((name, number, "top-level key %r given twice (first at line %d)" % (top, tops[top])))
            tops[top] = number
            value = line.split(":", 1)[1].split("#", 1)[0].strip()
            if not value:
                expect_child = number
            if ANCHORED.match(line):
                found.append((name, number, "an anchor or alias at the top level"))
            if top == "concurrency":
                if value:
                    found.append((name, number, "concurrency given as a string; write a block with a `group:` line"))
                else:
                    concurrency = (number, 0, False)
            job = None
            continue
        if expect_child is not None:
            if level != 2:
                found.append((name, number, "the first line under %r is %d spaces in, not 2" % (top, level)))
            expect_child = None
        if concurrency is not None and level == concurrency[1] + 2 and re.match(r"^\s*group:", line):
            concurrency = (concurrency[0], concurrency[1], True)
        if top != "jobs":
            continue
        if level in (1, 3, 5):
            found.append((name, number, "a line %d spaces in under jobs" % level))
            continue
        if level == 2:
            if expect_property:
                found.append((name, job_ids[job], "job %r has nothing four spaces in" % job))
            if not JOB_LINE.match(line):
                found.append((name, number, "a job not written as `  <id>:` alone: %s" % line.strip()))
                job = None
                expect_property = False
                continue
            job = line.strip().split(":", 1)[0]
            if job in job_ids:
                found.append((name, number, "job %r given twice (first at line %d)" % (job, job_ids[job])))
            job_ids[job] = number
            expect_property = True
            continue
        if job is None:
            continue
        if expect_property:
            if level != 4:
                found.append((name, number, "job %r's first property is %d spaces in, not 4" % (job, level)))
            expect_property = False
        if level == 4:
            if not PROPERTY.match(line):
                found.append((name, number, "a job property that is not a bare key: %s" % line.strip()))
                continue
            if line.strip().startswith("<<:") or ANCHORED.match(line):
                found.append((name, number, "a merge key, anchor or alias at job property level"))
            key, _, value = line.strip().partition(":")
            value = value.split("#", 1)[0].strip()
            if key == "concurrency":
                if value:
                    found.append((name, number, "job concurrency given as a string; write a block with a "
                                                "`group:` line"))
                else:
                    concurrency = (number, 4, False)
    if expect_child is not None:
        found.append((name, expect_child, "a top-level block with nothing two spaces in"))
    if expect_property:
        found.append((name, job_ids[job], "job %r has nothing four spaces in" % job))
    close_concurrency()
    for key in ("on", "jobs"):
        if key not in tops:
            found.append((name, 0, "no bare top-level `%s:`" % key))
    return found


def workflow_files(root):
    directory = root / ".github" / "workflows"
    return sorted(list(directory.glob("*.yml")) + list(directory.glob("*.yaml")))


def check_files(paths):
    found = []
    for path in paths:
        found.extend(violations(str(path), Path(path).read_text()))
    return found


GOOD = """name: x
on:
  push:
    branches: [main]
concurrency:
  group: x-${{ github.sha }}
  cancel-in-progress: false
jobs:
  a:
    runs-on: [self-hosted]
    timeout-minutes: 2
    concurrency:
      group: a-${{ github.sha }}
    steps:
      - name: s
        run: |
          echo "  odd: indent, \\"quoted\\": key {a: b}"
         echo three in, inside the scalar
      - run: *alias_in_a_step
"""

# (name, text, a phrase the refusal says); each is VERIFY-3 A-1's plant or a near spelling.
BAD = (
    ("quoted on", GOOD.replace("on:\n", '"on":\n', 1), "not a bare name"),
    ("true for on", GOOD.replace("on:\n", "true:\n", 1), "no bare top-level `on:`"),
    ("quoted runs-on", GOOD.replace("    runs-on:", '    "runs-on":'), "not a bare key"),
    ("flow-style job", GOOD.split("jobs:\n")[0] + "jobs:\n  a: { runs-on: ubuntu-latest, steps: [ { run: x } ] }\n",
     "not written as"),
    ("four-space on", GOOD.replace("on:\n  push:\n    branches", "on:\n    push:\n        branches"), "not 2"),
    ("four-space jobs", "name: x\non:\n  push:\njobs:\n    a:\n        runs-on: x\n", "not 2"),
    ("three-space property", GOOD.replace("    timeout-minutes", "   timeout-minutes"), "3 spaces in"),
    ("string concurrency", GOOD.replace("concurrency:\n  group: x-${{ github.sha }}\n  cancel-in-progress: false\n",
                                        "concurrency: planted-${{ github.ref }}\n"), "given as a string"),
    ("concurrency with no group", GOOD.replace("  group: x-${{ github.sha }}\n", ""), "without a `group:`"),
    ("job concurrency string", GOOD.replace("    concurrency:\n      group: a-${{ github.sha }}\n",
                                            "    concurrency: a-${{ github.ref }}\n"), "given as a string"),
    ("job concurrency with no group", GOOD.replace("      group: a-${{ github.sha }}\n", "      x: y\n"),
     "without a `group:`"),
    ("merge key in a job", GOOD.replace("    timeout-minutes: 2\n", "    timeout-minutes: 2\n    <<: *defaults\n"),
     "not a bare key"),
    ("alias as a property", GOOD.replace("    runs-on: [self-hosted]", "    runs-on: *hosted"), "anchor or alias"),
    ("job twice", GOOD + "  a:\n    runs-on: x\n", "given twice"),
    ("on twice", GOOD + "on:\n  schedule:\n", "given twice"),
    ("no jobs", "name: x\non:\n  push:\n", "no bare top-level `jobs:`"),
    ("tab", GOOD.replace("    runs-on", "\truns-on"), "tab"),
)


def self_test():
    failures = []
    if violations("good.yml", GOOD):
        failures.append("the canonical fixture was refused: %r" % violations("good.yml", GOOD))
    for label, text, phrase in BAD:
        found = violations("bad.yml", text)
        if not any(phrase in reason for _, _, reason in found):
            failures.append("%s: not refused with %r (got %r)" % (label, phrase, found))
    with tempfile.TemporaryDirectory() as directory:
        if workflow_files(Path(directory)):
            failures.append("an empty tree has workflow files")
    for line in failures:
        print("check_workflow_shape self-test: " + line)
    if failures:
        return 1
    print("check_workflow_shape self-test: ok (canonical fixture passes; %d departures each refused)" % len(BAD))
    return 0


def main(argv):
    if argv[:1] == ["--self-test"]:
        return self_test()
    paths = [Path(a) for a in argv] or workflow_files(ROOT)
    if not paths:
        print("check_workflow_shape: no workflows found under .github/workflows: nothing was checked")
        return 1
    found = check_files(paths)
    for name, number, reason in found:
        print("%s:%d: %s" % (name, number, reason))
    if found:
        print("check_workflow_shape: %d departure(s) from the shape the workflow checkers read "
              "(tools/check_workflow_shape.py)" % len(found))
        return 1
    print("check_workflow_shape: every workflow is in the shape the workflow checkers read (%d files)" % len(paths))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
