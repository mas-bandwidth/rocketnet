#!/usr/bin/env python3
"""Summarise the real-load nightly: one row per server transport.

Reads the run directories that scripts/lan-proof leaves behind (server.log,
merge.log, row.txt and its own console log) and prints, per cell, the server
call timing (p50, p99, p999 upper bounds and maximum, in microseconds),
connects, disconnects, delivery drops and the cell verdict. Writes the rows
as TSV and JSON for the run artifact and as a Markdown table for the job
summary.

A cell fails when its run directory has no server receipt, when fewer than
every client connected, when the server or any role saw a disconnect
(a timeout or a lost connection), or when the merger's proof cell is not
PASS.

  python3 tools/real_load_report.py --out DIR CELL=RUN_DIR [CELL=RUN_DIR ...]

Exits 0 when every cell passes, 1 when any cell fails, 2 on bad usage.
"""

import argparse
import json
import os
import sys

CALLS = ("server_update", "server_receive_messages", "server_start_message",
         "server_finish_message", "server_send_packets")
TIMED_CALLS = ("server_update", "server_send_packets")
COLUMNS = ("cell", "status", "clients", "connected", "disconnects", "unexpected_disconnects",
           "c2s_sent", "c2s_received", "s2c_sent", "s2c_received", "unreliable_lost",
           "sequenced_skipped", "update_p99_us", "update_p999_us", "update_max_us",
           "send_p99_us", "send_p999_us", "send_max_us", "reason")


def fields_of(line, prefix):
    """The key=value fields of one log line after its prefix."""
    out = {}
    for token in line[len(prefix):].split():
        if "=" in token:
            key, value = token.split("=", 1)
            out[key] = value
    return out


def last_line(text, prefix, extra=""):
    found = None
    for line in text.splitlines():
        if line.startswith(prefix) and extra in line:
            found = line
    return fields_of(found, prefix) if found is not None else None


def read(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def to_int(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def summarise(cell, run_dir):
    """One result row for one cell's run directory."""
    row = {key: "" for key in COLUMNS}
    row["cell"] = cell
    server = read(os.path.join(run_dir, "server.log"))
    merge = read(os.path.join(run_dir, "merge.log"))
    console = read(os.path.join(run_dir, "lan-proof.console"))
    reasons = []

    receipt = last_line(server, "split_receipt: ", "role=server")
    proof = last_line(server, "split_proof: ", "role=server")
    cell_line = last_line(merge, "split_proof_cell: ")
    calls = {}
    for line in server.splitlines():
        if line.startswith("split_server_call: "):
            fields = fields_of(line, "split_server_call: ")
            calls[fields.get("call")] = fields

    if receipt is None:
        refusal = ""
        for line in console.splitlines():
            if line.startswith("lan-proof: "):
                refusal = line[len("lan-proof: "):]
        reasons.append("no server receipt" + (": " + refusal if refusal else ""))
    else:
        total = to_int(receipt.get("total"))
        connected = to_int(receipt.get("connected"))
        row["clients"] = receipt.get("total", "")
        row["connected"] = receipt.get("connected", "")
        row["unexpected_disconnects"] = receipt.get("unexpected_disconnects", "")
        row["c2s_sent"] = receipt.get("c2s_accepted", "")
        row["c2s_received"] = receipt.get("c2s_received", "")
        row["s2c_sent"] = receipt.get("s2c_accepted", "")
        row["s2c_received"] = receipt.get("s2c_received", "")
        if total is None or connected is None or connected != total:
            reasons.append(f"connected {receipt.get('connected')} of {receipt.get('total')}")
        if to_int(receipt.get("unexpected_disconnects")) != 0:
            reasons.append(f"unexpected_disconnects={receipt.get('unexpected_disconnects')}")
    if proof is not None:
        row["disconnects"] = proof.get("disconnects", "")
        if to_int(proof.get("disconnects")) != 0:
            reasons.append(f"disconnects={proof.get('disconnects')}")
    elif receipt is not None:
        reasons.append("no server proof line")
    if cell_line is not None:
        row["unreliable_lost"] = cell_line.get("unreliable_lost", "")
        row["sequenced_skipped"] = cell_line.get("sequenced_skipped", "")
        if cell_line.get("verdict") != "PASS":
            reasons.append(f"proof cell {cell_line.get('verdict')}: {cell_line.get('reason', 'see merge.log')}")
    elif receipt is not None:
        reasons.append("no merger proof cell")

    timed = 0
    for call, key in (("server_update", "update"), ("server_send_packets", "send")):
        fields = calls.get(call)
        if fields is None:
            continue
        row[f"{key}_p99_us"] = fields.get("p99_upper_us", "")
        row[f"{key}_p999_us"] = fields.get("p999_upper_us", "")
        row[f"{key}_max_us"] = fields.get("max_us", "")
        if to_int(fields.get("p99_upper_us")) is not None:
            timed += 1
    if receipt is not None and timed != len(TIMED_CALLS):
        reasons.append("server call timing missing")
    row["status"] = "FAIL" if reasons else "PASS"
    row["reason"] = "; ".join(reasons) if reasons else "-"
    row["calls"] = {call: {k: calls[call].get(k) for k in ("count", "p50_upper_us", "p99_upper_us",
                                                         "p999_upper_us", "max_us", "overbudget_count")}
                    for call in CALLS if call in calls}
    return row


def markdown(rows, head):
    lines = [f"Real load, 1000 clients over the LAN, head `{head}`. "
             "Server call times are upper bounds in microseconds.", "",
             "| cell | status | connected | disconnects | c2s sent/received | s2c sent/received "
             "| unreliable lost | update p99 / p999 / max | send p99 / p999 / max | reason |",
             "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
    for row in rows:
        lines.append(
            f"| {row['cell']} | {row['status']} | {row['connected'] or '-'}/{row['clients'] or '-'} "
            f"| {row['disconnects'] or '-'} | {row['c2s_sent'] or '-'}/{row['c2s_received'] or '-'} "
            f"| {row['s2c_sent'] or '-'}/{row['s2c_received'] or '-'} | {row['unreliable_lost'] or '-'} "
            f"| {row['update_p99_us'] or '-'} / {row['update_p999_us'] or '-'} / {row['update_max_us'] or '-'} "
            f"| {row['send_p99_us'] or '-'} / {row['send_p999_us'] or '-'} / {row['send_max_us'] or '-'} "
            f"| {row['reason']} |")
    return "\n".join(lines) + "\n"


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", required=True)
    parser.add_argument("--head", default="unknown")
    parser.add_argument("--date", default="unknown")
    parser.add_argument("cells", nargs="+")
    try:
        args = parser.parse_args(argv)
    except SystemExit:
        return 2
    rows = []
    for item in args.cells:
        if "=" not in item:
            sys.stderr.write(f"real_load_report: {item!r} is not CELL=RUN_DIR\n")
            return 2
        cell, run_dir = item.split("=", 1)
        rows.append(summarise(cell, run_dir))
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "real-load.tsv"), "w", encoding="utf-8") as handle:
        handle.write("date\thead\t" + "\t".join(COLUMNS) + "\n")
        for row in rows:
            handle.write(f"{args.date}\t{args.head}\t" + "\t".join(str(row[key]) for key in COLUMNS) + "\n")
    with open(os.path.join(args.out, "real-load.json"), "w", encoding="utf-8") as handle:
        json.dump({"schema": "real_load_v1", "date": args.date, "head": args.head,
                   "cells": rows}, handle, indent=2)
        handle.write("\n")
    text = markdown(rows, args.head)
    with open(os.path.join(args.out, "real-load.md"), "w", encoding="utf-8") as handle:
        handle.write(text)
    sys.stdout.write(text)
    for row in rows:
        print(f"real_load: cell={row['cell']} status={row['status']} connected={row['connected'] or 'na'} "
              f"disconnects={row['disconnects'] or 'na'} update_p99_us={row['update_p99_us'] or 'na'} "
              f"update_p999_us={row['update_p999_us'] or 'na'} send_p99_us={row['send_p99_us'] or 'na'} "
              f"send_p999_us={row['send_p999_us'] or 'na'} reason={row['reason']}")
    return 1 if any(row["status"] != "PASS" for row in rows) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
