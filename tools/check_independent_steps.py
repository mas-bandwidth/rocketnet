#!/usr/bin/env python3
"""Every check step of a listed job runs whatever an earlier step did.

GitHub skips every later step of a job once one step fails, unless the later
step's `if:` says otherwise. In a job whose steps are separate checks (the
lint job), that means one red check hides the verdict of every check after
it: on 85 main commits the format step was red and 17 checks after it never
ran (VERIFY-3 A-11). So each step of a job named in INDEPENDENT_JOBS, other
than the setup steps that check the tree out, must carry an `if:` holding
`!cancelled()` (or `always()`); the job still fails when any step fails.

Usage: check_independent_steps.py [--self-test | --root DIR]
Prints one line per offending step and exits 1; exits 1 when a listed job is
not found or has no check step, so a renamed job is never "nothing to check".
"""

import os
import re
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# workflow file (relative to the root) -> jobs whose steps are independent checks
INDEPENDENT_JOBS = {
    ".github/workflows/ci.yml": ["lint", "lint-source", "lint-models", "lint-tools"],
}

SETUP_USES = re.compile(r"^(?:actions/checkout@|\./\.github/actions/ccache\b)")
RUNS_ANYWAY = re.compile(r"!\s*cancelled\(\)|\balways\(\)")


def job_steps(lines, job):
    """Return [(line, name, if_text, uses)] for each step of a two-space job."""
    steps = []
    in_job = False
    in_steps = False
    cur = None
    for lineno, raw in enumerate(lines, 1):
        line = raw.rstrip("\n")
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        indent = len(line) - len(line.lstrip())
        if indent <= 2:
            if cur is not None:
                steps.append(cur)
                cur = None
            in_job = indent == 2 and stripped == job + ":"
            in_steps = False
            continue
        if not in_job:
            continue
        if indent == 4:
            if cur is not None:
                steps.append(cur)
                cur = None
            in_steps = stripped == "steps:"
            continue
        if not in_steps:
            continue
        if indent == 6 and stripped.startswith("- "):
            if cur is not None:
                steps.append(cur)
            cur = [lineno, "", "", ""]
            stripped = stripped[2:].strip()
            indent = 8
        if cur is None or indent != 8:
            continue
        m = re.match(r"^(name|if|uses):\s*(.*)$", stripped)
        if m:
            cur[{"name": 1, "if": 2, "uses": 3}[m.group(1)]] = m.group(2).strip()
    if cur is not None:
        steps.append(cur)
    return steps


def check(root):
    failures = []
    checked = 0
    for workflow, jobs in sorted(INDEPENDENT_JOBS.items()):
        path = os.path.join(root, workflow)
        try:
            with open(path, encoding="utf-8") as fh:
                lines = fh.readlines()
        except OSError as error:
            failures.append("%s: %s" % (workflow, error))
            continue
        for job in jobs:
            steps = job_steps(lines, job)
            checks = [s for s in steps if not SETUP_USES.match(s[3])]
            if not checks:
                failures.append("%s: job %s has no check step (renamed or moved?)" % (workflow, job))
                continue
            for lineno, name, if_text, _uses in checks:
                checked += 1
                if not RUNS_ANYWAY.search(if_text):
                    failures.append("%s:%d: job %s step %r runs only while every earlier step passed; "
                                    "give it `if: ${{ !cancelled() }}` (joined with any condition it has)"
                                    % (workflow, lineno, job, name or "(unnamed)"))
    return checked, failures


GOOD = """jobs:
  lint:
    runs-on: [self-hosted]
    steps:
      - uses: actions/checkout@abc  # v4
      - uses: ./.github/actions/ccache
      - name: first
        if: ${{ !cancelled() }}
        run: true
      - name: second
        if: ${{ !cancelled() && inputs.full }}
        run: true
      - if: always()
        name: third
        run: true
  other:
    steps:
      - name: free
        run: true
""" + "".join(
    "  %s:\n    steps:\n      - name: only\n        if: ${{ !cancelled() }}\n        run: true\n" % job
    for job in ("lint-source", "lint-models", "lint-tools"))


def self_test():
    cases = [
        ("independent steps", GOOD, True, None),
        ("a step with no if", GOOD.replace("        if: ${{ !cancelled() }}\n", ""), False, "'first'"),
        ("a step whose if lacks !cancelled()", GOOD.replace("!cancelled() && inputs.full", "inputs.full"), False, "'second'"),
        ("the job renamed away", GOOD.replace("  lint:", "  lint2:"), False, "has no check step"),
        ("only setup steps", GOOD.split("      - name: first")[0], False, "has no check step"),
    ]
    bad = 0
    for title, text, want_ok, want_text in cases:
        with tempfile.TemporaryDirectory() as root:
            os.makedirs(os.path.join(root, ".github", "workflows"))
            with open(os.path.join(root, ".github", "workflows", "ci.yml"), "w") as fh:
                fh.write(text)
            _checked, failures = check(root)
        ok = not failures
        if ok != want_ok or (want_text and not any(want_text in f for f in failures)):
            print("SELF-TEST FAIL: %s: failures=%r" % (title, failures))
            bad += 1
    # the program itself, on a planted tree: exit 1 and the failure line
    with tempfile.TemporaryDirectory() as root:
        os.makedirs(os.path.join(root, ".github", "workflows"))
        with open(os.path.join(root, ".github", "workflows", "ci.yml"), "w") as fh:
            fh.write(GOOD.replace("        if: ${{ !cancelled() }}\n", ""))
        import subprocess
        result = subprocess.run([sys.executable, os.path.abspath(__file__), "--root", root],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode != 1 or "'first'" not in result.stdout:
            print("SELF-TEST FAIL: the program on a planted tree exited %d: %s" % (result.returncode, result.stdout))
            bad += 1
    if bad:
        return 1
    print("check_independent_steps self-test: ok (%d cases)" % (len(cases) + 1))
    return 0


def main(argv):
    root = ROOT
    if argv[:1] == ["--self-test"]:
        return self_test()
    if argv[:1] == ["--root"] and len(argv) == 2:
        root = argv[1]
    checked, failures = check(root)
    for failure in failures:
        print(failure)
    if failures:
        print("check_independent_steps: %d step(s) would be skipped after an earlier red step" % len(failures))
        return 1
    print("check_independent_steps: %d check steps each run and report on their own" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
