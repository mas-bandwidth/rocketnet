#!/usr/bin/env python3
"""
Per-call server timing report and budget check for load_split.

Reads one or more unmodified load_split server logs (docs/LOAD_SPLIT.md) and
prints one row per log and main server call: rocketnet_server_update,
receive_messages, start_message, finish_message and send_packets. The budget
for each call is 100 microseconds at p99, measured in Release with the server
alone on its machine and the client shards on other machines.

The budget is judged at the worst case only: 1000 clients, all connected, in
the full load mode (every client sends and receives one message every
update). Rows at other populations or loads are printed for correctness with
verdict NA and never pass or fail the budget; at least one worst-case log is
required.

Every log must carry exactly one split_server_call line for each of the five
calls, with one run ID, population and transport; anything else is invalid
evidence and exits 2. With --report-only the check prints the table and the
verdict and exits 0 on a budget miss. Without it a budget miss exits 1.
"""

import argparse
import sys

BUDGET_US = 100
GATED_TOTAL = 1000
GATED_LOAD = "full"
CALLS = (
    "server_update",
    "server_receive_messages",
    "server_start_message",
    "server_finish_message",
    "server_send_packets",
)
PREFIX = "split_server_call: "


def parse_fields(line):
    fields = {}
    for token in line[len(PREFIX):].split():
        if "=" not in token:
            raise ValueError(f"malformed field {token!r}")
        key, value = token.split("=", 1)
        if key in fields:
            raise ValueError(f"duplicated field {key}")
        fields[key] = value
    return fields


def integer(fields, key):
    value = fields.get(key)
    if value is None or not value.isdigit():
        raise ValueError(f"{key}: missing or not an integer")
    return int(value)


def parse_log(text):
    """Return (total, transport, load, {call: fields}) for one server log."""
    calls = {}
    identity = None
    for line in text.splitlines():
        if not line.startswith(PREFIX):
            continue
        fields = parse_fields(line)
        call = fields.get("call")
        if call not in CALLS:
            raise ValueError(f"unknown call {call!r}")
        if call in calls:
            raise ValueError(f"{call}: duplicated")
        if fields.get("budget_us") != str(BUDGET_US) or fields.get("scope") != "traffic_plus_drain":
            raise ValueError(f"{call}: wrong budget or scope")
        if fields.get("load") not in ("full", "rate_1hz"):
            raise ValueError(f"{call}: missing or unknown load")
        key = (fields.get("run_id"), integer(fields, "total"), fields.get("transport"), fields.get("load"))
        if identity is None:
            identity = key
        elif key != identity:
            raise ValueError(f"{call}: mixed run, population, transport or load")
        count = integer(fields, "count")
        p50 = integer(fields, "p50_upper_us")
        p99 = integer(fields, "p99_upper_us")
        if count == 0:
            raise ValueError(f"{call}: no samples")
        if p50 > p99:
            raise ValueError(f"{call}: quantiles contradict")
        calls[call] = fields
    missing = [call for call in CALLS if call not in calls]
    if missing:
        raise ValueError("missing calls: " + ", ".join(missing))
    return identity[1], identity[2], identity[3], calls


def report(logs, out):
    """Print the table; return the list of (total, transport, call, p99) over budget."""
    rows = []
    for name, text in logs:
        try:
            total, transport, load, calls = parse_log(text)
        except ValueError as error:
            raise ValueError(f"{name}: {error}") from None
        rows.append((total, transport, load, calls))
    if not any(total == GATED_TOTAL and load == GATED_LOAD for total, _, load, _ in rows):
        raise ValueError(f"no worst-case log ({GATED_TOTAL} clients, load={GATED_LOAD})")
    rows.sort(key=lambda row: (row[1], row[2], row[0]))
    over = []
    out.write("clients transport load call p50_us p99_us max_us count verdict\n")
    for total, transport, load, calls in rows:
        gated = total == GATED_TOTAL and load == GATED_LOAD
        for call in CALLS:
            fields = calls[call]
            p99 = int(fields["p99_upper_us"])
            verdict = "NA" if not gated else "PASS" if p99 <= BUDGET_US else "FAIL"
            if verdict == "FAIL":
                over.append((total, transport, call, p99))
            p50 = fields["p50_upper_us"]
            max_us = fields.get("max_us", "na")
            count = fields["count"]
            out.write(f"{total} {transport} {load} {call} {p50} {p99} {max_us} {count} {verdict}\n")
    return over


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("logs", nargs="+", help="load_split server logs, one per population")
    parser.add_argument("--report-only", action="store_true",
                        help="print the verdict but exit 0 on a budget miss")
    args = parser.parse_args(argv)
    logs = []
    for path in args.logs:
        with open(path, encoding="utf-8") as handle:
            logs.append((path, handle.read()))
    try:
        over = report(logs, sys.stdout)
    except ValueError as error:
        print(f"server_calls: INVALID {error}")
        return 2
    verdict = "PASS" if not over else "FAIL"
    print(f"server_calls: verdict={verdict} budget_us={BUDGET_US} over_budget={len(over)}"
          + (" report_only=1" if args.report_only else ""))
    if over and not args.report_only:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
