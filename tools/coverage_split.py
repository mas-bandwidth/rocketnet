#!/usr/bin/env python3
"""Split sdk/ line coverage into what production paths reach and what only
the unit suite reaches.

Builds an instrumented Release tree (build-cov) with clang source-based
coverage and the repository's own warning flags, runs the unit suite
(./bin/test) into one profile directory and the production-path runs (the
functional suite and the examples, through tools/run_examples.py) into
another, merges each with llvm-profdata, exports line coverage of sdk/ for
each with `llvm-cov export -format=text` against the binaries that ran, and
writes, per sdk/ source file and per line, three sets:

  production  reached by the functional suite or an example
  unit_only   reached by ./bin/test and by nothing else
  never       reached by neither

Two kinds of line are reported apart and never mixed into unit_only:
sdk/rocketnet_tests.cpp (the unit suite's own cases, linked only into
./bin/test) and test-seam code, the lines inside a preprocessor branch that
is compiled only when ROCKETNET_DEV is on. Those lines get their own
reached/never counts. The functional suite and the examples, which link
rocketnet_dev, can reach seam lines.

The examples run through tools/run_examples.py's own drivers, one at a
time, and a failing example does not stop the ones after it, so every
example is measured; the backend pair (tools/run_backend_examples.py) is
not run.

The build is Release, so the NDEBUG paths are the ones measured. Its
binaries go to build-cov/bin (CMAKE_RUNTIME_OUTPUT_DIRECTORY), never the
repository bin/, so a normal build's binaries are left alone.

Output (in --out): coverage.json (per file: the line numbers in each set;
the run results and wall times), summary.txt (the plain table: file, lines
in each set), the merged profiles, both exports and every run's log.

Exits non-zero when a run it drives fails; a failing functional case is
named, and the profiles and reports are still written. Linux and macOS;
llvm-cov and llvm-profdata are found unversioned or as -21 / -18
(LLVM_COV / LLVM_PROFDATA override). Analysis of the sets is not this tool.

usage: python3 tools/coverage_split.py --out DIR [--skip-build] [--jobs N]
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-cov"
BIN = BUILD / "bin"
COVERAGE_FLAGS = "-fprofile-instr-generate -fcoverage-mapping"
TESTS_SOURCE = "sdk/rocketnet_tests.cpp"
# binaries the functional suite runs besides itself (tests/functional/main.cpp execs)
FUNCTIONAL_EXECS = ["test_serialize", "test_transport_control", "test_xdp", "test_msgx"]
SEAM_MACRO = re.compile(r"\bROCKETNET_DEV\b")
NEGATED_SEAM = re.compile(r"!\s*(defined\s*\(?\s*)?ROCKETNET_DEV\b")
DIRECTIVE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$")
SETS = ("production", "unit_only", "never")


def find_tool(name, override):
    if os.environ.get(override):
        return os.environ[override]
    for candidate in (name, name + "-21", name + "-18"):
        if shutil.which(candidate):
            return candidate
    if shutil.which("xcrun"):
        found = subprocess.run(["xcrun", "--find", name], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        if found.returncode == 0 and found.stdout.strip():
            return found.stdout.strip()
    sys.exit(f"coverage_split: {name} not found (unversioned, -21 or -18); set {override}")


def find_compiler(name):
    for candidate in (name, name + "-21", name + "-18"):
        if shutil.which(candidate):
            return candidate
    sys.exit(f"coverage_split: {name} not found")


def run_logged(label, command, log, env=None, cwd=ROOT):
    """Runs one step with its output in log; returns (exit code, seconds)."""
    print(f"coverage_split: {label}", flush=True)
    start = time.monotonic()
    with open(log, "w") as out:
        result = subprocess.run(command, stdout=out, stderr=subprocess.STDOUT, env=env, cwd=cwd)
    seconds = time.monotonic() - start
    print(f"coverage_split: {label} exit {result.returncode} in {seconds:.1f}s", flush=True)
    return result.returncode, seconds


def build(jobs, out):
    cc = find_compiler("clang")
    cxx = find_compiler("clang++")
    configure = [
        "cmake", "-S", str(ROOT), "-B", str(BUILD),
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}",
        f"-DCMAKE_C_FLAGS={COVERAGE_FLAGS}", f"-DCMAKE_CXX_FLAGS={COVERAGE_FLAGS}",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={BIN}",
        "-DROCKETNET_BUILD_TESTS=ON", "-DROCKETNET_BUILD_EXAMPLES=ON", "-DROCKETNET_BUILD_TOOLS=ON",
    ]
    if shutil.which("ninja"):
        configure[1:1] = ["-G", "Ninja"]
    rc, configure_s = run_logged("configure build-cov", configure, out / "configure.log")
    if rc != 0:
        sys.exit(f"coverage_split: configure failed, see {out / 'configure.log'}")
    rc, build_s = run_logged("build build-cov", ["cmake", "--build", str(BUILD), "--parallel", str(jobs)], out / "build.log")
    if rc != 0:
        sys.exit(f"coverage_split: build failed, see {out / 'build.log'}")
    return configure_s + build_s


def failed_cases(log):
    names = []
    for line in Path(log).read_text(errors="replace").splitlines():
        if "check failed" in line or line.startswith("FAIL"):
            names.append(line.strip())
    return names[:40]


def run_examples(env, log):
    """Runs every example through tools/run_examples.py's own drivers
    (run_pair, run_single and its grouping), one group at a time, and keeps
    going past a failing example so the ones after it are still measured;
    tools/run_examples.py's main() stops at the first failure. Returns
    (exit code, seconds, failed example names)."""
    print("coverage_split: examples (tools/run_examples.py)", flush=True)
    start = time.monotonic()
    sys.path.insert(0, str(ROOT / "tools"))
    import run_examples as examples  # pylint: disable=import-outside-toplevel
    sys.path.pop(0)
    os.environ["LLVM_PROFILE_FILE"] = env["LLVM_PROFILE_FILE"]
    names = {path.stem for path in examples.EXAMPLES.glob("*.c")}
    pair_bases = {n[: -len("_server")] for n in names if n.endswith("_server") and n[: -len("_server")] + "_client" in names}
    used = {base + suffix for base in pair_bases for suffix in ("_server", "_client")}
    pair_bases -= examples.RUN_ELSEWHERE
    groups = [(base, [base + "_server", base + "_client"]) for base in sorted(pair_bases)]
    groups += [(name, [name]) for name in sorted(names) if name not in used]
    dirs = examples.binary_dirs(BUILD, None)
    failed = []
    with open(log, "w") as out:
        saved = (sys.stdout, sys.stderr)
        sys.stdout = sys.stderr = out
        try:
            for base, members in groups:
                binaries = [examples.find_binary(member, dirs) for member in members]
                if None in binaries:
                    ok = examples.failure(base, "binary not found under build-cov")
                elif len(binaries) == 2:
                    ok = examples.run_pair(base, binaries[0], binaries[1])
                else:
                    ok = examples.run_single(base, binaries[0])
                print(f"{'PASS' if ok is True else 'FAIL'} example_{base}", flush=True)
                if ok is not True:
                    failed.append(f"example_{base}")
        finally:
            sys.stdout, sys.stderr = saved
            del os.environ["LLVM_PROFILE_FILE"]
    seconds = time.monotonic() - start
    rc = 1 if failed else 0
    print(f"coverage_split: examples exit {rc} in {seconds:.1f}s", flush=True)
    return rc, seconds, failed


def merge(profdata, raw_dir, target):
    raws = sorted(str(p) for p in raw_dir.glob("*.profraw"))
    if not raws:
        return False
    listing = target.with_suffix(".list")
    listing.write_text("\n".join(raws) + "\n")
    subprocess.run([profdata, "merge", "-f", str(listing), "-o", str(target)], check=True)
    return True


def export(cov, objects, profile, target):
    present = [str(o) for o in objects if Path(o).is_file()]
    command = [cov, "export", "-format=text", "-skip-functions", f"-instr-profile={profile}", present[0]]
    for extra in present[1:]:
        command += ["-object", extra]
    command.append(str(ROOT / "sdk"))
    with open(target, "w") as out:
        subprocess.run(command, stdout=out, stderr=subprocess.DEVNULL, check=True)
    return present


def line_counts(segments):
    """Per-line execution counts from llvm-cov's segments, as llvm-cov's own
    LineCoverageStats computes them: {line: count} for every mapped line."""
    counts = {}
    if not segments:
        return counts
    by_line = {}
    for segment in segments:
        by_line.setdefault(segment[0], []).append(segment)
    last_line = max(by_line)
    wrapped = None
    for line in range(segments[0][0], last_line + 1):
        starts = by_line.get(line, [])
        region_starts = [s for s in starts if s[3] and s[4] and not s[5]]
        skipped = bool(starts) and not starts[0][3] and starts[0][4]
        mapped = not skipped and ((wrapped is not None and wrapped[3]) or bool(region_starts))
        mapped = mapped or any(s[4] and s[3] for s in starts)
        if mapped:
            count = wrapped[2] if wrapped is not None else 0
            for s in region_starts:
                count = max(count, s[2])
            counts[line] = count
        if starts:
            wrapped = starts[-1]
    return counts


def file_counts(export_path):
    data = json.loads(Path(export_path).read_text())
    result = {}
    for unit in data.get("data", []):
        for entry in unit.get("files", []):
            name = os.path.relpath(entry["filename"], ROOT)
            if not name.startswith("sdk/"):
                continue
            counts = line_counts(entry.get("segments", []))
            merged = result.setdefault(name, {})
            for line, count in counts.items():
                merged[line] = merged.get(line, 0) + count
    return result


def seam_lines(path):
    """Line numbers inside a preprocessor branch compiled only when a test
    seam macro is on."""
    stack = []
    lines = set()
    try:
        text = path.read_text(errors="replace").splitlines()
    except OSError:
        return lines
    for number, line in enumerate(text, 1):
        match = DIRECTIVE.match(line)
        if match:
            kind, rest = match.group(1), match.group(2)
            if kind == "ifndef":
                rest = "!" + rest.strip()
            positive = (bool(SEAM_MACRO.search(rest)) and not NEGATED_SEAM.search(rest) and "||" not in rest)
            if kind in ("if", "ifdef", "ifndef"):
                stack.append(positive)
            elif kind == "elif" and stack:
                stack[-1] = positive
            elif kind == "else" and stack:
                stack[-1] = False
            elif kind == "endif" and stack:
                stack.pop()
            continue
        if any(stack):
            lines.add(number)
    return lines


def classify(unit, production):
    files = {}
    for name in sorted(set(unit) | set(production)):
        unit_lines = unit.get(name, {})
        production_lines = production.get(name, {})
        seams = seam_lines(ROOT / name)
        entry = {set_name: [] for set_name in SETS}
        entry.update({"seam_production": [], "seam_unit_only": [], "seam_never": [], "not_in_production_objects": []})
        for line in sorted(set(unit_lines) | set(production_lines)):
            if production_lines.get(line, 0) > 0:
                kind = "production"
            elif unit_lines.get(line, 0) > 0:
                kind = "unit_only"
            else:
                kind = "never"
            if line in seams:
                entry["seam_" + kind].append(line)
            else:
                entry[kind].append(line)
            if line not in production_lines:
                entry["not_in_production_objects"].append(line)
        files[name] = entry
    return files


def table(files):
    header = f"{'file':<52} {'production':>10} {'unit_only':>9} {'never':>7} {'seam p/u/n':>14} {'not_in_prod':>11}"
    rows = [header, "-" * len(header)]
    totals = {k: 0 for k in ("production", "unit_only", "never", "seam_production", "seam_unit_only", "seam_never", "not_in_production_objects")}
    tests_rows = []
    for name, entry in files.items():
        n = {k: len(v) for k, v in entry.items()}
        seam = f"{n['seam_production']}/{n['seam_unit_only']}/{n['seam_never']}"
        row = f"{name:<52} {n['production']:>10} {n['unit_only']:>9} {n['never']:>7} {seam:>14} {n['not_in_production_objects']:>11}"
        if name == TESTS_SOURCE:
            tests_rows.append(row)
            continue
        rows.append(row)
        for k in totals:
            totals[k] += n[k]
    rows.append("-" * len(header))
    seam = f"{totals['seam_production']}/{totals['seam_unit_only']}/{totals['seam_never']}"
    rows.append(f"{'TOTAL (sdk/ less ' + TESTS_SOURCE + ')':<52} {totals['production']:>10} {totals['unit_only']:>9} {totals['never']:>7} {seam:>14} {totals['not_in_production_objects']:>11}")
    if tests_rows:
        rows += ["", "reported apart (the unit suite's own cases):", header] + tests_rows
    return "\n".join(rows) + "\n", totals


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True, help="output directory (created; profiles, exports, logs, coverage.json, summary.txt)")
    parser.add_argument("--skip-build", action="store_true", help="reuse build-cov as it is")
    parser.add_argument("--jobs", type=int, default=4, help="build parallelism (default 4)")
    args = parser.parse_args()

    started = time.monotonic()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    profdata = find_tool("llvm-profdata", "LLVM_PROFDATA")
    cov = find_tool("llvm-cov", "LLVM_COV")
    seconds = {}

    if args.skip_build:
        if not (BIN / "test").is_file():
            sys.exit("coverage_split: --skip-build but build-cov/bin/test is missing")
    else:
        seconds["build"] = build(args.jobs, out)

    runs = {}
    raw = {"unit": out / "raw-unit", "production": out / "raw-production"}
    for directory in raw.values():
        shutil.rmtree(directory, ignore_errors=True)
        directory.mkdir()

    env = dict(os.environ)
    env["LLVM_PROFILE_FILE"] = str(raw["unit"] / "unit-%p.profraw")
    rc, seconds["unit"] = run_logged("unit suite (./bin/test)", [str(BIN / "test")], out / "unit.log", env)
    runs["unit"] = {"exit": rc, "failed": failed_cases(out / "unit.log") if rc else []}

    env["LLVM_PROFILE_FILE"] = str(raw["production"] / "functional-%p.profraw")
    rc, seconds["functional"] = run_logged("functional suite (./bin/functional)", [str(BIN / "functional")], out / "functional.log", env)
    runs["functional"] = {"exit": rc, "failed": failed_cases(out / "functional.log") if rc else []}

    env["LLVM_PROFILE_FILE"] = str(raw["production"] / "examples-%p.profraw")
    rc, seconds["examples"], failed = run_examples(env, out / "examples.log")
    runs["examples"] = {"exit": rc, "failed": failed}

    start = time.monotonic()
    profiles = {}
    for kind, directory in raw.items():
        target = out / f"{kind}.profdata"
        if merge(profdata, directory, target):
            profiles[kind] = target
    unit_objects = [BIN / "test"]
    production_objects = [BIN / "functional"] + [BIN / name for name in FUNCTIONAL_EXECS]
    production_objects += sorted(BIN.glob("example_*"))
    objects = {"unit": unit_objects, "production": production_objects}
    counts = {}
    used = {}
    for kind in ("unit", "production"):
        if kind not in profiles:
            print(f"coverage_split: no {kind} profiles were written", file=sys.stderr)
            counts[kind] = {}
            continue
        export_path = out / f"{kind}.export.json"
        used[kind] = export(cov, objects[kind], profiles[kind], export_path)
        counts[kind] = file_counts(export_path)
    files = classify(counts["unit"], counts["production"])
    text, totals = table(files)
    seconds["report"] = time.monotonic() - start
    seconds["total"] = time.monotonic() - started

    lines = [text, "wall time: " + ", ".join(f"{k} {v:.1f}s" for k, v in seconds.items())]
    for kind, run in runs.items():
        lines.append(f"run {kind}: exit {run['exit']}" + ("" if not run["failed"] else ": " + "; ".join(run["failed"])))
    summary = "\n".join(lines) + "\n"
    (out / "summary.txt").write_text(summary)
    report = {
        "root": str(ROOT),
        "head": subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, stdout=subprocess.PIPE, text=True).stdout.strip(),
        "sets": {
            "production": "reached by the functional suite or an example",
            "unit_only": "reached by ./bin/test and nothing else",
            "never": "reached by neither",
            "seam_*": "the same three, for lines compiled only under ROCKETNET_DEV",
            "not_in_production_objects": "lines with no mapping in the production-run binaries (any set)",
        },
        "apart": [TESTS_SOURCE],
        "objects": used,
        "runs": runs,
        "seconds": seconds,
        "totals": totals,
        "files": files,
    }
    (out / "coverage.json").write_text(json.dumps(report, indent=1) + "\n")
    print(summary, end="")
    print(f"coverage_split: wrote {out / 'coverage.json'} and {out / 'summary.txt'}")
    return 0 if all(run["exit"] == 0 for run in runs.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
