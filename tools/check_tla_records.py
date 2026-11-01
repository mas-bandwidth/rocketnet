#!/usr/bin/env python3
"""Check committed TLA+ run records in proofs/tla/RUNS.tsv against proofs/tla/CASES.tsv.

CASES.tsv columns:
  config, module, expected, property, deadlock, group, gate, debt

RUNS.tsv columns:
  config, module, input_sha256, input_files, jar_sha256, java_version, host,
  cpus, started_utc, workers, generated, distinct, seconds, exit, result,
  expected, property, budget, mode

Rules enforced:
  - Exact bijection of (config, module) between CASES.tsv and RUNS.tsv with no
    duplicate, missing, or extra configs.
  - Each case declares expected in {pass, invariant, action, temporal}; pass
    requires property == '-', while invariant/action/temporal require a named
    property != '-'.
  - Each RUNS.tsv row matches its CASES.tsv row on module, expected, and
    property, has mode == 'bounded', and uses the pinned tla2tools.jar digest.
  - A row with result == 'PASS' must have the TLC exit code for its declared
    expectation (pass -> 0, invariant -> 12, action -> 13, temporal -> 13) and
    positive integer generated and distinct state counts.
  - A row with result != 'PASS' is allowed only when result == 'FAIL',
    case['gate'] == 'bench', case['debt'] != '-', and row['exit'] == '124'
    (honest declared timeout debt).
"""

import argparse
import csv
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CASES = ROOT / "proofs" / "tla" / "CASES.tsv"
DEFAULT_RUNS = ROOT / "proofs" / "tla" / "RUNS.tsv"

PINNED_JAR_SHA256 = "936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88"

CASES_COLUMNS = (
    "config",
    "module",
    "expected",
    "property",
    "deadlock",
    "group",
    "gate",
    "debt",
)

RUNS_COLUMNS = (
    "config",
    "module",
    "input_sha256",
    "input_files",
    "jar_sha256",
    "java_version",
    "host",
    "cpus",
    "started_utc",
    "workers",
    "generated",
    "distinct",
    "seconds",
    "exit",
    "result",
    "expected",
    "property",
    "budget",
    "mode",
)

EXPECTED_PASS_EXIT = {
    "pass": "0",
    "invariant": "12",
    "action": "13",
    "temporal": "13",
}


def positive_int_field(value):
    return isinstance(value, str) and value.isdigit() and int(value) > 0


def check(cases_path=DEFAULT_CASES, runs_path=DEFAULT_RUNS, out=sys.stdout):
    errors = []

    with Path(cases_path).open(encoding="utf-8", newline="") as handle:
        cases_reader = csv.DictReader(handle, delimiter="\t")
        if tuple(cases_reader.fieldnames or ()) != CASES_COLUMNS:
            errors.append(
                f"CASES.tsv columns {tuple(cases_reader.fieldnames or ())} != {CASES_COLUMNS}"
            )
            for err in errors:
                print(err, file=out)
            return 1
        cases = list(cases_reader)

    with Path(runs_path).open(encoding="utf-8", newline="") as handle:
        runs_reader = csv.DictReader(handle, delimiter="\t")
        if tuple(runs_reader.fieldnames or ()) != RUNS_COLUMNS:
            errors.append(
                f"RUNS.tsv columns {tuple(runs_reader.fieldnames or ())} != {RUNS_COLUMNS}"
            )
            for err in errors:
                print(err, file=out)
            return 1
        runs = list(runs_reader)

    plan = {}
    for case in cases:
        cfg = case["config"]
        if cfg in plan:
            errors.append(f"{cfg}: duplicate config in CASES.tsv")
            continue
        plan[cfg] = case
        expected = case["expected"]
        prop = case["property"]
        if expected not in EXPECTED_PASS_EXIT:
            errors.append(f"{cfg}: unknown expected outcome {expected!r} in CASES.tsv")
        elif expected == "pass":
            if prop != "-":
                errors.append(f"{cfg}: pass case must have property '-', got {prop!r}")
        else:
            if not prop or prop == "-":
                errors.append(f"{cfg}: {expected} case must name a property, got {prop!r}")

    seen_runs = set()
    for row in runs:
        cfg = row["config"]
        if cfg in seen_runs:
            errors.append(f"{cfg}: duplicate config in RUNS.tsv")
            continue
        seen_runs.add(cfg)
        case = plan.get(cfg)
        if case is None:
            errors.append(f"{cfg}: present in RUNS.tsv but missing from CASES.tsv")
            continue

        if row["module"] != case["module"]:
            errors.append(
                f"{cfg}: module mismatch (RUNS.tsv {row['module']!r} != CASES.tsv {case['module']!r})"
            )
        if row["expected"] != case["expected"]:
            errors.append(
                f"{cfg}: expected mismatch (RUNS.tsv {row['expected']!r} != CASES.tsv {case['expected']!r})"
            )
        if row["property"] != case["property"]:
            errors.append(
                f"{cfg}: property mismatch (RUNS.tsv {row['property']!r} != CASES.tsv {case['property']!r})"
            )
        if row["mode"] != "bounded":
            errors.append(f"{cfg}: not a bounded measurement (mode={row['mode']!r})")
        if row["jar_sha256"] != PINNED_JAR_SHA256:
            errors.append(f"{cfg}: different JAR ({row['jar_sha256']!r})")

        expected = case["expected"]
        want_exit = EXPECTED_PASS_EXIT.get(expected)
        if row["result"] == "PASS":
            if want_exit is not None and row["exit"] != want_exit:
                errors.append(
                    f"{cfg}: PASS row for expected {expected!r} requires exit {want_exit!r}, got {row['exit']!r}"
                )
            if not positive_int_field(row["generated"]) or not positive_int_field(row["distinct"]):
                errors.append(
                    f"{cfg}: PASS row requires positive generated/distinct state counts, got generated={row['generated']!r} distinct={row['distinct']!r}"
                )
        else:
            if not (
                row["result"] == "FAIL"
                and case["gate"] == "bench"
                and case["debt"] != "-"
                and bool(case["debt"].strip())
                and row["exit"] == "124"
            ):
                errors.append(
                    f"{cfg}: failure without declared timeout debt (result={row['result']!r}, exit={row['exit']!r}, gate={case['gate']!r}, debt={case['debt']!r})"
                )

    for cfg in plan:
        if cfg not in seen_runs:
            errors.append(f"{cfg}: present in CASES.tsv but missing from RUNS.tsv")

    if errors:
        for err in errors:
            print(err, file=out)
        return 1

    print(
        f"Required outcomes match ({len(runs)} rows); any unfinished cases are explicit timeout debt.",
        file=out,
    )
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cases", type=Path, default=DEFAULT_CASES)
    parser.add_argument("--runs", type=Path, default=DEFAULT_RUNS)
    args = parser.parse_args(argv)
    return check(args.cases, args.runs)


if __name__ == "__main__":
    sys.exit(main())
