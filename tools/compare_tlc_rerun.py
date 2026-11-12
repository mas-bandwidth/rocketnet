#!/usr/bin/env python3
"""Compare fresh TLC re-runs against their committed RUNS.tsv records.

tlc.yml's `TLC re-run slice K/N` and `TLC re-run full` run each planned case
(tools/plan_tlc_nightly.py --select rerun) with the pinned nightly adapter
(tools/tlc_nightly.go), which writes <evidence>/run-<group>-<shard>/result.json.
This holds every fresh result to its record and fails by case name on any
difference in:

  result       the fresh run reached its declared result (PASS), as the record says
  exit         TLC's exit status (0 pass, 12 invariant, 13 action or temporal)
  violated     no violation for a pass case; for a witness, every violation TLC
               reported is of the declared kind and names the record's property
               (TLC may leave a temporal property unnamed)
  fingerprint  the engine's input SHA-256 equals the record's input_sha256
  jar          the TLC JAR digest equals the record's
  workers      the adapter's worker count equals the record's
  generated,   the state counts. A witness runs on one worker, so its search
  distinct     and the counts at its stop are deterministic. A pass case runs
               on two workers but explores the whole reachable state space, so
               its distinct count (the reachable set) and generated count (the
               successors of each distinct state, computed once) do not depend
               on the schedule; the bench run that set this rule compared all
               of them (see tlc.yml).

A nightly-debt case (plan budget not 0; proofs/tla/NIGHTLY-DEBT.tsv) has a
bounded FAIL/124 record, because the pinned tlacheck caps a record at 110 s.
It is held to the completed result instead: result PASS, the exit of its
declared result, and its NIGHTLY-DEBT.tsv generated and distinct counts;
fingerprint, JAR and workers still come from its RUNS.tsv record. A run its
budget_s stopped is a `budget` mismatch.

Exit 0 when every planned case matches, 1 on any mismatch (each named on a
`TLC-RERUN MISMATCH <config> <field>` line), 2 on a usage error.
"""

import argparse
import csv
import json
import sys
from pathlib import Path

KINDS = {"invariant": "Invariant", "action": "Action property", "temporal": "Temporal property"}


def read_plan(path):
    """Plan lines: group, cardinality, shard, config, budget seconds (0 for none)."""
    planned = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        fields = line.split()
        if len(fields) != 5:
            raise ValueError(f"{path}:{number}: want 'group cardinality shard config budget', got {line!r}")
        planned.append((fields[0], int(fields[1]), int(fields[2]), fields[3], fields[4]))
    if not planned:
        raise ValueError(f"{path}: the plan holds zero cases")
    return planned


def violations_differ(record, fresh):
    """A description of how the fresh violations differ from the record's
    declared one, or None when they agree."""
    expected = record.get("expected", "")
    found = [(v.get("kind", ""), v.get("name", "")) for v in fresh.get("violated") or []]
    shown = ",".join(f"{kind}:{name or '-'}" for kind, name in found) or "-"
    if fresh.get("violated") is None:
        return "the evidence carries no violated list"
    if expected == "pass":
        return None if not found else f"record=- fresh={shown}"
    kind = KINDS.get(expected)
    if kind is None:
        return f"record expected={expected!r} is not a known result"
    names = record.get("property", "").split("|")
    want = f"{kind}:{record.get('property', '')}"
    if not found:
        return f"record={want} fresh=-"
    for got_kind, got_name in found:
        if got_kind != kind or (got_name not in names and not (kind == "Temporal property" and got_name == "")):
            return f"record={want} fresh={shown}"
    return None


def compare_case(record, fresh):
    """(field, record value, fresh value) for every difference."""
    out = []

    def same(field, want, got):
        if str(want) != str(got):
            out.append((field, want, got))

    same("result", record.get("result"), fresh.get("result"))
    same("exit", record.get("exit"), fresh.get("exit"))
    differ = violations_differ(record, fresh)
    if differ:
        out.append(("violated", differ, ""))
    same("fingerprint", record.get("input_sha256"), fresh.get("engine_input_sha256"))
    same("jar", record.get("jar_sha256"), fresh.get("jar_sha256"))
    same("workers", record.get("workers"), fresh.get("workers"))
    same("generated", record.get("generated"), fresh.get("generated"))
    same("distinct", record.get("distinct"), fresh.get("distinct"))
    return out


EXITS = {"pass": "0", "invariant": "12", "action": "13", "temporal": "13"}


def completed(record, row):
    """What a nightly-debt case's fresh run must show: its record with the
    bounded FAIL/124 replaced by the completed result its NIGHTLY-DEBT.tsv row
    measured."""
    want = dict(record)
    want.update(result="PASS", exit=EXITS.get(record.get("expected"), "?"),
                generated=row["generated"], distinct=row["distinct"])
    return want


def compare(planned, records, evidence, out=sys.stdout, nightly=None):
    """Compare each planned case; return the configs that differ."""
    failed = []
    nightly = nightly or {}
    for group, _count, shard, config, budget in planned:
        problems = []
        record = records.get(config)
        if record is not None and budget != "0":
            if config in nightly:
                record = completed(record, nightly[config])
            else:
                problems.append(("nightly-debt", f"planned with budget {budget} s but not in NIGHTLY-DEBT.tsv", ""))
        path = evidence / f"run-{group}-{shard}" / "result.json"
        fresh = None
        if record is None:
            problems.append(("record", "missing from RUNS.tsv", ""))
        try:
            fresh = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            problems.append(("evidence", f"no readable {path}", str(error)))
        if fresh is not None:
            got = (fresh.get("case") or {}).get("Config")
            if got != config:
                problems.append(("case", config, got))
            elif record is not None:
                if fresh.get("interrupted"):
                    problems.append(("budget", f"stopped at its budget of {budget} s", ""))
                problems.extend(compare_case(record, fresh))
        if problems:
            failed.append(config)
            for field, want, got in problems:
                detail = want if got == "" else f"record={want} fresh={got}"
                print(f"TLC-RERUN MISMATCH {config} {field}: {detail}", file=out)
        else:
            violated = ",".join(f"{v['kind']}:{v['name'] or '-'}" for v in fresh["violated"]) or "-"
            print(f"TLC-RERUN MATCH {config} result={fresh['result']} exit={fresh['exit']} "
                  f"violated={violated} generated={fresh['generated']} distinct={fresh['distinct']} "
                  f"seconds={fresh.get('seconds', 0):.1f} recorded={record.get('seconds')}", file=out)
    return failed


def read_records(path):
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream, delimiter="\t"))
    records = {}
    for row in rows:
        if row["config"] in records:
            raise ValueError(f"{path}: duplicate config {row['config']}")
        records[row["config"]] = row
    return records


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runs", type=Path, default=Path("proofs/tla/RUNS.tsv"))
    parser.add_argument("--plan", type=Path, required=True, help="plan_tlc_nightly.py --select rerun output")
    parser.add_argument("--evidence", type=Path, required=True, help="directory holding run-<group>-<shard>/")
    parser.add_argument("--nightly-debt", type=Path, default=Path("proofs/tla/NIGHTLY-DEBT.tsv"),
                        help="the completed counts of the nightly-debt cases (plan budget not 0)")
    args = parser.parse_args(argv)
    try:
        planned = read_plan(args.plan)
        records = read_records(args.runs)
        nightly = read_records(args.nightly_debt) if args.nightly_debt.exists() else {}
    except (ValueError, OSError, KeyError) as error:
        parser.error(str(error))
    failed = compare(planned, records, args.evidence, nightly=nightly)
    if failed:
        print(f"TLC-RERUN-FAILED {len(failed)} of {len(planned)} cases differ from their records: "
              + " ".join(failed))
        return 1
    print(f"TLC-RERUN-OK {len(planned)} cases match their records (result, exit, violated property, "
          "fingerprint, JAR, workers, generated and distinct counts)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
