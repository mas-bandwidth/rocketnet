#!/usr/bin/env python3
"""Check every tla/COVERAGE.tsv row against its model and code."""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COVERAGE = ROOT / "tla" / "COVERAGE.tsv"
TLA_DIR = ROOT / "tla"

FUNC_RE = re.compile(r"(?:rn_|rocketnet_)[A-Za-z0-9_]+(?=,|\)|and\b)")


def iter_rows():
    lines = COVERAGE.read_text().splitlines()
    if not lines:
        return
    header = lines[0].split("\t")
    for line in lines[1:]:
        if not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) < len(header):
            continue
        yield dict(zip(header, parts))


def tla_modules():
    modules = []
    for path in sorted(TLA_DIR.glob("*.tla")):
        name = path.stem
        if name.startswith("MC") or name == "Link":
            continue
        modules.append(name)
    return modules


def check_row(row):
    failures = []
    module = row["module"]
    status = row.get("status", "")
    code = row.get("code", "")
    note = row.get("note", "")

    module_file = TLA_DIR / f"{module}.tla"
    if not module_file.exists():
        failures.append(f"{module}: tla/{module}.tla does not exist")

    if status != "owed":
        code_file = ROOT / code
        if not code_file.is_file():
            failures.append(f"{module}: {code} does not exist")
            return failures
        text = code_file.read_text(errors="replace")
        cite = f"tla/{module}.tla"
        if cite not in text:
            failures.append(f"{module}: {code} does not cite {cite}")
        for fn in sorted(FUNC_RE.findall(note)):
            if f"{fn}(" not in text:
                failures.append(f"{module}: {fn} not defined in {code}")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require", action="append", default=[], metavar="MODULES",
                        help="comma-separated modules that must each have a coverage row")
    parser.add_argument("--all", action="store_true",
                        help="every tla module (MC wrappers and Link excluded) must have a row")
    args = parser.parse_args()

    rows = list(iter_rows())
    failures = []
    for row in rows:
        failures.extend(check_row(row))

    have = {row["module"] for row in rows}
    required = set()
    for group in args.require:
        required.update(m for m in group.split(",") if m.strip())
    for module in sorted(required):
        if module not in have:
            failures.append(f"{module}: no coverage row")

    if args.all:
        for module in tla_modules():
            if module not in have:
                failures.append(f"{module}: no coverage row")

    if failures:
        for failure in sorted(failures):
            print(failure)
        print(f"COVERAGE FAIL {len(failures)}")
        return 1
    print(f"COVERAGE OK {len(rows)} rows")
    return 0


if __name__ == "__main__":
    sys.exit(main())
