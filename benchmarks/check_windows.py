#!/usr/bin/env python3
"""Validate benchmark WINDOW records and compute the rate summary."""
import argparse
import math
from pathlib import Path
import re
import statistics
import sys


def validate(text, windows, expected_rate=None, tolerance=0.10):
    rows = []
    for line in text.splitlines():
        if not re.match(r"^WINDOW \d+ secs=", line):
            continue
        values = dict(re.findall(r"(\w+)=(\S+)", line))
        required = ("secs", "pkts", "pps", "bytes", "p50_ns", "p99_ns", "syscalls", "empty", "timed", "timed_pkts", "fold")
        if any(key not in values for key in required):
            raise ValueError("incomplete WINDOW record")
        secs, rate = float(values["secs"]), float(values["pps"])
        packets, byte_count = int(values["pkts"]), int(values["bytes"])
        counters = [int(values[key]) for key in required[4:-1]]
        int(values["fold"], 16)
        if not math.isfinite(secs) or not math.isfinite(rate) or secs <= 0 or rate <= 0 or packets <= 0 or byte_count <= 0 or any(n < 0 for n in counters):
            raise ValueError("invalid or empty WINDOW")
        if abs(rate - packets / secs) > max(1, rate * .002):
            raise ValueError("WINDOW rate disagrees with packet count and duration")
        if expected_rate is not None and abs(rate - expected_rate) > expected_rate * tolerance:
            raise ValueError(f"pace outside {tolerance:.0%} tolerance of {expected_rate:g} pps")
        rows.append((int(line.split()[1]), rate))
    if len(rows) != windows or len({n for n, _ in rows}) != windows:
        raise ValueError(f"expected {windows} distinct WINDOW records, found {len(rows)}")
    rates = [rate for _, rate in rows]
    return statistics.median(rates), min(rates), max(rates)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--windows", type=int, required=True)
    parser.add_argument("--expected-rate", type=float)
    args = parser.parse_args()
    if args.windows < 1 or (args.expected_rate is not None and (not math.isfinite(args.expected_rate) or args.expected_rate <= 0)):
        parser.error("windows and expected rate must be positive")
    try:
        median, low, high = validate(args.log.read_text(), args.windows, args.expected_rate)
    except (ValueError, OSError) as error:
        print(f"INVALID {args.log}: {error}", file=sys.stderr)
        return 1
    print(f"{args.log.name}: pps median={median:g} min={low:g} max={high:g}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
