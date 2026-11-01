#!/usr/bin/env python3
"""Select stale groups and recorded timeout debt for the pinned nightly runner.

Emit group, full group cardinality, and zero-based shard index. Pinned tlacheck
0c97d595 preserves CASES.tsv order in ParseCases and Select; filtering to just
selected cases before assigning indices would run the wrong case.
"""

import argparse
import csv
import json
import sys
from pathlib import Path


def keyed(rows, label):
    result = {}
    for row in rows:
        config = row["config"]
        if config in result:
            raise ValueError(f"{label}: duplicate config {config}")
        result[config] = row
    return result


def plan(cases, runs, stale):
    """Return each selected case once, retaining its full group's shard position."""
    declarations = keyed(cases, "CASES.tsv")
    records = keyed(runs, "RUNS.tsv")
    if not isinstance(stale, list) or any(not isinstance(g, str) for g in stale):
        raise ValueError("stale groups must be a JSON list of group names")
    groups = {}
    for case in cases:
        groups.setdefault(case["group"], []).append(case)
    unknown = set(stale) - groups.keys()
    if unknown:
        raise ValueError("unknown stale groups: " + ", ".join(sorted(unknown)))
    unknown = records.keys() - declarations.keys()
    if unknown:
        raise ValueError("undeclared records: " + ", ".join(sorted(unknown)))

    stale = set(stale)
    selected = []
    for group, members in groups.items():
        for shard, case in enumerate(members):
            record = records.get(case["config"], {})
            debt = (case["gate"] == "bench" and case["debt"] not in ("", "-")
                    and record.get("result") == "FAIL" and record.get("exit") == "124")
            if group in stale or debt:
                reason = ("stale+debt" if group in stale and debt
                          else "stale" if group in stale else "debt")
                selected.append((group, len(members), shard, case["config"], reason))
    return selected


def read_tsv(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream, delimiter="\t"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cases", type=Path, default=Path("proofs/tla/CASES.tsv"))
    parser.add_argument("--runs", type=Path, default=Path("proofs/tla/RUNS.tsv"))
    parser.add_argument("--stale", type=Path, required=True)
    args = parser.parse_args()
    try:
        selected = plan(read_tsv(args.cases), read_tsv(args.runs), json.loads(args.stale.read_text()))
    except (ValueError, KeyError, OSError) as error:
        parser.error(str(error))
    for group, count, shard, config, reason in selected:
        print(group, count, shard)
        print(f"TLC-PLAN {reason} {config} ({group} shard {shard}/{count})", file=sys.stderr)
    print(f"TLC-PLAN cases={len(selected)}", file=sys.stderr)


if __name__ == "__main__":
    main()
