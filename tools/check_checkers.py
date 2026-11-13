#!/usr/bin/env python3
"""Every checker is registered in tools/checkers.txt, its self-test runs in CI, and zero inputs is red.

Not covered: whether a self-test plants the right violation (a reader's
judgement; the nightly planted-violations job applies plants to the tree),
and checkers outside the globs below.

tools/checkers.txt, tab-separated, '#' comment lines:

    <checker path>\t<self-test command>\t<zero-input command>\t<workflow file>\t<job id>\t<step name>

Refused:
  - a file matching tools/check_*, tools/run_*, .github/scripts/*.sh or
    scripts/*-selftest that has no row, and a row whose path does not exist;
  - a row with the wrong number of fields, an empty field, or a path twice;
  - a row whose workflow, job or step (by its `name:`) does not exist, or
    whose step's `run:` text does not contain the self-test command
    (whitespace-normalized), so the self-test is shown to run, not claimed;
  - a zero-input command that exits 0, exits 126/127 (could not run), prints
    a Python traceback, or takes over 60 s: zero inputs must be a named
    refusal. In the command, {empty} is a fresh empty directory and
    {emptyfile} a fresh empty file; it runs with `bash -c` from the
    repository root. `-` is accepted only for a scripts/*-selftest row (it
    is itself a self-test and reads no input).
  - zero rows.
--no-run skips the zero-input commands (the registry checks alone).

Usage: check_checkers.py [--root DIR] [--no-run] | --self-test
"""

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import workflow_jobs  # noqa: E402

ROOT = HERE.parent
REGISTRY = "tools/checkers.txt"
GLOBS = ["tools/check_*", "tools/run_*", ".github/scripts/*.sh", "scripts/*-selftest"]
FIELDS = 6
TIMEOUT = 60


def norm(text):
    return re.sub(r"\s+", " ", text).strip()


def read_registry(path):
    rows, problems = [], []
    try:
        lines = Path(path).read_text(encoding="utf-8").splitlines()
    except OSError as error:
        return [], [f"{path}: {error}"]
    seen = set()
    for number, raw in enumerate(lines, 1):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        fields = [f.strip() for f in raw.split("\t")]
        if len(fields) != FIELDS or not all(fields):
            problems.append(f"{REGISTRY}:{number}: want {FIELDS} non-empty tab-separated fields, got {len(fields)}")
            continue
        if fields[0] in seen:
            problems.append(f"{REGISTRY}:{number}: {fields[0]} registered twice")
            continue
        seen.add(fields[0])
        rows.append((number, fields))
    return rows, problems


def workflow_steps(root, cache, name):
    if name not in cache:
        path = root / ".github" / "workflows" / name
        try:
            cache[name] = workflow_jobs.parse(path.read_text(encoding="utf-8"))
        except (OSError, workflow_jobs.WorkflowError) as error:
            cache[name] = error
    return cache[name]


def run_zero(root, command):
    with tempfile.TemporaryDirectory() as tmp:
        empty = Path(tmp) / "empty"
        empty.mkdir()
        emptyfile = Path(tmp) / "emptyfile"
        emptyfile.write_text("")
        text = command.replace("{empty}", str(empty)).replace("{emptyfile}", str(emptyfile))
        try:
            result = subprocess.run(["bash", "-c", text], cwd=root, capture_output=True, text=True,
                                    timeout=TIMEOUT, stdin=subprocess.DEVNULL)
        except subprocess.TimeoutExpired:
            return None, f"did not finish in {TIMEOUT} s"
        output = (result.stdout + result.stderr).strip()
        last = output.splitlines()[-1] if output else "(no output)"
        return result.returncode, output if "Traceback (most recent call last)" in output else last


def check(root, no_run=False, out=sys.stdout):
    root = Path(root).resolve()
    rows, problems = read_registry(root / REGISTRY)
    if not rows and not problems:
        problems.append(f"{REGISTRY}: 0 rows: nothing was checked")
    registered = {fields[0] for _, fields in rows}
    present = set()
    for pattern in GLOBS:
        present |= {p.relative_to(root).as_posix() for p in root.glob(pattern) if p.is_file()}
    for path in sorted(present - registered):
        problems.append(f"{path}: a checker with no row in {REGISTRY}")
    cache = {}
    zero_jobs = []
    for number, (path, selftest, zero, workflow, job_id, step_name) in rows:
        where = f"{REGISTRY}:{number}: {path}"
        if not (root / path).is_file():
            problems.append(f"{where}: no such file")
        data = workflow_steps(root, cache, workflow)
        if isinstance(data, Exception):
            problems.append(f"{where}: workflow {workflow}: {data}")
        else:
            job = (data.get("jobs") or {}).get(job_id)
            if job is None:
                problems.append(f"{where}: {workflow} has no job {job_id}")
            else:
                matches = [s for s in job.get("steps") or [] if str(s.get("name", "")) == step_name]
                if not matches:
                    problems.append(f"{where}: {workflow} job {job_id} has no step named {step_name!r}")
                elif not any(norm(selftest) in norm(str(s.get("run", ""))) for s in matches):
                    problems.append(f"{where}: step {step_name!r} does not run the self-test {selftest!r}")
        if zero == "-":
            if not path.startswith("scripts/") or not path.endswith("-selftest"):
                problems.append(f"{where}: no zero-input command ('-' is for scripts/*-selftest rows only)")
        else:
            zero_jobs.append((where, zero))
    ran = 0
    if not no_run:
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(2, min(8, os.cpu_count() or 2))) as pool:
            results = list(pool.map(lambda job: (job[0], job[1], run_zero(root, job[1])), zero_jobs))
        for where, command, (code, detail) in results:
            ran += 1
            if code is None:
                problems.append(f"{where}: zero-input command {detail}: {command}")
            elif code == 0:
                problems.append(f"{where}: zero-input command PASSED (exit 0): {command}: {detail}")
            elif code in (126, 127):
                problems.append(f"{where}: zero-input command could not run (exit {code}): {command}: {detail}")
            elif "Traceback (most recent call last)" in detail:
                problems.append(f"{where}: zero-input command crashed (a traceback, not a refusal): {command}")
    for problem in problems:
        print(f"CHECKERS {problem}", file=out)
    print(f"check_checkers: {len(rows)} registered, {len(present)} matching {', '.join(GLOBS)}, "
          f"{ran} zero-input commands run, {len(problems)} problems", file=out)
    return 1 if problems else 0


def self_test():
    failures = []

    def case(label, files, registry, want, no_run=False):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, text in files.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text)
            (root / "tools").mkdir(exist_ok=True)
            (root / REGISTRY).write_text(registry)
            sink = open(os.devnull, "w")
            got = check(root, no_run=no_run, out=sink)
            sink.close()
            ok = (got == 0) == (want == 0)
            print(f"{'ok' if ok else 'FAIL'}: {label}: exit {got}")
            if not ok:
                failures.append(label)
                check(root, no_run=no_run)

    workflow = ("on:\n  push:\njobs:\n  lint:\n    runs-on: [self-hosted]\n    steps:\n"
                "      - name: Self-tests\n        run: |\n          python3 tools/check_a.py --self-test\n")
    checker = "import sys\nsys.exit(0 if sys.argv[1:] == ['--self-test'] else (1 if not any(__import__('os').scandir(sys.argv[1])) else 0))\n"
    files = {".github/workflows/ci.yml": workflow, "tools/check_a.py": checker}
    good = ("# header\ntools/check_a.py\tpython3 tools/check_a.py --self-test\tpython3 tools/check_a.py {empty}"
            "\tci.yml\tlint\tSelf-tests\n")
    case("a registered checker, self-test wired, zero input red", files, good, 0)
    case("an unregistered checker", {**files, "tools/check_b.py": checker}, good, 1, no_run=True)
    case("a registered file that does not exist", files,
         good + good.splitlines()[1].replace("check_a.py\t", "check_gone.py\t", 1) + "\n", 1, no_run=True)
    case("a self-test no step runs", files, good.replace("--self-test\tpython3", "--selftest\tpython3"), 1,
         no_run=True)
    case("a step that does not exist", files, good.replace("Self-tests", "Self tests"), 1, no_run=True)
    case("a zero-input command that passes", files, good.replace("{empty}", "{empty} || true"), 1)
    case("a zero-input command that cannot run", files, good.replace("python3 tools/check_a.py {empty}",
                                                                    "no-such-tool {empty}"), 1)
    case("a zero-input command that crashes", files, good.replace("python3 tools/check_a.py {empty}",
                                                                 "python3 -c 'raise SystemExit(1/0)'"), 1)
    case("'-' for a checker that is not a scripts/*-selftest", files,
         good.replace("python3 tools/check_a.py {empty}", "-"), 1, no_run=True)
    case("a row with five fields", files, "tools/check_a.py\tx\ty\tci.yml\tlint\n", 1, no_run=True)
    case("zero rows", {".github/workflows/ci.yml": workflow}, "# nothing\n", 1, no_run=True)
    if failures:
        print(f"check_checkers self-test: {len(failures)} failed: {failures}")
        return 1
    print("check_checkers self-test: 11 cases passed")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(ROOT))
    parser.add_argument("--no-run", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv[1:])
    if args.self_test:
        return self_test()
    return check(args.root, no_run=args.no_run)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
