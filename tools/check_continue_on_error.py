#!/usr/bin/env python3
"""No `continue-on-error` in a workflow or action outside a reasoned allow list.

Not covered: a result dropped any other way (`if: always()` on a step whose
outcome nothing reads, `|| true`: tools/check_or_true.py), and files outside
.github/workflows and .github/actions.

A job or step with `continue-on-error` concludes success whatever it did, so
a red result is read by nobody (VERIFY-2 class 1). Every line whose key is
`continue-on-error:` in .github/workflows/*.yml|yaml or
.github/actions/**/*.yml|yaml is refused unless
tools/continue_on_error_allowlist.txt lists that file and that exact line
with a reason (tools/line_allowlist.py: a stale or reasonless entry is
refused too). The list is empty today. Zero workflow files is an error.

Usage: check_continue_on_error.py [--root DIR] [--allowlist FILE] | --self-test
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import line_allowlist  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
ALLOWLIST = "tools/continue_on_error_allowlist.txt"
KEY = re.compile(r"^\s*(?:-\s+)?[\"']?continue-on-error[\"']?\s*:")


def scanned_files(root):
    workflows = root / ".github" / "workflows"
    actions = root / ".github" / "actions"
    return (sorted(workflows.glob("*.yml")) + sorted(workflows.glob("*.yaml"))
            + sorted(actions.rglob("*.yml")) + sorted(actions.rglob("*.yaml")))


def run(root, allowlist_path):
    root = Path(root).resolve()
    files = scanned_files(root)
    if not files:
        print(f"check_continue_on_error: 0 workflow files under {root}: nothing was checked")
        return 1
    flagged = []
    for path in files:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if KEY.match(line):
                flagged.append((path.relative_to(root).as_posix(), number, line))
    entries, problems = line_allowlist.read(allowlist_path)
    refused, allowed, stale = line_allowlist.judge(flagged, entries, allowlist_path)
    for path, number, text in refused:
        print(f"{path}:{number}: continue-on-error hides this result from every reader; remove it or "
              f"list it with a reason in {ALLOWLIST}: {text.strip()}")
    for problem in problems + stale:
        print(problem)
    print(f"check_continue_on_error: {len(files)} workflow files, {len(flagged)} continue-on-error, "
          f"{len(allowed)} allow-listed, {len(refused)} refused, {len(problems) + len(stale)} allow-list problems")
    return 1 if refused or problems or stale else 0


def self_test():
    me = Path(__file__).resolve()
    failures = []

    def case(label, files, allowlist, want):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, text in files.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text)
            (root / "allow.txt").write_text(allowlist)
            result = subprocess.run([sys.executable, str(me), "--root", str(root),
                                     "--allowlist", str(root / "allow.txt")],
                                    capture_output=True, text=True)
            ok = (result.returncode == 0) == (want == 0)
            print(f"{'ok' if ok else 'FAIL'}: {label}: exit {result.returncode}")
            if not ok:
                failures.append(label)
                print(result.stdout + result.stderr)

    clean = {".github/workflows/a.yml": "jobs:\n  a:\n    steps:\n      - run: make test\n"}
    step = {".github/workflows/a.yml": "jobs:\n  a:\n    steps:\n      - run: make test\n"
                                       "        continue-on-error: true\n"}
    job = {".github/workflows/a.yml": "jobs:\n  a:\n    continue-on-error: ${{ matrix.experimental }}\n"
                                      "    steps:\n      - run: make test\n"}
    case("clean workflow", clean, "", 0)
    case("planted continue-on-error on a step", step, "", 1)
    case("planted continue-on-error on a job (expression)", job, "", 1)
    case("planted in a composite action", {**clean, ".github/actions/x/action.yml":
                                           "runs:\n  steps:\n    - continue-on-error: true\n      run: x\n"}, "", 1)
    case("allow-listed with a reason", step,
         ".github/workflows/a.yml\tcontinue-on-error: true\tthe step only uploads a report\n", 0)
    case("allow-list entry without a reason", step, ".github/workflows/a.yml\tcontinue-on-error: true\t\n", 1)
    case("stale allow-list entry", clean, ".github/workflows/a.yml\tcontinue-on-error: true\tgone\n", 1)
    case("zero workflow files", {"README": "x\n"}, "", 1)
    if failures:
        print(f"check_continue_on_error self-test: {len(failures)} failed: {failures}")
        return 1
    print("check_continue_on_error self-test: 8 cases passed")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(ROOT))
    parser.add_argument("--allowlist")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv[1:])
    if args.self_test:
        return self_test()
    return run(args.root, args.allowlist or str(Path(args.root) / ALLOWLIST))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
