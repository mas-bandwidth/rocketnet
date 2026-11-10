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
  - Each row's input_sha256 and input_files are the fingerprint its case has
    now (the pinned tlacheck's): a record of other inputs is stale and refused.
"""

import argparse
import csv
import hashlib
import re
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


# The fingerprint of pinned tlacheck 0c97d595 (nova-tools pkg/tlc/inputs.go), so
# a record measured on other inputs is refused per commit, as `tlacheck groups
# --stale` would. Its runner files are inputs of every case: their digests at
# the pin in .github/workflows/tlc.yml, which cross-checks this port.
RUNNER_SHA256 = {
    "pkg/tlc/outcome.go": "beeb2482310944921dc38c1e481c83e75aced119be86418a2992d5291c30036f",
    "pkg/tlc/plan.go": "d2a932979746c8d5c856b7b04f9f57ccf484ca7fd14ae0be1e8f02516701626b",
    "pkg/tlc/run.go": "bb10dac644a7bf231a1f62fc982aa8598dff18c29954399fdeb26bd62f8c17c1",
    "pkg/tlc/suite.go": "442b1ac74eb798a7be04a9b9a6f409e72886e4b749bfebd34c78cfae4ebfe433",
}
STANDARD_MODULES = {
    "Bags", "FiniteSets", "Integers", "Naturals", "Randomization",
    "RealTime", "Reals", "Sequences", "TLC", "Toolbox",
}
EXTENDS_RE = re.compile(r"\bEXTENDS\b\s*([A-Za-z0-9_]+(?:\s*,\s*[A-Za-z0-9_]+)*)", re.ASCII)
INSTANCE_RE = re.compile(r"\bINSTANCE\s+([A-Za-z0-9_]+)", re.ASCII)
MODULE_OPEN_RE = re.compile(r"^\s*-{4,}\s*MODULE\s+([A-Za-z0-9_]+)", re.ASCII)
MODULE_END_RE = re.compile(r"^\s*={4,}", re.ASCII)


def strip_comments(text):
    """Blank comments and string contents, keeping length and line breaks."""
    out = list(text)

    def blank(start, end):
        for k in range(start, end):
            if out[k] != "\n":
                out[k] = " "

    i, n = 0, len(text)
    while i < n:
        if text[i] == '"':
            j = i + 1
            while j < n and text[j] != '"' and text[j] != "\n":
                if text[j] == "\\" and j + 1 < n:
                    j += 1
                j += 1
            if j < n and text[j] == '"':
                j += 1
            blank(i, j)
            i = j
        elif text.startswith("(*", i):
            depth, j = 1, i + 2
            while j < n and depth > 0:
                if text.startswith("(*", j):
                    depth, j = depth + 1, j + 2
                elif text.startswith("*)", j):
                    depth, j = depth - 1, j + 2
                else:
                    j += 1
            blank(i, j)
            i = j
        elif text.startswith("\\*", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            blank(i, j)
            i = j
        else:
            i += 1
    return "".join(out)


def module_references(text):
    """The modules a module's text EXTENDS or INSTANCEs, as tlacheck reads them."""
    lines = strip_comments(text).split("\n")
    first = next((i for i, line in enumerate(lines) if MODULE_OPEN_RE.match(line)), 0)
    declared, body, depth = set(), [], 0
    for line in lines[first:]:
        opened, ended = MODULE_OPEN_RE.match(line), MODULE_END_RE.match(line)
        if opened:
            depth += 1
            declared.add(opened.group(1))
            body.append(line[opened.end():])
        elif ended:
            depth -= 1
            if depth <= 0:
                break
            body.append(line[ended.end():])
        else:
            body.append(line)
    clean = "\n".join(body)
    hits = [(m.start(), [s.strip() for s in m.group(1).split(",")]) for m in EXTENDS_RE.finditer(clean)]
    hits += [(m.start(), [m.group(1)]) for m in INSTANCE_RE.finditer(clean)]
    hits.sort(key=lambda hit: hit[0])
    names = []
    for _, group in hits:
        names += [name for name in group if name not in names and name not in declared]
    return names


def fingerprint(tla_dir, config):
    """(input_sha256, input_files) of a case as tlacheck computes it from tla_dir."""
    tla_dir = Path(tla_dir)
    with (tla_dir / "CASES.tsv").open(encoding="utf-8", newline="") as handle:
        rows = [row for row in csv.reader(handle, delimiter="\t") if row]
    found = [row for row in rows[1:] if row[0] == config]
    if len(found) != 1:
        raise ValueError(f"CASES.tsv declares {config} {len(found)} times")
    module = found[0][rows[0].index("module")]
    files = {"tla/" + config: (tla_dir / config).read_bytes()}
    queue, seen = [module], set()
    while queue:
        name = queue.pop(0)
        if name in seen:
            continue
        seen.add(name)
        raw = (tla_dir / name).read_bytes()
        files["tla/" + name] = raw
        for ref in module_references(raw.decode("latin-1")):
            if (tla_dir / (ref + ".tla")).exists():
                queue.append(ref + ".tla")
            elif ref not in STANDARD_MODULES:
                raise ValueError(f"module {name} names {ref}, neither a file nor a standard module")
    files["tla/CASES.tsv#" + config] = ("\t".join(rows[0]) + "\n" + "\t".join(found[0]) + "\n").encode("utf-8")
    digests = {path: hashlib.sha256(raw).hexdigest() for path, raw in files.items()}
    digests.update(RUNNER_SHA256)
    h = hashlib.sha256()
    for path in sorted(digests):
        h.update(path.encode("utf-8") + b"\0" + digests[path].encode("ascii") + b"\n")
    return h.hexdigest(), len(digests)


def check(cases_path=DEFAULT_CASES, runs_path=DEFAULT_RUNS, out=sys.stdout, forbid_stale=True):
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
        if forbid_stale:
            try:
                digest, files = fingerprint(Path(cases_path).parent, cfg)
            except (OSError, ValueError) as err:
                errors.append(f"{cfg}: cannot fingerprint its inputs: {err}")
            else:
                if (row["input_sha256"], row["input_files"]) != (digest, str(files)):
                    errors.append(
                        f"{cfg}: stale record, measured on other inputs (input_sha256 {row['input_sha256']} over {row['input_files']} files, now {digest} over {files}); run its group again with tlacheck and commit the record"
                    )

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
