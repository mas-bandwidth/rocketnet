#!/usr/bin/env python3
"""soak_matrix.py: the macOS soak matrix, one soak per server transport, impaired and clean.

usage:
  soak_matrix.py [--soak ./bin/soak] [--seconds S] [--interval N] [--out DIR]
                 [--cell NAME ...] [--wait-quiet SECONDS]
  soak_matrix.py --self-test   the verdict rules on fixture logs (no soak run)

The two scripts:
  tools/macos/resource_watch.py  samples one pid (fds, threads, rss and
      phys_footprint, by ctypes over libproc) into a CSV and judges the series
      flat or red after a 10 percent warm-up; `--planted-leak-check` is its red
      proof on a child that leaks one fd per second.
  tools/macos/soak_matrix.py     (this script) runs `soak --seconds S` once per
      cell under the watcher and under `leaks --atExit` with
      MallocStackLogging=1, and prints one table row per run.

The four cells. Each server transport is paired with the matching client
transport (ROCKETNET_SERVER_TRANSPORT and ROCKETNET_CLIENT_TRANSPORT):
  portable-impaired       portable-clean        server portable, client portable
  msgx-impaired           msgx-clean            server msgx, client msgx
A cell is judged on the transports soak reports it was granted (its summary
lines server_transport_io and client_transport_io), never on what it asked
for: a cell granted anything else fails. (There is no cell for msgx's
degraded rung: nothing the library reads from the environment forces an msgx
refusal, so such a cell would only rerun the ladder's msgx grant.)
The impaired cells run first, then the clean ones.
An impaired cell turns the link simulator on at both ends
(ROCKETNET_DEV_SERVER_* and ROCKETNET_DEV_CLIENT_*, which soak writes into the
transport configs it creates from; its "soak link simulator:" lines show what
each end was created with): loss 5 percent, latency 30 ms, jitter 20 ms,
duplicate 2 percent. A clean cell leaves it off; either way the cell fails
unless soak's "soak link simulator:" lines show those values (or zero) at
both ends. --cell picks cells by name (repeatable); the default is all four,
one at a time. --wait-quiet waits before each run until no other functional,
load or soak suite runs on the machine (up to that many seconds). The watcher
samples until soak prints its "soak finished:" line; leaks then scans the
process held at exit, and a leaks that has not reported within --leaks-timeout
seconds (default 600) is stopped and its column reads "not run". With
ROCKETNET_NIGHTLY_NO_TIMEOUT=1, child discovery, sampling and the leaks wait
have no elapsed cap; sampling still ends at the soak verdict or process exit.

Each run writes into --out (default soak-matrix): <cell>.log (soak's output,
then the leaks report), <cell>.csv (the watcher samples) and <cell>.verdict.

How to read the verdicts. A row passes only when every column passes:
  soak     soak's own verdict: its "soak finished:" line with 0 failures. soak
           counts every payload verify failure, every counter conservation
           failure, every disconnect it did not plan and every REFUSAL
           counter it does not excuse (below), and names each.
  transport  the granted transports (server_transport_io, client_transport_io)
           are the cell's, and the link simulator lines match the cell.
  verify   the "verified" summary equals the "received" summary, and no
           "PAYLOAD VERIFY FAILED" line.
  unplanned  disconnects other than soak's own planned reconnect cycle: 0.
  conserve "soak: conservation ok (server and every client)": every counter
           family balances, RECEIVED equals the sum of its outcomes, on the
           server and on every client.
  watch    the watcher's verdict: FLAT, or RED naming the series (fds or
           threads with a slope past 2 per hour, rss or phys_footprint
           growing more than 1 MiB per hour).
  leaks    the count from the leaks line "Process N: L leaks for B total
           leaked bytes": 0. "not run" is a fail: leaks needs the task port
           of the process, which macOS grants only with developer tools
           access enabled for the user running it.
Refusals are part of the soak column: soak fails a run on any REFUSAL
counter, except the <FAMILY>_ALREADY_RECEIVED refusals of the duplicates an
impaired cell plants (tests/soak/soak.c, its header's paragraph on
refusals); soak prints those as "planted duplicates refused at ...". The
server socket is bound on 127.0.0.1 (unless SOAK_SERVER_ADDRESS names
another), so another suite on the same machine can still send to its port,
and such a packet is refused and counted too: a server WRONG_DIRECTION
refusal is a CONNECTION_COOKIE, CHALLENGE or DENIED packet arriving at a
server, which only ever sends them (the "wrong direction" drop in
sdk/rocketnet_server_protocol.cpp). Read the sender before calling it a
defect; rerun the cell on a quiet machine.
The script exits 0 when every row passes and 1 otherwise.
"""

import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import resource_watch  # noqa: E402
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from nightly_timeout import execution_timeout, no_execution_timeout  # noqa: E402

IMPAIRMENT = {
    "PACKET_LOSS_PERCENT": "5",
    "LATENCY_MS": "30",
    "JITTER_MS": "20",
    "DUPLICATE_PERCENT": "2",
}

# (cell label, server transport, client transport): each is a pin, and the
# run must report exactly that grant at both ends
TRANSPORTS = [
    ("portable", "portable", "portable"),
    ("msgx", "msgx", "msgx"),
]

CELLS = []
for _impaired in (True, False):
    for _label, _server, _client in TRANSPORTS:
        CELLS.append(("%s-%s" % (_label, "impaired" if _impaired else "clean"), _server, _client, _impaired))

# soak's "soak link simulator: <end> ..." fields, in the IMPAIRMENT order
LINK_FIELDS = (("latency_ms", "LATENCY_MS"), ("jitter_ms", "JITTER_MS"),
               ("loss_percent", "PACKET_LOSS_PERCENT"), ("duplicate_percent", "DUPLICATE_PERCENT"))

SUITES = ("functional", "load", "soak", "soak_sim", "transport_memory", "load_1000", "test_lifecycle")


def other_suites(own):
    out = subprocess.run(["ps", "-axo", "pid=,comm="], capture_output=True, text=True).stdout
    found = []
    for line in out.splitlines():
        parts = line.split(None, 1)
        if len(parts) != 2:
            continue
        pid, comm = int(parts[0]), os.path.basename(parts[1].strip())
        if comm in SUITES and pid not in own:
            found.append("%d %s" % (pid, comm))
    return found


def wait_quiet(limit):
    start = time.monotonic()
    shown = -60
    while True:
        busy = other_suites({os.getpid()})
        waited = time.monotonic() - start
        if not busy:
            print("soak_matrix: quiet after %.0fs (%s)" % (waited, os.getloadavg()), flush=True)
            return True
        if waited - shown >= 60:
            print("soak_matrix: waiting on %s" % ", ".join(busy[:6]), flush=True)
            shown = waited
        if waited >= limit:
            print("soak_matrix: still busy after %.0fs" % waited, flush=True)
            return False
        time.sleep(5)


def cell_env(server, client, impaired):
    env = dict(os.environ)
    for key in list(env):
        if key.startswith("ROCKETNET_SERVER_SIMULATED_") or key.startswith("ROCKETNET_CLIENT_SIMULATED_"):
            del env[key]
        for end in ("SERVER", "CLIENT"):
            if key in ["ROCKETNET_DEV_%s_%s" % (end, field) for field in IMPAIRMENT]:
                del env[key]
                break
    env["ROCKETNET_SERVER_TRANSPORT"] = server
    env["ROCKETNET_CLIENT_TRANSPORT"] = client
    if impaired:
        for field, value in IMPAIRMENT.items():
            env["ROCKETNET_DEV_SERVER_" + field] = value
            env["ROCKETNET_DEV_CLIENT_" + field] = value
    env["MallocStackLogging"] = "1"
    return env


def child_pid(parent, deadline):
    while no_execution_timeout() or time.monotonic() < deadline:
        if parent.poll() is not None:
            return None
        out = subprocess.run(["pgrep", "-P", str(parent.pid)], capture_output=True, text=True).stdout.split()
        if out:
            return int(out[0])
        time.sleep(0.05)
    return None


def parse(log_text):
    summary = {}
    for m in re.finditer(r"^soak summary: (\S+) (\d+)$", log_text, re.M):
        summary[m.group(1)] = int(m.group(2))
    finished = re.search(r"^soak finished: (\d+) iterations, (\d+) connects, (\d+) payloads verified, (\d+) failures$", log_text, re.M)
    leaks = re.search(r"^Process \d+: (\d+) leaks? for (\d+) total leaked bytes", log_text, re.M)
    unplanned = sum(v for k, v in summary.items()
                    if k.startswith("ROCKETNET_CLIENT_STATE_") and k != "ROCKETNET_CLIENT_STATE_DISCONNECTED")
    planned_cap = summary.get("reconnects", 0) + 1
    unplanned += max(0, summary.get("ROCKETNET_CLIENT_STATE_DISCONNECTED", 0) - planned_cap)
    unplanned += len(re.findall(r"^soak: client connection failed", log_text, re.M))
    granted = dict(re.findall(r"^soak summary: (server_transport_io|client_transport_io) (\S+)$", log_text, re.M))
    links = {}
    for m in re.finditer(r"^soak link simulator: (server|client) latency_ms=(\d+) jitter_ms=(\d+)"
                         r" loss_percent=(\d+) duplicate_percent=(\d+)$", log_text, re.M):
        links.setdefault(m.group(1), set()).add(tuple(int(v) for v in m.groups()[1:]))
    return {
        "server_io": granted.get("server_transport_io"),
        "client_io": granted.get("client_transport_io"),
        "links": links,
        "summary": summary,
        "failures": int(finished.group(4)) if finished else None,
        "verify_failed": len(re.findall(r"PAYLOAD VERIFY FAILED", log_text)),
        "conserved": "soak: conservation ok (server and every client)" in log_text,
        "unplanned": unplanned,
        "leaks": int(leaks.group(1)) if leaks else None,
        "leaked_bytes": int(leaks.group(2)) if leaks else None,
    }


def cell_problems(result, server, client, impaired):
    """Why a run is not the cell it names: the granted transports and the
    link simulator each end was created with. Empty when it is the cell."""
    problems = []
    if result["server_io"] != server:
        problems.append("server granted %s, not %s" % (result["server_io"] or "nothing reported", server))
    if result["client_io"] != client:
        problems.append("client granted %s, not %s" % (result["client_io"] or "nothing reported", client))
    want = tuple(int(IMPAIRMENT[key]) if impaired else 0 for _, key in LINK_FIELDS)
    for end in ("server", "client"):
        seen = result["links"].get(end)
        if not seen:
            problems.append("no %s link simulator line" % end)
        elif seen != {want}:
            problems.append("%s link simulator %s, not %s" % (end, sorted(seen), want))
    return problems


def run_cell(args, name, server, client, impaired):
    log_path = os.path.join(args.out, name + ".log")
    csv_path = os.path.join(args.out, name + ".csv")
    verdict_path = os.path.join(args.out, name + ".verdict")
    env = cell_env(server, client, impaired)
    command = ["leaks", "--atExit", "--", args.soak, "--seconds", str(args.seconds)]
    print("soak_matrix: %s: %s (server %s, client %s, %s)" % (
        name, " ".join(command), server, client, "impaired" if impaired else "clean"), flush=True)
    started = time.monotonic()

    def soak_finished():
        with open(log_path) as f:
            text = f.read()
        return "\nsoak finished:" in text or "soak: failed to create" in text

    leaks_note = ""
    with open(log_path, "w") as log:
        leaks_proc = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        soak_pid = child_pid(leaks_proc, time.monotonic() + 30)
        # the watch ends when soak prints its verdict: after that the process
        # only exits, and leaks holds it at exit while it scans
        rows = resource_watch.watch(soak_pid, csv_path, args.interval, stop_after=execution_timeout(args.seconds + 300),
                                    until=soak_finished) if soak_pid else []
        elapsed = time.monotonic() - started
        try:
            leaks_rc = leaks_proc.wait(timeout=execution_timeout(args.leaks_timeout))
        except subprocess.TimeoutExpired:
            leaks_note = "leaks did not report within %.0fs of the end of the run (no task port for the process?)" % args.leaks_timeout
            leaks_proc.kill()
            leaks_rc = leaks_proc.wait()
            if soak_pid:
                try:
                    os.kill(soak_pid, 9)
                except ProcessLookupError:
                    pass
    if leaks_note:
        with open(log_path, "a") as f:
            f.write("soak_matrix: %s\n" % leaks_note)

    lines = []

    class Tee:
        def write(self, text):
            lines.append(text)

    watch_verdict, red = resource_watch.judge(rows, out=Tee())
    with open(verdict_path, "w") as f:
        f.write("".join(lines))
    with open(log_path) as f:
        result = parse(f.read())
    s = result["summary"]
    not_the_cell = cell_problems(result, server, client, impaired)
    row = {
        "cell": name,
        "seconds": elapsed,
        "soak": result["failures"] == 0,
        "failures": result["failures"],
        "verify": result["verify_failed"] == 0 and s.get("verified", -1) == s.get("received", -2),
        "verified": s.get("verified"),
        "received": s.get("received"),
        "sent": s.get("sent"),
        "reconnects": s.get("reconnects"),
        "unplanned": result["unplanned"],
        "conserved": result["conserved"],
        "watch": watch_verdict if not red else "RED(%s)" % ",".join(red),
        "watch_lines": "".join(lines),
        "leaks": result["leaks"],
        "leaked_bytes": result["leaked_bytes"],
        "leaks_rc": leaks_rc,
        "leaks_note": leaks_note,
        "samples": len(rows),
        "transport": not not_the_cell,
        "not_the_cell": not_the_cell,
    }
    row["pass"] = (row["soak"] and row["verify"] and row["unplanned"] == 0 and row["conserved"]
                   and row["transport"] and watch_verdict == "FLAT" and row["leaks"] == 0)
    sys.stdout.write("".join("  " + line for line in lines))
    print("soak_matrix: %s: %s in %.0fs: failures %s, sent %s, received %s, verified %s, reconnects %s, unplanned %s, conservation %s, watch %s, leaks %s (%s bytes)" % (
        name, "PASS" if row["pass"] else "FAIL", elapsed, row["failures"], row["sent"], row["received"], row["verified"],
        row["reconnects"], row["unplanned"], "ok" if row["conserved"] else "FAILED", row["watch"], row["leaks"],
        row["leaked_bytes"]), flush=True)
    for problem in not_the_cell:
        print("soak_matrix: %s: NOT THE CELL: %s" % (name, problem), flush=True)
    if leaks_note:
        print("soak_matrix: %s: %s" % (name, leaks_note), flush=True)
    return row


def table(rows):
    print("")
    print("| cell | run s | result | transport | sent | received | verified | reconnects | unplanned | conservation | watcher | leaks |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        print("| %s | %.0f | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            r["cell"], r["seconds"], "PASS" if r["pass"] else "FAIL", "ok" if r["transport"] else "NOT THE CELL",
            r["sent"], r["received"], r["verified"],
            r["reconnects"], r["unplanned"], "ok" if r["conserved"] else "FAILED", r["watch"],
            r["leaks"] if r["leaks"] is not None else "not run"))


def fixture_log(server_io, client_io, link):
    lines = ["soak link simulator: %s latency_ms=%d jitter_ms=%d loss_percent=%d duplicate_percent=%d" % ((end,) + link[end])
             for end in ("server", "client") if end in link]
    lines += ["soak finished: 10 iterations, 1 connects, 100 payloads verified, 0 failures",
              "soak summary: sent 100", "soak summary: received 100", "soak summary: verified 100"]
    if server_io:
        lines.append("soak summary: server_transport_io %s" % server_io)
    if client_io:
        lines.append("soak summary: client_transport_io %s" % client_io)
    return "\n".join(lines) + "\n"


def self_test():
    """The cell rule on fixture logs: a run is its cell only when both granted
    transports and both link simulator settings are the cell's."""
    impaired = tuple(int(IMPAIRMENT[key]) for _, key in LINK_FIELDS)
    clean = (0, 0, 0, 0)
    cases = [
        ("msgx cell granted msgx", ("msgx", "msgx", {"server": impaired, "client": impaired}), ("msgx", "msgx", True), []),
        ("clean portable cell", ("portable", "portable", {"server": clean, "client": clean}), ("portable", "portable", False), []),
        ("msgx cell granted portable", ("portable", "portable", {"server": impaired, "client": impaired}), ("msgx", "msgx", True),
         ["server granted portable, not msgx", "client granted portable, not msgx"]),
        ("no granted transport reported", (None, None, {"server": clean, "client": clean}), ("portable", "portable", False),
         ["server granted nothing reported, not portable", "client granted nothing reported, not portable"]),
        ("impaired cell with the simulator off", ("msgx", "msgx", {"server": clean, "client": impaired}), ("msgx", "msgx", True),
         ["server link simulator [(0, 0, 0, 0)], not (30, 20, 5, 2)"]),
        ("clean cell with no client link line", ("msgx", "msgx", {"server": clean}), ("msgx", "msgx", False),
         ["no client link simulator line"]),
    ]
    failures = 0
    for label, (server_io, client_io, link), (server, client, is_impaired), expected in cases:
        got = cell_problems(parse(fixture_log(server_io, client_io, link)), server, client, is_impaired)
        if got != expected:
            print("FAIL %s: %s, expected %s" % (label, got, expected))
            failures += 1
    if [c[0] for c in CELLS] != ["portable-impaired", "msgx-impaired", "portable-clean", "msgx-clean"]:
        print("FAIL cells: %s" % [c[0] for c in CELLS])
        failures += 1
    if failures:
        print("soak_matrix self-test: %d failed" % failures)
        return 1
    print("soak_matrix self-test: %d cases passed" % (len(cases) + 1))
    return 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    parser = argparse.ArgumentParser(description="the macOS soak matrix under the resource watcher and leaks")
    parser.add_argument("--soak", default="./bin/soak")
    parser.add_argument("--seconds", type=int, default=1800)
    parser.add_argument("--interval", type=float, default=5.0)
    parser.add_argument("--out", default="soak-matrix")
    parser.add_argument("--cell", action="append", default=[])
    parser.add_argument("--wait-quiet", type=float, default=0.0)
    parser.add_argument("--leaks-timeout", type=float, default=600.0)
    args = parser.parse_args(argv)

    names = [c[0] for c in CELLS]
    for cell in args.cell:
        if cell not in names:
            print("soak_matrix: unknown cell %s (cells: %s)" % (cell, ", ".join(names)), file=sys.stderr)
            return 2
    if args.seconds <= 0 or args.interval <= 0:
        print("soak_matrix: --seconds and --interval must be positive", file=sys.stderr)
        return 2
    os.makedirs(args.out, exist_ok=True)

    rows = []
    for name, server, client, impaired in CELLS:
        if args.cell and name not in args.cell:
            continue
        if args.wait_quiet > 0 and not wait_quiet(args.wait_quiet):
            print("soak_matrix: %s: NOT RUN: another suite kept the machine busy" % name, flush=True)
            continue
        rows.append(run_cell(args, name, server, client, impaired))
    table(rows)
    return 0 if rows and all(r["pass"] for r in rows) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
