#!/usr/bin/env python3
"""No `|| true` (or `|| :`) discards a command's status in CI shell code.

Not covered: a status dropped any other way (`; true`, `|| exit 0`, `set +e`,
a missing `set -e`), and files outside the scanned set below.

Scanned: .github/workflows/*.yml|yaml, .github/actions/**/*.yml|yaml,
.github/scripts/*, scripts/* (every file but *.txt, *.md, *.example) and
tools/**/*.sh. A line whose code (comment lines skipped) has `|| true`,
`|| :` or `|| /bin/true` is refused unless tools/or_true_allowlist.txt lists
that file and that exact line with a reason (tools/line_allowlist.py: a stale
or reasonless entry is refused too). The fix is explicit handling that keeps
a failure visible: `grep ... || [ $? -eq 1 ]` (no match is fine, an error is
not), `if [ -f log ]; then tail log; else echo "(no log)"; fi`, or recording
the status and failing at the end. The allow list is for statuses that are
genuinely irrelevant: cleanup after the verdict, a kill of a process that
may already have exited.

Zero files scanned is an error. Exit 0 clean, 1 refused.

Usage: check_or_true.py [--root DIR] [--allowlist FILE] | --self-test
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
ALLOWLIST = "tools/or_true_allowlist.txt"
MASK = re.compile(r"\|\|\s*(?:true|:|/bin/true|/usr/bin/true)(?=\s|$|[;)&|#\"'`}])")
SKIP_SUFFIXES = (".txt", ".md", ".example")


def scanned_files(root):
    files = []
    workflows = root / ".github" / "workflows"
    files += sorted(workflows.glob("*.yml")) + sorted(workflows.glob("*.yaml"))
    actions = root / ".github" / "actions"
    files += sorted(actions.rglob("*.yml")) + sorted(actions.rglob("*.yaml"))
    files += sorted(p for p in (root / ".github" / "scripts").glob("*") if p.is_file())
    files += sorted(p for p in (root / "scripts").glob("*")
                    if p.is_file() and not p.name.endswith(SKIP_SUFFIXES))
    files += sorted((root / "tools").rglob("*.sh"))
    return files


def flagged_lines(root, files):
    found = []
    for path in files:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            found.append((str(path.relative_to(root)), 0, f"unreadable: {error}"))
            continue
        for number, line in enumerate(text.splitlines(), 1):
            if line.lstrip().startswith("#"):
                continue
            if MASK.search(line):
                found.append((path.relative_to(root).as_posix(), number, line))
    return found


def run(root, allowlist_path):
    root = Path(root).resolve()
    files = scanned_files(root)
    if not files:
        print(f"check_or_true: 0 files scanned under {root}: nothing was checked")
        return 1
    entries, problems = line_allowlist.read(allowlist_path)
    refused, allowed, stale = line_allowlist.judge(flagged_lines(root, files), entries, allowlist_path)
    for path, number, text in refused:
        print(f"{path}:{number}: '|| true' discards the status; handle it explicitly or list it "
              f"with a reason in {ALLOWLIST}: {text.strip()}")
    for problem in problems + stale:
        print(problem)
    print(f"check_or_true: {len(files)} files scanned, {len(refused) + len(allowed)} masked statuses, "
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

    clean = {".github/workflows/a.yml": "jobs:\n  a:\n    steps:\n      - run: grep x f || [ $? -eq 1 ]\n"}
    planted = {".github/workflows/a.yml": "jobs:\n  a:\n    steps:\n      - run: grep x f || true\n"}
    case("clean workflow", clean, "", 0)
    case("planted '|| true' in a workflow", planted, "", 1)
    case("planted '|| :' in tools/*.sh", {"tools/x/y.sh": "grep x f || :\n"}, "", 1)
    case("planted '|| true' in scripts/", {"scripts/run": "kill $pid || true\n", **clean}, "", 1)
    case("a comment line is not code", {**clean, "tools/z.sh": "# never write || true here\n"}, "", 0)
    case("allow-listed with a reason", planted,
         ".github/workflows/a.yml\t- run: grep x f || true\tthe status is irrelevant here\n", 0)
    case("allow-list entry without a reason", planted, ".github/workflows/a.yml\t- run: grep x f || true\t\n", 1)
    case("stale allow-list entry", clean, ".github/workflows/a.yml\t- run: gone || true\tused to be here\n", 1)
    case("planted '|| : > file' in .github/scripts", {".github/scripts/s.sh": "grep x f > o || : > o\n"}, "", 1)
    case("zero files scanned", {"README": "nothing here\n"}, "", 1)
    if failures:
        print(f"check_or_true self-test: {len(failures)} failed: {failures}")
        return 1
    print("check_or_true self-test: 10 cases passed")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(ROOT))
    parser.add_argument("--allowlist")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv[1:])
    if args.self_test:
        return self_test()
    allowlist = args.allowlist or str(Path(args.root) / ALLOWLIST)
    return run(args.root, allowlist)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
