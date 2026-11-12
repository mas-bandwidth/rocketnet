#!/usr/bin/env python3
"""Select TLA+ cases for the pinned nightly runner.

Three selections:
  stale  every case of a stale group that is not timeout debt (tlc.yml's
         `Model records are current`);
  debt   every timeout-debt case, stale or not, except the nightly debt
         (tlc-debt.yml);
  rerun  every case that is not timeout debt, or one slice of them: the case
         whose slice key (SHA-256 of "<group>/<config>", never a line number)
         modulo SLICES is K, with K the UTC day number modulo SLICES
         (tlc.yml's `TLC re-run slice K/N` and `TLC re-run full`); and, in
         every slice and first, each nightly-debt case. A fresh result is
         compared against its RUNS.tsv record (a nightly-debt case: against
         its NIGHTLY-DEBT.tsv row) by tools/compare_tlc_rerun.py.

Nightly debt (proofs/tla/NIGHTLY-DEBT.tsv: config, budget_s, measured_s,
host, generated, distinct) is timeout debt that finishes in minutes: over the
pinned tlacheck's 110 s record cap, so its bounded record stays FAIL/124, but
short enough to run every night under its own budget_s instead of weekly.
nightly_debt() refuses a row whose config is not declared timeout debt with a
FAIL/124 record, a budget_s under twice measured_s, or a malformed number;
tools/check_tla_records.py runs the same check on every commit.

Emit group, full group cardinality, and zero-based shard index (and, for
rerun, the config and the budget in seconds, 0 for none). Pinned tlacheck 0c97d595 preserves CASES.tsv order in
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
# The nightly debt runs in every slice: its slowest measurements (taken on
# loaded benches) sum to about 1010 s, 505 s at two at once, so a loaded N=3
# night is about 2531 + 505 = 3036 s (51 min).
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


NIGHTLY_DEBT_COLUMNS = ("config", "budget_s", "measured_s", "host", "generated", "distinct")


def positive(value, kind):
    try:
        number = kind(value)
    except (TypeError, ValueError):
        return None
    return number if number > 0 else None


def nightly_debt(rows, cases, runs, columns=NIGHTLY_DEBT_COLUMNS):
    """The NIGHTLY-DEBT.tsv rows by config, after refusing (every problem at
    once) a row that is not declared timeout debt with its bounded FAIL/124
    record, a duplicate, a budget_s under twice measured_s, or a count or time
    that is not a positive number."""
    if tuple(columns or ()) != NIGHTLY_DEBT_COLUMNS:
        raise ValueError(f"NIGHTLY-DEBT.tsv columns {tuple(columns or ())} != {NIGHTLY_DEBT_COLUMNS}")
    declared = set(declared_debt(cases))
    records = keyed(runs, "RUNS.tsv")
    problems, out = [], {}
    for row in rows:
        config = row["config"]
        if config in out:
            problems.append(f"{config}: listed twice")
            continue
        out[config] = row
        if config not in declared:
            problems.append(f"{config}: not declared timeout debt in CASES.tsv (gate bench with a debt reason)")
        record = records.get(config, {})
        if (record.get("result"), record.get("exit")) != ("FAIL", "124"):
            problems.append(f"{config}: its RUNS.tsv record is not the bounded FAIL/124 of timeout debt "
                            f"(result={record.get('result')!r}, exit={record.get('exit')!r})")
        budget, measured = positive(row["budget_s"], float), positive(row["measured_s"], float)
        if budget is None or measured is None:
            problems.append(f"{config}: budget_s {row['budget_s']!r} and measured_s {row['measured_s']!r} "
                            "must be positive numbers of seconds")
        elif budget < 2 * measured:
            problems.append(f"{config}: budget_s {row['budget_s']} is under twice measured_s {row['measured_s']}")
        if positive(row["generated"], int) is None or positive(row["distinct"], int) is None:
            problems.append(f"{config}: generated {row['generated']!r} and distinct {row['distinct']!r} "
                            "must be the positive counts of a complete run")
    if problems:
        raise ValueError("NIGHTLY-DEBT.tsv: " + "; ".join(problems))
    return out


def read_nightly_debt(path, cases, runs):
    """NIGHTLY-DEBT.tsv checked against the cases and records; no file is no nightly debt."""
    if not path.exists():
        return {}
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        rows = list(reader)
        return nightly_debt(rows, cases, runs, reader.fieldnames)


def slice_key(group, config):
    """The case's stable slice key: its group and config file name, never its
    position in CASES.tsv, so adding or moving a row moves no other case."""
    return int.from_bytes(hashlib.sha256(f"{group}/{config}".encode()).digest()[:8], "big")


def day_slice(day, slices=SLICES):
    """K for UTC day number `day` (days since 1970-01-01)."""
    if slices < 1 or day < 0:
        raise ValueError(f"bad day {day} or slice count {slices}")
    return day % slices


def rerun(cases, runs, index=None, slices=SLICES, nightly=None):
    """Every nightly-debt case first (in every slice, each under its
    budget_s), then the non-debt cases of slice index/slices (all of them
    when index is None, budget 0: none), as (group, cardinality, shard,
    config, budget). An empty selection is an error: a night that re-runs
    nothing must never be green."""
    if index is not None and not 0 <= index < slices:
        raise ValueError(f"slice {index} is outside 0..{slices - 1}")
    nightly = nightly or {}
    every, _ = entries(cases, runs)
    picked = [(group, count, shard, config, nightly[config]["budget_s"])
              for group, count, shard, config, debt in every if debt and config in nightly]
    picked += [(group, count, shard, config, "0") for group, count, shard, config, debt in every
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
    parser.add_argument("--nightly-debt", type=Path, default=Path("proofs/tla/NIGHTLY-DEBT.tsv"),
                        help="the timeout debt run every night under its own budget (rerun) and left out of debt")
    parser.add_argument("--stale", type=Path, help="tlacheck groups --stale JSON (stale, debt)")
    parser.add_argument("--select", choices=("stale", "debt", "rerun"), required=True)
    parser.add_argument("--day", type=int, help="rerun: UTC day number; the slice is day mod SLICES")
    parser.add_argument("--full", action="store_true", help="rerun: every non-debt and nightly-debt case")
    parser.add_argument("--only", action="append", metavar="CONFIG",
                        help="rerun with --full: only these cases (repeatable), to re-run named cases on a bench")
    parser.add_argument("--outputs", action="store_true",
                        help="rerun: print the GitHub outputs (title, slice, count) instead of the plan")
    parser.add_argument("--slice-report", metavar="N,N,...",
                        help="print per-slice recorded seconds for each slice count, then exit")
    args = parser.parse_args()
    try:
        cases, runs = read_tsv(args.cases), read_tsv(args.runs)
        nightly = read_nightly_debt(args.nightly_debt, cases, runs)
        if args.slice_report:
            for line in slice_report(cases, runs, [int(n) for n in args.slice_report.split(",")]):
                print(line)
            return
        if args.select == "rerun":
            if args.full == (args.day is not None):
                raise ValueError("rerun needs exactly one of --day or --full")
            index = None if args.full else day_slice(args.day)
            picked = rerun(cases, runs, index, nightly=nightly)
            total = len(rerun(cases, runs, nightly=nightly))
            name = "full" if index is None else f"slice {index}/{SLICES}"
            if args.only:
                if not args.full:
                    raise ValueError("--only needs --full")
                unknown = set(args.only) - {entry[3] for entry in picked}
                if unknown:
                    raise ValueError("--only names no re-run case: " + ", ".join(sorted(unknown)))
                picked = [entry for entry in picked if entry[3] in args.only]
                name = "named " + " ".join(args.only)
            if args.outputs:
                print(f"title=TLC re-run {name}")
                print(f"slice={name}")
                print(f"count={len(picked)}")
            else:
                for group, count, shard, config, budget in picked:
                    print(group, count, shard, config, budget)
                    limit = f" budget {budget} s" if budget != "0" else ""
                    print(f"TLC-PLAN rerun {config} ({group} shard {shard}/{count}){limit}", file=sys.stderr)
            day = "" if args.day is None else f" day={args.day}"
            debts = sum(1 for entry in picked if entry[4] != "0")
            print(f"TLC-RERUN {name}{day} cases={len(picked)} ({debts} nightly debt) of {total} re-run cases",
                  file=sys.stderr)
            return
        if args.stale is None:
            raise ValueError(f"--select {args.select} needs --stale")
        selected = choose(plan(cases, runs, json.loads(args.stale.read_text())), args.select)
        if args.select == "debt":
            # The nightly debt runs in tlc.yml's re-run every night, not here.
            for config in sorted(nightly):
                print(f"TLC-DEBT {config} is nightly debt (tlc.yml re-run, budget {nightly[config]['budget_s']} s)",
                      file=sys.stderr)
            selected = [entry for entry in selected if entry[3] not in nightly]
        if args.select == "debt" and not selected:
            declared = [config for config in declared_debt(cases) if config not in nightly]
            if declared:
                raise ValueError("TLC-DEBT zero cases planned while CASES.tsv declares timeout debt: "
                                 + ", ".join(declared))
            print("TLC-DEBT CASES.tsv declares no weekly timeout debt: nothing to run this week", file=sys.stderr)
    except (ValueError, KeyError, OSError) as error:
        parser.error(str(error))
    for group, count, shard, config, reason in selected:
        print(group, count, shard)
        print(f"TLC-PLAN {reason} {config} ({group} shard {shard}/{count})", file=sys.stderr)
    print(f"TLC-PLAN cases={len(selected)}", file=sys.stderr)


if __name__ == "__main__":
    main()
