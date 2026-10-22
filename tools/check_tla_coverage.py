#!/usr/bin/env python3
"""Check every tla/COVERAGE.tsv row against its model and code."""

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COVERAGE = ROOT / "tla" / "COVERAGE.tsv"
TLA_DIR = ROOT / "tla"

FUNC_RE = re.compile(r"(?:rn_|rocketnet_)[A-Za-z0-9_]+(?=,|\)|and\b)")
DST_CHECK_RE = re.compile(r'rocketnet_dst_check_fail\(\s*\w+,\s*"([a-z0-9_]+)"')


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


def dst_check_names():
    path = ROOT / "tests" / "dst_checks.h"
    if not path.is_file():
        return set()
    return set(DST_CHECK_RE.findall(path.read_text(errors="replace")))


def check_row(row, dst_names):
    failures = []
    module = row["module"]
    machine = row.get("machine", "")
    status = row.get("status", "")
    code = row.get("code", "")
    note = row.get("note", "")

    if status == "none":
        if module != "-":
            failures.append(f"{module}: {machine} a row with no model must name - as its module")
        if "," in code:
            failures.append(f"{module}: {machine} code column must hold one path, one per row")
            return failures
        if not (ROOT / code).is_file():
            failures.append(f"{module}: {code} does not exist")
            return failures
        if "no model" not in note.lower():
            failures.append(f"{module}: {machine} a row with no model must say why the code is safe")
        return failures

    if status == "dst":
        if module != "-":
            module_file = TLA_DIR / f"{module}.tla"
            if not module_file.exists():
                failures.append(f"{module}: tla/{module}.tla does not exist")
        if "," in code:
            failures.append(f"{module}: {machine} code column must hold one path, one per row")
            return failures
        if not (ROOT / code).is_file():
            failures.append(f"{module}: {code} does not exist")
            return failures
        if not any(re.search(rf"\b{re.escape(name)}\b", note) for name in dst_names):
            failures.append(f"{module}: {machine} a dst row must name the DST check in its note")
        return failures

    if status != "owed" and status not in ("current", "covered"):
        failures.append(f"{module}: {machine} unknown status {status}")
        return failures

    module_file = TLA_DIR / f"{module}.tla"
    if not module_file.exists():
        failures.append(f"{module}: tla/{module}.tla does not exist")

    if "," in code:
        failures.append(f"{module}: {machine} code column must hold one path, one per row")
        return failures

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


def owed_but_cited(rows):
    """An owed row says no code implements its model; a model that a file
    under sdk/ cites is implemented, so its owed row is stale. The citation
    counts with or without the tla/ prefix: sdk/rocketnet_client_receive_
    handshake.h writes RNClientIo.tla bare, seven times."""
    failures = []
    sdk = ROOT / "sdk"
    texts = {}
    if sdk.is_dir():
        for path in sorted(sdk.rglob("*")):
            if path.is_file():
                texts[path.relative_to(ROOT).as_posix()] = path.read_text(errors="replace")
    for row in rows:
        if row.get("status", "") != "owed":
            continue
        module = row["module"]
        forms = (f"tla/{module}.tla", f"{module}.tla")
        for name, text in texts.items():
            cite = next((form for form in forms if form in text), None)
            if cite:
                failures.append(f"{module}: {row.get('machine', '')} is owed but {name} cites {cite}")
                break
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
    dst_names = dst_check_names()
    for row in rows:
        failures.extend(check_row(row, dst_names))

    for machine, count in Counter(row["machine"] for row in rows).items():
        if count > 1:
            failures.append(f"{machine}: row appears twice in tla/COVERAGE.tsv")

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

    failures.extend(owed_but_cited(rows))

    if failures:
        for failure in sorted(failures):
            print(failure)
        print(f"COVERAGE FAIL {len(failures)}")
        return 1
    print(f"COVERAGE OK {len(rows)} rows")
    return 0


if __name__ == "__main__":
    sys.exit(main())
