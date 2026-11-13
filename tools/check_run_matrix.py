#!/usr/bin/env python3
"""Every verification cell the tree can run is declared in tools/verification_matrix.tsv with the job that runs it.

The run matrix is the set of REQUIRED cells: each test executable, suite,
transport pin, build type, sanitizer, compiler, compile-time value and option
the tree has, crossed only where the axis changes what runs (the reason column
says why). Refused:

  (a) inventory: something exists in the tree and no cell names it:
      - an add_executable target (CMakeLists.txt, examples/, cmake/, tools/*/);
      - a transport row of sdk/rocketnet_drivers.inc: on every platform its
        mask names, the functional suite must have a Release and a Debug cell
        with it pinned as server (and as client where its client rank is not 0);
      - a CMake option() (as a non-default value in a cell's config, or the
        dev / sanitizer columns for ROCKETNET_DEV and ROCKETNET_SANITIZE_ADDRESS),
        unless it is in OPTIONS_WITHOUT_CELL with its reason;
      - each bound of a documented range: `CACHE STRING "... (A..B ...)"` in
        CMakeLists.txt and `#error X must be between A and B` in sdk/rocketnet.h;
      - a fuzz harness tests/fuzz/rocketnet_fuzz*.cpp not named in
        .github/workflows/fuzz-nightly.yml, a DST schedule in tests/dst.cpp's
        usage not run as `--schedule <name>` in nightly-dst.yml, a CBMC proof
        proofs/cbmc/proof_*.c named in no workflow and not in proofs/cbmc/Makefile;
      - a scripts/* file (not *.example, *.txt) or tools/check_* file that is
        neither a cell's artefact nor registered in tools/checkers.txt;
  (b) wiring: a cell names a workflow, job or step (name prefix; '*' any step)
      that does not exist, or whose run/env/matrix text does not hold every
      word of the cell's needle; a malformed row, a duplicate cell; a tier that
      is not per-commit, nightly, manual (with its command as reason),
      inside:<artefact that has cells> or owed:<YYYY-MM-DD>; an owed cell whose
      date has passed (it was owed and is still run nowhere);
  (c) --remote: the named job's most recent completed run on main (ci.yml jobs
      also in nightly-ci.yml's run, where the full tier calls ci.yml) was
      skipped, cancelled or missing, the named step did not run, or a suite
      step's log shows no executed case (`*** ALL TESTS PASSED ***` or a
      `check failed` line).

Every owed cell is printed (OWED) on every run, so nothing runs nowhere silently.

Not covered: whether a cell's command is the right command for its axis
beyond the needle words (a reader's judgement); TLA+ cases (tools/check_tla_records.py
and tlc.yml hold them); the run-matrix rows' reasons.

Usage: check_run_matrix.py [--root DIR] [--today YYYY-MM-DD] [--remote [--repo OWNER/NAME]] | --self-test
"""

import argparse
import datetime
import io
import json
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
REGISTRY = "tools/verification_matrix.tsv"
FIELDS = 7
REPO = "mas-bandwidth/rocketnet"

# add_executable names (literal, or the loop variable as written) -> the artefact column of their cells
ARTEFACT_OF_TARGET = {
    "test": "unit",
    "happy_server": "happy_path",
    "happy_client": "happy_path",
    "client": "client+server(tests/client_server)",
    "server": "client+server(tests/client_server)",
    "fuzz_replay_${harness}": "fuzz_replay(all harnesses)",
    "example_${example_name}": "examples",
    "${consumer}": "build+consumer",
    "${test_target}": "test_lifecycle",
    "${rocketnet_stress_target}": "test_channels_stress",
    "xver": "xver(portable,gso,io_uring)",
    "instruction_count": "instruction_count",
}
# options whose other value changes no code that runs, with the reason
OPTIONS_WITHOUT_CELL = {
    "ROCKETNET_BUILD_TESTS": "OFF builds no test: nothing to run",
    "ROCKETNET_BUILD_EXAMPLES": "OFF builds fewer targets; the library is unchanged",
    "ROCKETNET_BUILD_TOOLS": "OFF builds fewer targets; the library is unchanged",
    "ROCKETNET_WARNINGS_AS_ERRORS": "OFF only lets a warning through; the per-commit build holds zero warnings",
    "ROCKETNET_STRICT_WARNINGS": "OFF only removes warning flags; the per-commit build holds zero warnings",
}
PLATFORMS = {
    "ROCKETNET_TRANSPORT_ALL_PLATFORMS": ["linux", "macos", "windows"],
    "ROCKETNET_TRANSPORT_LINUX": ["linux"],
    "ROCKETNET_TRANSPORT_IO_URING_PLATFORMS": ["linux"],
    "ROCKETNET_TRANSPORT_MAC": ["macos"],
    "ROCKETNET_TRANSPORT_WINDOWS": ["windows"],
}
SUITES = {"test", "functional", "functional_production"}
EXECUTED = re.compile(r"\*\*\* ALL TESTS PASSED \*\*\*|check failed")


def norm(text):
    return re.sub(r"\s+", " ", text).strip()


class Row:
    def __init__(self, number, fields):
        self.number = number
        self.cell, self.workflow, self.job, self.step, self.needle, self.tier, self.reason = fields
        parts = self.cell.split("|")
        self.axes = parts if len(parts) == 9 else None

    @property
    def artefact(self):
        return self.axes[0] if self.axes else ""

    def where(self):
        return f"{REGISTRY}:{self.number}: {self.cell}"


def read_registry(path):
    rows, problems, seen = [], [], set()
    try:
        lines = Path(path).read_text(encoding="utf-8").splitlines()
    except OSError as error:
        return [], [f"{path}: {error}"]
    for number, raw in enumerate(lines, 1):
        if not raw.strip() or raw.startswith("#"):
            continue
        fields = raw.split("\t")
        if len(fields) != FIELDS or not all(f.strip() for f in fields):
            problems.append(f"{REGISTRY}:{number}: want {FIELDS} non-empty tab-separated fields, got {len(fields)}")
            continue
        row = Row(number, [f.strip() for f in fields])
        if row.axes is None:
            problems.append(f"{row.where()}: a cell has nine '|' fields")
            continue
        if row.cell in seen:
            problems.append(f"{row.where()}: declared twice")
            continue
        seen.add(row.cell)
        rows.append(row)
    if not rows and not problems:
        problems.append(f"{REGISTRY}: 0 rows: nothing was checked")
    return rows, problems


def read(root, rel):
    try:
        return (root / rel).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def inventory(root, rows):
    problems = []
    artefacts = {r.artefact for r in rows}
    # targets
    cmake_files = [root / "CMakeLists.txt", root / "examples" / "CMakeLists.txt"]
    cmake_files += sorted((root / "cmake").glob("*.cmake")) + sorted((root / "tools").glob("*/CMakeLists.txt"))
    targets = set()
    for path in cmake_files:
        if path.is_file():
            targets |= set(re.findall(r"add_executable\(\s*([A-Za-z0-9_${}]+)", path.read_text(encoding="utf-8")))
    if not targets:
        problems.append("inventory: no add_executable target found (is --root the repository?)")
    for target in sorted(targets):
        name = ARTEFACT_OF_TARGET.get(target, target)
        if name not in artefacts:
            problems.append(f"inventory: target {target} has no cell (artefact {name!r})")
    # transports
    drivers = read(root, "sdk/rocketnet_drivers.inc")
    rx = re.compile(r"^ROCKETNET_TRANSPORT\(\s*\w+,\s*\"(\w+)\",\s*\w+,\s*\w+,\s*(\w+),\s*(\([^)]*\)|\w+),\s*(\w+)\s*\)", re.M)
    transports = rx.findall(drivers)
    if not transports:
        problems.append("inventory: no transport row read from sdk/rocketnet_drivers.inc")
    have = {(r.axes[1], r.axes[4], r.axes[6], r.axes[7]) for r in rows if r.artefact == "functional"}
    for name, mask, server_rank, client_rank in transports:
        for os_name in PLATFORMS.get(mask, []):
            for build in ("Release", "Debug"):
                if not any(o == os_name and b == build and s == "none" and p.split("~")[0] == name for o, b, s, p in have):
                    problems.append(f"inventory: transport {name} has no {build} functional cell on {os_name} with it as server")
                if client_rank not in ("0",) and not any(
                        o == os_name and b == build and s == "none" and p.split("~")[1] == name for o, b, s, p in have):
                    problems.append(f"inventory: transport {name} has no {build} functional cell on {os_name} with it as client")
        if mask not in PLATFORMS:
            problems.append(f"inventory: transport {name} has a platform mask {mask} this checker does not know")
    # options
    configs = " ".join(r.axes[8] for r in rows)
    cmake = read(root, "CMakeLists.txt")
    for option in re.findall(r"^option\((\w+)", cmake, re.M):
        if option in OPTIONS_WITHOUT_CELL:
            continue
        if option == "ROCKETNET_DEV":
            ok = any(r.axes[5] == "devOFF" for r in rows)
        elif option == "ROCKETNET_SANITIZE_ADDRESS":
            ok = any(r.axes[6] == "asan" for r in rows)
        else:
            ok = f"{option}=" in configs
        if not ok:
            problems.append(f"inventory: CMake option {option} has no cell at its other value")
    # documented ranges
    bounds = []
    for name, low, high in re.findall(r"set\((\w+)\s+\S+\s+CACHE STRING \"[^\"]*?\((\d+)\.\.(\d+)", cmake):
        bounds.append((name.replace("ROCKETNET_", ""), low, high))
    for name, low, high in re.findall(r"#error (\w+) must be between (\d+) and (\d+)", read(root, "sdk/rocketnet.h")):
        bounds.append((name.replace("ROCKETNET_", ""), low, high))
    defaults = dict(re.findall(r"set\(ROCKETNET_(\w+)\s+(\d+)\s+CACHE", cmake))
    defaults.update(re.findall(r"#define ROCKETNET_(MAX_CHANNELS|MAX_CLIENTS)\s+(\d+)", read(root, "sdk/rocketnet.h")))
    for name, low, high in bounds:
        for value in (low, high):
            if defaults.get(name) == value:
                continue
            if f"{name}={value}" not in configs:
                problems.append(f"inventory: documented bound {name}={value} has no cell")
    # fuzz harnesses, DST schedules, CBMC proofs
    fuzz = read(root, ".github/workflows/fuzz-nightly.yml")
    for harness in sorted((root / "tests" / "fuzz").glob("rocketnet_fuzz*.cpp")):
        if harness.stem not in fuzz and f"fuzz:{harness.stem}" not in artefacts:
            problems.append(f"inventory: fuzz harness {harness.stem} is not in fuzz-nightly.yml and has no cell")
    usage = re.search(r"--schedule ([a-z0-9|-]+)\]", read(root, "tests/dst.cpp"))
    dst = read(root, ".github/workflows/nightly-dst.yml")
    for schedule in (usage.group(1).split("|") if usage else []):
        if f"--schedule {schedule}" not in dst:
            problems.append(f"inventory: DST schedule {schedule} is not run in nightly-dst.yml")
    workflows_text = " ".join(p.read_text(encoding="utf-8") for p in sorted((root / ".github" / "workflows").glob("*.yml")))
    cbmc_make = read(root, "proofs/cbmc/Makefile")
    for proof in sorted((root / "proofs" / "cbmc").glob("proof_*.c")):
        if proof.stem not in workflows_text and proof.stem not in cbmc_make:
            problems.append(f"inventory: CBMC proof {proof.stem} is run by no workflow and not in proofs/cbmc/Makefile")
    # scripts and checkers
    registered = set()
    for line in read(root, "tools/checkers.txt").splitlines():
        if line and not line.startswith("#"):
            registered.add(line.split("\t")[0])
    files = [p for p in sorted((root / "scripts").glob("*")) if p.is_file()]
    files += [p for p in sorted((root / "tools").glob("check_*")) if p.is_file()]
    for path in files:
        rel = path.relative_to(root).as_posix()
        if path.suffix in (".example", ".txt") or path.name.endswith(".inventory.example"):
            continue
        if rel not in artefacts and rel not in registered:
            problems.append(f"inventory: {rel} is neither a cell nor registered in tools/checkers.txt")
    return problems, len(targets), len(transports)


def wiring(root, rows, today):
    problems, cache = [], {}
    artefacts = {r.artefact for r in rows}
    owed = []
    for row in rows:
        tier = row.tier
        if tier.startswith("owed:"):
            try:
                due = datetime.date.fromisoformat(tier[5:])
            except ValueError:
                problems.append(f"{row.where()}: owed date {tier[5:]!r} is not YYYY-MM-DD")
                continue
            owed.append(row)
            if due < today:
                problems.append(f"{row.where()}: owed since {due} and still run by no workflow")
            if row.workflow != "-":
                problems.append(f"{row.where()}: an owed cell names a workflow; make it nightly or per-commit")
            continue
        if tier.startswith("inside:"):
            if tier[7:] not in artefacts:
                problems.append(f"{row.where()}: inside {tier[7:]!r}, which has no cell")
            continue
        if tier == "manual":
            if len(row.reason) < 20:
                problems.append(f"{row.where()}: a manual cell gives the command and the missing hardware as its reason")
            continue
        if tier not in ("per-commit", "nightly"):
            problems.append(f"{row.where()}: tier {tier!r} is not per-commit, nightly, manual, inside:<a> or owed:<date>")
            continue
        if row.workflow not in cache:
            try:
                cache[row.workflow] = workflow_jobs.parse(read(root, f".github/workflows/{row.workflow}") or "{}")
            except workflow_jobs.WorkflowError as error:
                cache[row.workflow] = error
        data = cache[row.workflow]
        if isinstance(data, Exception) or not isinstance(data, dict) or not data.get("jobs"):
            problems.append(f"{row.where()}: workflow {row.workflow} does not exist or has no jobs")
            continue
        job = data["jobs"].get(row.job)
        if not isinstance(job, dict):
            problems.append(f"{row.where()}: {row.workflow} has no job {row.job}")
            continue
        steps = [s for s in job.get("steps") or [] if isinstance(s, dict)]
        if row.step != "*":
            steps = [s for s in steps if str(s.get("name", "")).startswith(row.step)]
            if not steps:
                problems.append(f"{row.where()}: {row.workflow} job {row.job} has no step named {row.step!r}")
                continue
        context = norm(json.dumps(job.get("env") or {}) + " " + json.dumps(job.get("strategy") or {}))
        words = [w.strip() for w in row.needle.split("&&") if w.strip()]
        if not any(all(re.search(r"(?<![A-Za-z0-9_])" + re.escape(w) + r"(?![A-Za-z0-9])",
                                 norm(str(s.get("run", "")) + " " + json.dumps(s.get("env") or {}) + " " + context))
                       for w in words) for s in steps):
            problems.append(f"{row.where()}: no step {row.step!r} of {row.workflow} job {row.job} runs {row.needle!r}")
    return problems, owed


def gh(path):
    command = ["gh", "api", path]
    if path.endswith("/logs"):
        command.insert(2, "--allow-escape-sequences")  # job logs carry the runner's colour codes
    result = subprocess.run(command, capture_output=True, text=True, timeout=120, errors="replace")
    if result.returncode != 0:
        raise RuntimeError(f"gh api {path}: {result.stderr.strip()[:200]}")
    return result.stdout


def remote(root, rows, repo, fetch=gh, seen=None):
    problems, runs, logs = [], {}, {}

    def latest(workflow):
        if workflow not in runs:
            try:
                data = json.loads(fetch(f"repos/{repo}/actions/workflows/{workflow}/runs?branch=main&status=completed&per_page=1"))
            except RuntimeError as error:
                if "HTTP 404" not in str(error):
                    raise
                data = {}  # the workflow is not on main yet: each of its cells is reported as not run

            run = (data.get("workflow_runs") or [None])[0]
            jobs = []
            if run:
                jobs = json.loads(fetch(f"repos/{repo}/actions/runs/{run['id']}/jobs?per_page=100")).get("jobs") or []
            runs[workflow] = (run, jobs)
        return runs[workflow]

    checked = 0
    for row in rows:
        if row.tier not in ("per-commit", "nightly"):
            continue
        try:
            workflow = workflow_jobs.Workflow(root / ".github" / "workflows" / row.workflow)
            job = next(j for j in workflow.jobs if j.id == row.job)
            pattern = re.compile(job.pattern())
        except Exception as error:  # the wiring check names the cause
            problems.append(f"{row.where()}: cannot read {row.workflow} job {row.job}: {error}")
            continue
        candidates = [row.workflow] + (["nightly-ci.yml"] if row.workflow == "ci.yml" else [])
        found = []
        for name in candidates:
            run, jobs = latest(name)
            for j in jobs:
                display = j.get("name", "")
                if pattern.fullmatch(display) or pattern.fullmatch(display.split(" / ", 1)[-1]):
                    found.append((run, j))
        if not found:
            problems.append(f"{row.where()}: no run on main of {row.workflow} has job {row.job}")
            continue
        ran = [(run, j) for run, j in found if j.get("conclusion") in ("success", "failure")]
        if not ran:
            problems.append(f"{row.where()}: the latest {row.job} on main was {found[0][1].get('conclusion')} "
                            f"(run {found[0][0]['id']}), not run")
            continue
        steps_ran = []
        for run, j in ran:
            for step in j.get("steps") or []:
                if (row.step == "*" or step.get("name", "").startswith(row.step)) and step.get("conclusion") in ("success", "failure"):
                    steps_ran.append((run, j))
                    break
        if not steps_ran:
            problems.append(f"{row.where()}: step {row.step!r} of {row.job} did not run in run {ran[0][0]['id']}")
            continue
        if row.needle.split(" && ")[0] in SUITES:
            run, j = steps_ran[0]
            if j["id"] not in logs:
                logs[j["id"]] = fetch(f"repos/{repo}/actions/jobs/{j['id']}/logs")
            if not EXECUTED.search(logs[j["id"]]):
                problems.append(f"{row.where()}: job {j.get('name')} (run {run['id']}) shows no executed case")
                continue
        checked += 1
        if seen is not None:  # the run each cell was seen in, for the evidence record
            run, j = steps_ran[0]
            seen.append(f"{row.cell}\t{run['id']}\t{j.get('id')}\t{j.get('name')}\t{j.get('conclusion')}")
    return problems, checked


def check(root, today, do_remote=False, repo=REPO, fetch=gh, out=sys.stdout, ran_out=None):
    root = Path(root).resolve()
    rows, problems = read_registry(root / REGISTRY)
    targets = transports = 0
    owed = []
    if rows:
        found, targets, transports = inventory(root, rows)
        problems += found
        found, owed = wiring(root, rows, today)
        problems += found
    checked = 0
    if do_remote and rows:
        try:
            seen = [] if ran_out else None
            found, checked = remote(root, rows, repo, fetch, seen)
            if ran_out:
                Path(ran_out).write_text("".join(line + "\n" for line in seen))
        except (RuntimeError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
            found, checked = [f"API-ERROR {error}"], 0  # an unreadable history is red, never a pass
        problems += found
    for row in owed:
        print(f"OWED {row.tier[5:]} {row.cell}", file=out)
    for problem in problems:
        print(f"RUN-MATRIX {problem}", file=out)
    print(f"check_run_matrix: {len(rows)} cells, {len(owed)} owed, {targets} targets, {transports} transports"
          + (f", {checked} cells seen run on main" if do_remote else "") + f", {len(problems)} problems", file=out)
    return 1 if problems else 0


def self_test():
    failures = []
    cases = [0]
    today = datetime.date(2026, 11, 12)
    ci = ("on:\n  push:\njobs:\n  build:\n    runs-on: [self-hosted]\n    steps:\n"
          "      - name: Unit suite\n        run: ./bin/test\n"
          "      - name: Functional suite\n        run: ./bin/functional\n"
          "      - name: Functional, gso pinned\n        env:\n          ROCKETNET_SERVER_TRANSPORT: gso\n"
          "        run: ./bin/functional\n")
    files = {
        ".github/workflows/ci.yml": ci,
        ".github/workflows/fuzz-nightly.yml": "on:\n  schedule:\njobs: {}\n# rocketnet_fuzz\n",
        ".github/workflows/nightly-dst.yml": "# --schedule burst\n",
        "tests/fuzz/rocketnet_fuzz.cpp": "",
        "tests/dst.cpp": "\"           [--schedule burst]\\n\"\n",
        "CMakeLists.txt": "option(ROCKETNET_DEV \"x\" ON)\nadd_executable(test t.cpp)\nadd_executable(functional f.cpp)\n",
        "sdk/rocketnet.h": "",
        "sdk/rocketnet_drivers.inc": "ROCKETNET_TRANSPORT( GSO, \"gso\", true, ops, ROCKETNET_TRANSPORT_LINUX, 20, 0 )\n",
        "tools/checkers.txt": "",
    }
    head = "# header\n"

    def row(cell, wf="ci.yml", job="build", step="Unit suite", needle="test", tier="per-commit"):
        return "\t".join([cell, wf, job, step, needle, tier, "a reason for the cell"]) + "\n"

    good = head + "".join([
        row("unit|linux|x86_64|gcc|Release|devON|none|-~-|default"),
        row("unit|linux|x86_64|gcc|Release|devOFF|none|-~-|default"),
        row("functional|linux|x86_64|gcc|Release|devON|none|gso~portable|default", step="Functional, gso pinned", needle="functional && gso"),
        row("functional|linux|x86_64|gcc|Debug|devON|none|gso~portable|default", wf="-", job="-", step="-", needle="functional && gso", tier="owed:2026-11-19"),
    ])
    run = {"id": 7}
    jobs_ok = {"jobs": [{"id": 70, "name": "build", "conclusion": "success",
                         "steps": [{"name": "Unit suite", "conclusion": "success"},
                                   {"name": "Functional, gso pinned", "conclusion": "success"}]}]}

    def fetcher(jobs, log="*** ALL TESTS PASSED ***"):
        def fetch(path):
            if path.endswith("per_page=1"):
                return json.dumps({"workflow_runs": [run]})
            if "/jobs?" in path:
                return json.dumps(jobs)
            return log
        return fetch

    def case(label, extra_files, registry, want, do_remote=False, fetch=None, expect=None):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, text in {**files, **extra_files}.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text)
            (root / REGISTRY).write_text(registry)
            sink = io.StringIO()
            got = check(root, today, do_remote=do_remote, fetch=fetch or fetcher(jobs_ok), out=sink)
            problems = [line for line in sink.getvalue().splitlines() if line.startswith("RUN-MATRIX ")]
            # a red case must be red for its own plant only: every problem line names it
            ok = got == want and (want == 0 or (problems and all(expect in p for p in problems)))
            cases[0] += 1
            print(f"{'ok' if ok else 'FAIL'}: {label}: exit {got}")
            if not ok:
                failures.append(label)
                check(root, today, do_remote=do_remote, fetch=fetch or fetcher(jobs_ok))

    case("a complete matrix, every cell wired", {}, good, 0)
    case("a complete matrix, seen run on main", {}, good, 0, do_remote=True)
    # (a) inventory plants
    case("(a) a test executable with no cell", {"CMakeLists.txt": files["CMakeLists.txt"] + "add_executable(test_new n.cpp)\n"},
         good, 1, expect="target test_new has no cell")
    case("(a) a transport with no Release cell", {"sdk/rocketnet_drivers.inc": files["sdk/rocketnet_drivers.inc"]
                                                  + "ROCKETNET_TRANSPORT( URING, \"io_uring\", true, ops, ROCKETNET_TRANSPORT_LINUX, 25, 0 )\n"},
         good + row("functional|linux|x86_64|gcc|Debug|devON|none|io_uring~portable|default", wf="-", job="-", step="-",
                    needle="functional && io_uring", tier="owed:2026-11-19"),
         1, expect="transport io_uring has no Release functional cell on linux")
    case("(a) a CMake option with no cell", {"CMakeLists.txt": files["CMakeLists.txt"] + "option(ROCKETNET_NEW \"x\" ON)\n"},
         good, 1, expect="CMake option ROCKETNET_NEW")
    case("(a) a documented bound with no cell", {"sdk/rocketnet.h": "#error ROCKETNET_MAX_CLIENTS must be between 512 and 512\n"},
         good, 1, expect="documented bound MAX_CLIENTS=512")
    case("(a) a fuzz harness no nightly runs", {"tests/fuzz/rocketnet_fuzz_new.cpp": ""}, good, 1, expect="rocketnet_fuzz_new")
    case("(a) a DST schedule no nightly runs", {"tests/dst.cpp": "\"           [--schedule burst|storm]\\n\"\n"}, good, 1,
         expect="DST schedule storm")
    case("(a) a checker neither a cell nor registered", {"tools/check_new.py": ""}, good, 1, expect="tools/check_new.py")
    # (b) wiring plants
    case("(b) a step that does not exist", {}, good.replace("\tUnit suite\t", "\tUnit suites!\t", 1), 1, expect="no step named")
    case("(b) a step that does not run the pin", {}, good.replace("functional && gso\tper-commit", "functional && io_uring\tper-commit"),
         1, expect="runs 'functional && io_uring'")
    case("(b) a job that does not exist", {}, good.replace("\tbuild\tUnit suite", "\tbuilds\tUnit suite", 1), 1, expect="has no job builds")
    case("(b) an owed cell past its date", {}, good.replace("owed:2026-11-19", "owed:2026-11-01"), 1, expect="owed since 2026-11-01")
    case("(b) a duplicate cell", {}, good + good.splitlines()[1] + "\n", 1, expect="declared twice")
    case("(b) zero rows", {}, head, 1, expect="0 rows")
    # (c) remote plants
    skipped = json.loads(json.dumps(jobs_ok))
    skipped["jobs"][0]["conclusion"] = "skipped"
    remote_good = good
    case("(c) the job was skipped on main", {}, remote_good, 1, do_remote=True, fetch=fetcher(skipped), expect="was skipped")
    cancelled = json.loads(json.dumps(jobs_ok))
    cancelled["jobs"][0]["conclusion"] = "cancelled"
    case("(c) the job was cancelled on main", {}, remote_good, 1, do_remote=True, fetch=fetcher(cancelled), expect="was cancelled")
    step_skipped = json.loads(json.dumps(jobs_ok))
    step_skipped["jobs"][0]["steps"][0]["conclusion"] = "skipped"
    step_skipped["jobs"][0]["steps"][1]["conclusion"] = "skipped"
    case("(c) the step was skipped on main", {}, remote_good, 1, do_remote=True, fetch=fetcher(step_skipped), expect="did not run")
    case("(c) the suite executed zero cases", {}, remote_good, 1, do_remote=True,
         fetch=fetcher(jobs_ok, log="check passed: 0 cases\n"), expect="shows no executed case")
    case("(c) no run on main has the job", {}, remote_good, 1, do_remote=True, fetch=fetcher({"jobs": []}), expect="has job build")
    if failures:
        print(f"check_run_matrix self-test: {len(failures)} failed: {failures}")
        return 1
    print(f"check_run_matrix self-test: {cases[0]} cases passed")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(ROOT))
    parser.add_argument("--today", default=None, help="YYYY-MM-DD (default: today, UTC)")
    parser.add_argument("--remote", action="store_true")
    parser.add_argument("--repo", default=REPO)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--ran-out", default=None, help="with --remote: write cell, run id, job id, job name, conclusion per cell seen run")
    args = parser.parse_args(argv[1:])
    if args.self_test:
        return self_test()
    today = datetime.date.fromisoformat(args.today) if args.today else datetime.datetime.now(datetime.timezone.utc).date()
    return check(args.root, today, do_remote=args.remote, repo=args.repo, ran_out=args.ran_out)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
