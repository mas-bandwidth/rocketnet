#!/usr/bin/env python3
"""A workflow step that prints a trace, hash or digest compares it with something.

Not covered: measurements and counts printed without one of the keys below
(VERIFY-2 class 9 names those too), values compared in a later step or job
(the comparison must sit in the printing step), and printers not in PRINTERS.

A `run:` step in .github/workflows prints a value for comparison when its
text holds `trace=`, `hash=` or `sha256=`, or when it runs a program listed
in PRINTERS (the dst harness prints `trace=` on its PASS line). Such a step
must compare: its text holds `<key>=$` or `<key>=${{` (the printed value is
written against an expected value from a committed variable, as
`want="... trace=$DST_REFERENCE_TRACE"` does), or it sits in a job whose id
or name holds `report-only`, or tools/printed_values_allowlist.txt lists it
(`<workflow path>\t<job id>: <step name>\t<reason>`; tools/line_allowlist.py
refuses a stale or reasonless entry). Zero workflow files is an error.

Usage: check_printed_compared.py [--root DIR] [--allowlist FILE] | --self-test
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import line_allowlist  # noqa: E402
import workflow_jobs  # noqa: E402

ROOT = HERE.parent
ALLOWLIST = "tools/printed_values_allowlist.txt"
KEYS = r"(?:trace|hash|sha256)"
PRINTS = re.compile(r"(?<![A-Za-z0-9_])" + KEYS + r"=")
# an expected value from a variable (`trace=$REF`, `trace=${REF}`, `trace=${{ ... }}`;
# never `$(`, which is the printed value itself), or a digest checker
COMPARES = re.compile(r"(?<![A-Za-z0-9_])" + KEYS + r"=\$(?:\{\{|\{?[A-Za-z_])|\bsha256sum\s+(?:--check|-c)\b"
                      r"|\bshasum\s+-a\s*256\s+(?:--check|-c)\b")
# programs that print one of the keys themselves: (pattern, what it prints).
# dst prints trace= on a --seeds run; an --outage run prints rows= instead.
PRINTERS = [
    (re.compile(r"^(?!.*--outage).*\bdst(?:\$DST_EXE|\.exe)?\"?\s+--seeds\b", re.M), "the dst harness prints trace="),
]


def steps(root):
    directory = Path(root) / ".github" / "workflows"
    files = sorted(directory.glob("*.yml")) + sorted(directory.glob("*.yaml"))
    found, problems = [], []
    for path in files:
        rel = path.relative_to(root).as_posix()
        try:
            data = workflow_jobs.parse(path.read_text(encoding="utf-8"))
        except workflow_jobs.WorkflowError as error:
            problems.append(f"{rel}: cannot be read: {error}")
            continue
        for job_id, job in (data.get("jobs") or {}).items():
            job_name = str(job.get("name") or "")
            for index, step in enumerate(job.get("steps") or [], 1):
                if "run" in step:
                    found.append((rel, job_id, job_name, str(step.get("name") or f"step {index}"), str(step["run"])))
    return files, found, problems


def judge(text):
    """None when the step prints nothing for comparison, else what it prints."""
    match = PRINTS.search(text)
    if match:
        return f"prints {match.group(0)}"
    for pattern, what in PRINTERS:
        if pattern.search(text):
            return what
    return None


def run(root, allowlist_path):
    root = Path(root).resolve()
    files, found, problems = steps(root)
    if not files:
        print(f"check_printed_compared: 0 workflow files under {root}: nothing was checked")
        return 1
    flagged = []
    printing = compared = report_only = 0
    for rel, job_id, job_name, step_name, text in found:
        what = judge(text)
        if what is None:
            continue
        printing += 1
        if COMPARES.search(text):
            compared += 1
            continue
        if "report-only" in job_id or "report-only" in job_name:
            report_only += 1
            continue
        flagged.append((rel, 0, f"{job_id}: {step_name}", what))
    entries, list_problems = line_allowlist.read(allowlist_path)
    refused, allowed, stale = line_allowlist.judge([(p, n, t) for p, n, t, _ in flagged], entries, allowlist_path)
    whats = {(p, t): w for p, _, t, w in flagged}
    for path, _, text in refused:
        print(f"PRINTED-NOT-COMPARED {path}: {text}: {whats[(path, text)]} and compares nothing "
              f"(write it against a committed value, name the job report-only, or list it in {ALLOWLIST})")
    for problem in problems + list_problems + stale:
        print(problem)
    print(f"check_printed_compared: {len(files)} workflow files, {len(found)} run steps, {printing} print a value: "
          f"{compared} compared, {report_only} report-only, {len(allowed)} allow-listed, {len(refused)} refused")
    return 1 if refused or problems or list_problems or stale else 0


def self_test():
    me = Path(__file__).resolve()
    failures = []

    def case(label, workflow, allowlist, want):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            if workflow is not None:
                (root / ".github" / "workflows").mkdir(parents=True)
                (root / ".github" / "workflows" / "a.yml").write_text(workflow)
            (root / "allow.txt").write_text(allowlist)
            result = subprocess.run([sys.executable, str(me), "--root", str(root), "--allowlist",
                                     str(root / "allow.txt")], capture_output=True, text=True)
            ok = (result.returncode == 0) == (want == 0)
            print(f"{'ok' if ok else 'FAIL'}: {label}: exit {result.returncode}")
            if not ok:
                failures.append(label)
                print(result.stdout + result.stderr)

    def wf(run, job="sweep", name="sweep"):
        return (f"on:\n  push:\njobs:\n  {job}:\n    name: {name}\n    runs-on: [self-hosted]\n    steps:\n"
                f"      - name: s\n        run: |\n          {run}\n")

    case("a printed hash compared with a committed value",
         wf('want="hash=$PINNED"; [ "$(./tool)" = "$want" ]'), "", 0)
    case("planted: a printed hash compared with nothing", wf('echo "hash=$(./tool)"'), "", 1)
    case("planted: the dst harness run with no reference", wf('"$DST_BIN/dst$DST_EXE" --seeds 1-50 | tee x.log'), "", 1)
    case("dst with its reference", wf('./bin/dst --seeds 1-50; want="PASS trace=$REF"'), "", 0)
    case("a report-only job", wf('echo "sha256=$(sha256sum f)"', job="digest-report-only"), "", 0)
    case("allow-listed with a reason", wf('echo "trace=$(./t)" | cut -c1'),
         ".github/workflows/a.yml\tsweep: s\tthe range rolls nightly; nothing to compare\n", 0)
    case("a step printing nothing", wf("make test"), "", 0)
    case("stale allow-list entry", wf("make test"), ".github/workflows/a.yml\tsweep: s\tgone\n", 1)
    case("zero workflow files", None, "", 1)
    if failures:
        print(f"check_printed_compared self-test: {len(failures)} failed: {failures}")
        return 1
    print("check_printed_compared self-test: 9 cases passed")
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
