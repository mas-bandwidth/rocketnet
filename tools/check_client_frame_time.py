#!/usr/bin/env python3
"""
Nightly client frame time row for one platform client transport.

Drives benchmarks/client_frame_time (see benchmarks/client_frame_time.md):
starts its server on loopback, runs its client at the space client profile
(15 MTU packets a tick, 60 Hz) and prints one row:

    CLIENT-FRAME-TIME transport=<t> frames=<n> frame_p99_us=<x> frame_max_us=<x> max_us=<x>[ report-only] allocs=<n> verdict=<PASS|FAIL>

The row asserts, every night:
  - zero allocations through the library's allocator hooks inside client
    calls in steady state (always enforced: it does not depend on the machine)
  - the worst client frame is at or under --max-us (with --report-only, on a
    runner that is not reserved quiet, a frame over it is a FAIL verdict that
    exits 0 and prints ::warning title=REPORT-ONLY-FAIL::<verdict line>)

and then proves both assertions can fail (the red proof), with the meter's
plants inside the library's own calls:
  - a 500 us busy loop inside update must fail the worst-frame assertion (exit 3)
  - one allocation inside send each frame must fail the zero-allocation assertion (exit 2)

A red proof that passes is a FAIL: the row would no longer catch what it says.
"""

import argparse
import os
import re
import subprocess
import sys
import time
from typing import List, Optional, Tuple

FRAME_RE = re.compile(r"^cft call=frame frames=(\d+) p50_us=([0-9.]+) p99_us=([0-9.]+) p999_us=([0-9.]+) max_us=([0-9.]+)", re.M)
ALLOCS_RE = re.compile(r"allocs_in_calls=(\d+)")
TRANSPORT_RE = re.compile(r"^cft transport=(\S+)", re.M)

EXIT_ALLOC = 2
EXIT_MAX_US = 3


def run_pair(binary: str, port: int, token: str, client_args: List[str], seconds: int) -> Tuple[int, str]:
    if os.path.exists(token):
        os.remove(token)
    server = subprocess.Popen(
        [binary, "server", "--bind", "127.0.0.1:%d" % port, "--token-file", token, "--seconds", str(seconds)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        deadline = time.monotonic() + 30.0
        while not os.path.exists(token):
            if server.poll() is not None or time.monotonic() > deadline:
                return 1, "server did not write its token (exit %s)" % server.poll()
            time.sleep(0.05)
        client = subprocess.run([binary, "client", "--token-file", token] + client_args, capture_output=True, text=True)
        return client.returncode, client.stdout + client.stderr
    finally:
        server.wait(timeout=seconds + 30)


def parse(output: str) -> Tuple[Optional[str], Optional[float], Optional[float], Optional[int], Optional[int]]:
    transport = TRANSPORT_RE.search(output)
    frame = FRAME_RE.search(output)
    allocs = ALLOCS_RE.search(output)
    return (
        transport.group(1) if transport else None,
        float(frame.group(3)) if frame else None,
        float(frame.group(5)) if frame else None,
        int(frame.group(1)) if frame else None,
        int(allocs.group(1)) if allocs else None,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bin", required=True, help="path to client_frame_time")
    parser.add_argument("--max-us", type=float, default=400.0, help="worst client frame allowed, us")
    parser.add_argument("--frames", type=int, default=3600)
    parser.add_argument("--warmup", type=int, default=600)
    parser.add_argument("--port", type=int, default=40600)
    parser.add_argument("--report-only", action="store_true", help="record the worst frame without enforcing --max-us")
    args = parser.parse_args()

    token = os.path.abspath("client_frame_time.token")
    seconds = int((args.frames + args.warmup) / 60) + 120
    failures = []

    # the row

    row_args = ["--frames", str(args.frames), "--warmup", str(args.warmup), "--condition", "nightly-clean", "--assert-zero-alloc", "1"]
    if not args.report_only:
        row_args += ["--assert-max-us", "%.1f" % args.max_us]
    code, output = run_pair(args.bin, args.port, token, row_args, seconds)
    sys.stdout.write(output)
    transport, p99, worst, frames, allocs = parse(output)
    if code == EXIT_ALLOC:
        failures.append("%s allocations inside client calls in steady state" % allocs)
    elif code == EXIT_MAX_US:
        failures.append("worst client frame %.2f us is over %.1f us" % (worst or -1.0, args.max_us))
    elif code != 0 or worst is None:
        failures.append("client exit %d" % code)

    # the red proofs: each plant must fail its assertion

    proofs = [
        ("busy-update", ["--plant", "busy", "--plant-call", "update", "--plant-us", "500", "--assert-max-us", "400"], EXIT_MAX_US),
        ("alloc-send", ["--plant", "alloc", "--plant-call", "send", "--assert-zero-alloc", "1"], EXIT_ALLOC),
    ]
    for index, (name, plant_args, expected) in enumerate(proofs):
        proof_args = ["--frames", "600", "--warmup", "300", "--condition", "red-proof-" + name] + plant_args
        code, proof_output = run_pair(args.bin, args.port + 1 + index, token, proof_args, 120)
        status = "RED" if code == expected else "MISSED"
        print("CLIENT-FRAME-TIME-PROOF plant=%s exit=%d expected=%d %s" % (name, code, expected, status))
        if code != expected:
            sys.stdout.write(proof_output)
            failures.append("red proof %s exited %d, not %d" % (name, code, expected))

    # report-only: the worst frame is compared and printed, and over --max-us
    # is a FAIL verdict that exits 0 with the REPORT-ONLY-FAIL annotation the
    # nightly watch reads (tools/check_nightly_dispositions.py); allocations
    # and the red proofs still fail the job

    report_fail = args.report_only and not failures and worst is not None and worst > args.max_us
    verdict = "FAIL" if failures or report_fail else "PASS"
    line = "CLIENT-FRAME-TIME transport=%s frames=%s frame_p99_us=%s frame_max_us=%s max_us=%.1f%s allocs=%s verdict=%s" % (
        transport,
        frames,
        "%.2f" % p99 if p99 is not None else "na",
        "%.2f" % worst if worst is not None else "na",
        args.max_us,
        " report-only" if args.report_only else "",
        allocs,
        verdict,
    )
    print(line)
    for failure in failures:
        print("FAIL: " + failure)
    if report_fail:
        print("::warning title=REPORT-ONLY-FAIL::" + line)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
