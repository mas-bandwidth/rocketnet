#!/usr/bin/env python3
"""Select TLA+ cases for the pinned nightly runner.

Three selections:
  stale  every case of a stale group that is not timeout debt (tlc.yml's
         `Model records are current`);
  debt   every timeout-debt case, stale or not (tlc-debt.yml);
  rerun  every case that is not timeout debt, or one slice of them: the case
         whose slice key (SHA-256 of "<group>/<config>", never a line number)
         modulo SLICES is K, with K the UTC day number modulo SLICES
         (tlc.yml's `TLC re-run slice K/N` and `TLC re-run full`). A fresh
         result is compared against its RUNS.tsv record by
         tools/compare_tlc_rerun.py.

Emit group, full group cardinality, and zero-based shard index (and, for
rerun, the config). Pinned tlacheck 0c97d595 preserves CASES.tsv order in
ParseCases and Select; filtering to just selected cases before assigning
indices would run the wrong case.
"""

import argparse
import csv
import hashlib
import json
import sys
from pathlib import Path

# The scheduled re-run covers every non-debt case once every SLICES nights.
# Arithmetic (RUNS.tsv at 76edea48e: 894 non-debt cases, 5379 s of recorded
# single-case time; `--slice-report 1,2,3` prints the per-slice sums): the
# job runs nproc/2 cases at once, 2 on the fleet's smallest (4-CPU) runner.
# The largest slice's recorded time is 5379 s at N=1, 2721 s at N=2, 2025 s
# at N=3. On a loaded bench a case took 2.5 times its recorded time
# (MCRNSendSpans, 44 s recorded, 109 s), so a night is at most about
# max-slice * 2.5 / 2: N=1 6724 s, N=2 3401 s (57 min, past an hour with the
# build), N=3 2531 s (42 min). N=3 is the smallest that keeps a loaded night
# under an hour; the whole inventory is re-run every three nights. Unloaded,
# a 4-CPU slot of a bench ran the cases in 0.54 of their recorded time.
SLICES = 3


def keyed(rows, label):
    result = {}
    for row in rows:
        config = row["config"]
        if config in result:
            raise ValueError(f"{label}: duplicate config {config}")
        result[config] = row
    return result


def entries(cases, runs):
    """Every declared case once, in declaration order, as
    (group, cardinality, shard, config, debt)."""
    declarations = keyed(cases, "CASES.tsv")
    records = keyed(runs, "RUNS.tsv")
    unknown = records.keys() - declarations.keys()
    if unknown:
        raise ValueError("undeclared records: " + ", ".join(sorted(unknown)))
    groups = {}
    for case in cases:
        groups.setdefault(case["group"], []).append(case)
    out = []
    for group, members in groups.items():
        for shard, case in enumerate(members):
            record = records.get(case["config"], {})
            debt = (case["gate"] == "bench" and case["debt"] not in ("", "-")
                    and record.get("result") == "FAIL" and record.get("exit") == "124")
            out.append((group, len(members), shard, case["config"], debt))
    return out, groups


def plan(cases, runs, stale):
    """Return each selected case once, retaining its full group's shard position."""
    if not isinstance(stale, list) or any(not isinstance(g, str) for g in stale):
        raise ValueError("stale groups must be a JSON list of group names")
    every, groups = entries(cases, runs)
    unknown = set(stale) - groups.keys()
    if unknown:
        raise ValueError("unknown stale groups: " + ", ".join(sorted(unknown)))
    stale = set(stale)
    selected = []
    for group, count, shard, config, debt in every:
        if group in stale or debt:
            reason = ("stale+debt" if group in stale and debt
                      else "stale" if group in stale else "debt")
            selected.append((group, count, shard, config, reason))
    return selected


def choose(selected, which):
    """Keep the cases one workflow runs: the gate's are stale and not debt
    (tlc.yml); every debt case, stale or not, is the debt watch's
    (tlc-debt.yml)."""
    reasons = {"stale": ("stale",), "debt": ("debt", "stale+debt")}[which]
    return [entry for entry in selected if entry[4] in reasons]


def declared_debt(cases):
    """The cases CASES.tsv declares as timeout debt, whatever their records say."""
    return [case["config"] for case in cases
            if case["gate"] == "bench" and case["debt"] not in ("", "-")]


def slice_key(group, config):
    """The case's stable slice key: its group and config file name, never its
    position in CASES.tsv, so adding or moving a row moves no other case."""
    return int.from_bytes(hashlib.sha256(f"{group}/{config}".encode()).digest()[:8], "big")


def day_slice(day, slices=SLICES):
    """K for UTC day number `day` (days since 1970-01-01)."""
    if slices < 1 or day < 0:
        raise ValueError(f"bad day {day} or slice count {slices}")
    return day % slices


def rerun(cases, runs, index=None, slices=SLICES):
    """The non-debt cases of slice index/slices (all of them when index is
    None), as (group, cardinality, shard, config). An empty selection is an
    error: a night that re-runs nothing must never be green."""
    if index is not None and not 0 <= index < slices:
        raise ValueError(f"slice {index} is outside 0..{slices - 1}")
    every, _ = entries(cases, runs)
    picked = [(group, count, shard, config) for group, count, shard, config, debt in every
              if not debt and (index is None or slice_key(group, config) % slices == index)]
    if not picked:
        where = "full" if index is None else f"slice {index}/{slices}"
        raise ValueError(f"TLC-RERUN {where} holds zero cases")
    return picked


def slice_report(cases, runs, counts):
    """Per slice count: cases and recorded seconds per slice (min, mean, max)."""
    records = keyed(runs, "RUNS.tsv")
    every, _ = entries(cases, runs)
    lines = []
    for n in counts:
        seconds = [0.0] * n
        members = [0] * n
        for group, _, _, config, debt in every:
            if debt:
                continue
            k = slice_key(group, config) % n
            members[k] += 1
            seconds[k] += float(records.get(config, {}).get("seconds") or 0)
        lines.append(f"N={n} cases min={min(members)} max={max(members)} "
                     f"seconds min={min(seconds):.0f} mean={sum(seconds) / n:.0f} max={max(seconds):.0f} "
                     f"total={sum(seconds):.0f}")
    return lines


def read_tsv(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream, delimiter="\t"))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cases", type=Path, default=Path("proofs/tla/CASES.tsv"))
    parser.add_argument("--runs", type=Path, default=Path("proofs/tla/RUNS.tsv"))
    parser.add_argument("--stale", type=Path, help="tlacheck groups --stale JSON (stale, debt)")
    parser.add_argument("--select", choices=("stale", "debt", "rerun"), required=True)
    parser.add_argument("--day", type=int, help="rerun: UTC day number; the slice is day mod SLICES")
    parser.add_argument("--full", action="store_true", help="rerun: every non-debt case")
    parser.add_argument("--only", action="append", metavar="CONFIG",
                        help="rerun with --full: only these non-debt cases (repeatable), to re-run named cases on a bench")
    parser.add_argument("--outputs", action="store_true",
                        help="rerun: print the GitHub outputs (title, slice, count) instead of the plan")
    parser.add_argument("--slice-report", metavar="N,N,...",
                        help="print per-slice recorded seconds for each slice count, then exit")
    args = parser.parse_args()
    try:
        cases, runs = read_tsv(args.cases), read_tsv(args.runs)
        if args.slice_report:
            for line in slice_report(cases, runs, [int(n) for n in args.slice_report.split(",")]):
                print(line)
            return
        if args.select == "rerun":
            if args.full == (args.day is not None):
                raise ValueError("rerun needs exactly one of --day or --full")
            index = None if args.full else day_slice(args.day)
            picked = rerun(cases, runs, index)
            total = len(rerun(cases, runs))
            name = "full" if index is None else f"slice {index}/{SLICES}"
            if args.only:
                if not args.full:
                    raise ValueError("--only needs --full")
                unknown = set(args.only) - {entry[3] for entry in picked}
                if unknown:
                    raise ValueError("--only names no non-debt case: " + ", ".join(sorted(unknown)))
                picked = [entry for entry in picked if entry[3] in args.only]
                name = "named " + " ".join(args.only)
            if args.outputs:
                print(f"title=TLC re-run {name}")
                print(f"slice={name}")
                print(f"count={len(picked)}")
            else:
                for group, count, shard, config in picked:
                    print(group, count, shard, config)
                    print(f"TLC-PLAN rerun {config} ({group} shard {shard}/{count})", file=sys.stderr)
            day = "" if args.day is None else f" day={args.day}"
            print(f"TLC-RERUN {name}{day} cases={len(picked)} of {total} non-debt cases", file=sys.stderr)
            return
        if args.stale is None:
            raise ValueError(f"--select {args.select} needs --stale")
        selected = choose(plan(cases, runs, json.loads(args.stale.read_text())), args.select)
        if args.select == "debt" and not selected:
            declared = declared_debt(cases)
            if declared:
                raise ValueError("TLC-DEBT zero cases planned while CASES.tsv declares timeout debt: "
                                 + ", ".join(declared))
            print("TLC-DEBT CASES.tsv declares no timeout debt: nothing to run this week", file=sys.stderr)
    except (ValueError, KeyError, OSError) as error:
        parser.error(str(error))
    for group, count, shard, config, reason in selected:
        print(group, count, shard)
        print(f"TLC-PLAN {reason} {config} ({group} shard {shard}/{count})", file=sys.stderr)
    print(f"TLC-PLAN cases={len(selected)}", file=sys.stderr)


if __name__ == "__main__":
    main()
