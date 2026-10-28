#!/usr/bin/env python3
"""
Nightly performance trend recorder (nightly-perf-trend.yml).

Reads what the two existing measurements already print and records one trend
record per night; it measures nothing itself:

  - tools/check_instructions.py's report: one line per region,
      instructions region=<r> ir=<n> per_unit=<n> baseline=<n> delta_pct=<x%> verdict=<V>
    The instruction gate itself (bench/instruction_baseline.json, +2.0%) is
    check_instructions.py's own exit status; this tool records it and never
    second-guesses it.

  - bench/run_all.sh's run.txt: one summary line per matrix row,
      <harness>.txt: pps median=<n> min=<n> max=<n>
    (bench/check_windows.py's output; the honesty_* plant lines are not results).

Wall-clock pps is REPORT-ONLY. Each row is compared with the committed
baseline (bench/perf_trend_baseline.json) the way bench/PROTOCOL.md says two
sittings are compared: a median inside the other sitting's min/max spread is
TIED; outside it the row is said to be above or below the baseline spread.
Nothing here fails on a wall-clock number.

Subcommands:

  record   parse one night's outputs, print the comparison, append the record
           to the trend history (JSON lines) and write the history out.
  repin    write one OS's bench rows from a trend record into the baseline
           file, with a mandatory --reason, the same rule check_instructions.py
           keeps for its own baseline.

Exit codes: 0 recorded, 1 malformed input or refused repin.
"""

import argparse
import json
import os
import re
import sys
from typing import Any, Dict, List, Optional, Tuple

DEFAULT_BASELINE_PATH: str = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "bench",
    "perf_trend_baseline.json",
)

INSTRUCTION_LINE = re.compile(r"^instructions region=(\S+) ir=(\d+) .*delta_pct=(\S+)% verdict=(PASS|FAIL)\b")
BENCH_LINE = re.compile(r"^(\S+)\.txt: pps median=(\S+) min=(\S+) max=(\S+)$")
OS_NAMES: Tuple[str, ...] = ("linux", "macos")


def parse_instructions(text: str) -> Dict[str, Any]:
    """
    Parse check_instructions.py stdout. Returns {"regions": {region: {...}},
    "comparable": bool, "note": str}. A 'not comparable' or 'toolchain
    mismatch' report has no regions and says so in note.
    """
    regions: Dict[str, Dict[str, Any]] = {}
    note = ""
    comparable = True
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("not comparable:") or line.startswith("toolchain mismatch:"):
            comparable = False
            note = line
            continue
        m = INSTRUCTION_LINE.match(line)
        if m:
            regions[m.group(1)] = {
                "ir": int(m.group(2)),
                "delta_pct": float(m.group(3)),
                "verdict": m.group(4),
            }
    if comparable and not regions:
        raise ValueError("no 'instructions region=' lines in the instruction report")
    return {"regions": regions, "comparable": comparable, "note": note}


def parse_campaign(text: str) -> Dict[str, Any]:
    """
    Parse bench/run_all.sh run.txt. Returns {"rows": {row: {median,min,max}},
    "host": str, "cores": str, "complete": bool}. Raises if the campaign did
    not complete or produced no rows.
    """
    rows: Dict[str, Dict[str, float]] = {}
    host = ""
    cores = ""
    complete = False
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("host "):
            host = line[len("host "):].strip()
        elif line.startswith("cores "):
            cores = line[len("cores "):].strip()
        elif line.startswith("campaign complete"):
            complete = True
        m = BENCH_LINE.match(line)
        if m and not m.group(1).startswith("honesty_"):
            median, low, high = (float(m.group(i)) for i in (2, 3, 4))
            if not (0 < low <= median <= high):
                raise ValueError(f"inconsistent pps summary: {line!r}")
            rows[m.group(1)] = {"median": median, "min": low, "max": high}
    if not complete:
        raise ValueError("bench campaign did not complete (no 'campaign complete' line)")
    if not rows:
        raise ValueError("bench campaign produced no pps summary rows")
    return {"rows": rows, "host": host, "cores": cores, "complete": complete}


def compare_row(today: Dict[str, float], base: Optional[Dict[str, float]]) -> Tuple[str, Optional[float]]:
    """
    Compare one pps row against its baseline, per bench/PROTOCOL.md: a median
    inside the other sitting's spread is tied. Returns (status, delta_pct).
    """
    if not base:
        return "no baseline", None
    delta = (today["median"] - base["median"]) / base["median"] * 100.0
    if base["min"] <= today["median"] <= base["max"] or today["min"] <= base["median"] <= today["max"]:
        return "tied", delta
    if today["median"] < base["min"]:
        return "below baseline spread", delta
    return "above baseline spread", delta


def build_record(
    os_name: str,
    host: str,
    ref: str,
    commit: str,
    date: str,
    instructions: Optional[Dict[str, Any]],
    instructions_exit: Optional[int],
    campaign: Dict[str, Any],
) -> Dict[str, Any]:
    record: Dict[str, Any] = {
        "date": date,
        "os": os_name,
        "host": host,
        "ref": ref,
        "commit": commit,
        "bench": {"cores": campaign["cores"], "uname": campaign["host"], "rows": campaign["rows"]},
    }
    if instructions is not None:
        record["instructions"] = {
            "exit": instructions_exit,
            "comparable": instructions["comparable"],
            "note": instructions["note"],
            "regions": instructions["regions"],
        }
    return record


def previous_record(history: List[Dict[str, Any]], os_name: str) -> Optional[Dict[str, Any]]:
    for rec in reversed(history):
        if rec.get("os") == os_name:
            return rec
    return None


def report(record: Dict[str, Any], baseline: Dict[str, Any], previous: Optional[Dict[str, Any]]) -> str:
    lines: List[str] = []
    os_name = record["os"]
    lines.append(f"perf trend os={os_name} host={record['host']} ref={record['ref']} commit={record['commit']} date={record['date']}")

    ins = record.get("instructions")
    if ins is not None:
        if not ins["comparable"]:
            lines.append(f"instructions {ins['note']} gate_exit={ins['exit']}")
        prev_regions = ((previous or {}).get("instructions") or {}).get("regions", {})
        for region, r in sorted(ins["regions"].items()):
            nightly = ""
            if region in prev_regions and prev_regions[region]["ir"] > 0:
                d = (r["ir"] - prev_regions[region]["ir"]) / prev_regions[region]["ir"] * 100.0
                nightly = f" vs_previous={d:+.2f}%"
            lines.append(
                f"instructions region={region} ir={r['ir']} vs_baseline={r['delta_pct']:+.2f}%{nightly} verdict={r['verdict']} (gated, +2.0%)"
            )

    base_rows = ((baseline.get(os_name) or {}).get("bench") or {})
    prev_rows = ((previous or {}).get("bench") or {}).get("rows", {})
    for row, today in sorted(record["bench"]["rows"].items()):
        status, delta = compare_row(today, base_rows.get(row))
        delta_text = f" vs_baseline={delta:+.1f}%" if delta is not None else ""
        nightly = ""
        if row in prev_rows and prev_rows[row]["median"] > 0:
            d = (today["median"] - prev_rows[row]["median"]) / prev_rows[row]["median"] * 100.0
            nightly = f" vs_previous={d:+.1f}%"
        lines.append(
            f"bench row={row} pps_median={today['median']:g} min={today['min']:g} max={today['max']:g}"
            f"{delta_text}{nightly} {status} (report-only)"
        )
    return "\n".join(lines)


def repin(baseline: Dict[str, Any], record: Dict[str, Any], reason: str) -> Dict[str, Any]:
    clean = (reason or "").strip()
    if not clean:
        raise ValueError("repin requires a non-empty --reason <text>")
    os_name = record.get("os")
    if os_name not in OS_NAMES:
        raise ValueError(f"record has unknown os {os_name!r}")
    out = dict(baseline)
    out[os_name] = {
        "host": record["host"],
        "commit": record["commit"],
        "date": record["date"],
        "cores": record["bench"]["cores"],
        "reason": clean,
        "bench": record["bench"]["rows"],
    }
    return out


def read_history(path: Optional[str]) -> List[Dict[str, Any]]:
    if not path or not os.path.isfile(path):
        return []
    out: List[Dict[str, Any]] = []
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            raw = raw.strip()
            if raw:
                out.append(json.loads(raw))
    return out


def cmd_record(args: argparse.Namespace) -> int:
    with open(args.bench, "r", encoding="utf-8") as f:
        campaign = parse_campaign(f.read())
    instructions = None
    if args.instructions:
        with open(args.instructions, "r", encoding="utf-8") as f:
            instructions = parse_instructions(f.read())
    with open(args.baseline, "r", encoding="utf-8") as f:
        baseline = json.load(f)
    history = read_history(args.history)
    record = build_record(
        os_name=args.os,
        host=args.host,
        ref=args.ref,
        commit=args.commit,
        date=args.date,
        instructions=instructions,
        instructions_exit=args.instructions_exit,
        campaign=campaign,
    )
    text = report(record, baseline, previous_record(history, args.os))
    print(text)
    history.append(record)
    with open(args.out, "w", encoding="utf-8") as f:
        for rec in history:
            f.write(json.dumps(rec, sort_keys=True) + "\n")
    with open(args.record_out, "w", encoding="utf-8") as f:
        json.dump(record, f, indent=2, sort_keys=True)
        f.write("\n")
    return 0


def cmd_repin(args: argparse.Namespace) -> int:
    with open(args.record, "r", encoding="utf-8") as f:
        record = json.load(f)
    with open(args.baseline, "r", encoding="utf-8") as f:
        baseline = json.load(f)
    out = repin(baseline, record, args.reason)
    with open(args.baseline, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"repinned os={record['os']} rows={len(record['bench']['rows'])} into {args.baseline}")
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Record the nightly performance trend.")
    sub = parser.add_subparsers(dest="command", required=True)

    rec = sub.add_parser("record", help="record one night's results")
    rec.add_argument("--os", required=True, choices=OS_NAMES)
    rec.add_argument("--host", required=True)
    rec.add_argument("--ref", required=True)
    rec.add_argument("--commit", required=True)
    rec.add_argument("--date", required=True, help="UTC timestamp of the run, supplied by the caller")
    rec.add_argument("--bench", required=True, help="bench/run_all.sh run.txt")
    rec.add_argument("--instructions", help="tools/check_instructions.py stdout (Linux only)")
    rec.add_argument("--instructions-exit", type=int, help="tools/check_instructions.py exit status")
    rec.add_argument("--baseline", default=DEFAULT_BASELINE_PATH)
    rec.add_argument("--history", help="previous trend history (JSON lines); absent on the first night")
    rec.add_argument("--out", required=True, help="trend history to write (JSON lines)")
    rec.add_argument("--record-out", required=True, help="this night's record alone (JSON)")

    rp = sub.add_parser("repin", help="write one OS's bench rows from a record into the baseline")
    rp.add_argument("--record", required=True)
    rp.add_argument("--reason", required=True)
    rp.add_argument("--baseline", default=DEFAULT_BASELINE_PATH)

    args = parser.parse_args(argv)
    try:
        if args.command == "record":
            return cmd_record(args)
        return cmd_repin(args)
    except (ValueError, OSError, KeyError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
