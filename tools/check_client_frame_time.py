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

A loaded host cannot time a frame. Before the row the 1-minute load average is
polled every 30 s until it is at or under cores / 4 (the bound of
tools/check_frame_budget.py), for up to --quiet-wait-s. When the host is still
loaded before the row, or is loaded after it, the worst frame is not judged:
the verdict is VOID (never PASS), it prints
::warning title=UNMEASURED::<why> for the nightly watch, and it exits 0 when
nothing else failed. The allocation assertion and the red proofs are enforced
on a loaded host too. Where the platform has no load average (Windows) the row
never voids.

Exit: 0 PASS (or a report-only FAIL of the worst frame, or a VOID), 1 FAIL, 2 refused
(--bin is not an executable file). --self-test checks the verdicts on
synthetic client output (tools/test_check_client_frame_time.py).
"""

import argparse
import os
import re
import subprocess
import sys
import time
from typing import Callable, List, NamedTuple, Optional, Tuple

FRAME_RE = re.compile(r"^cft call=frame frames=(\d+) p50_us=([0-9.]+) p99_us=([0-9.]+) p999_us=([0-9.]+) max_us=([0-9.]+)", re.M)
ALLOCS_RE = re.compile(r"allocs_in_calls=(\d+)")
TRANSPORT_RE = re.compile(r"^cft transport=(\S+)", re.M)

EXIT_ALLOC = 2
EXIT_MAX_US = 3

PROOFS = [
    ("busy-update", ["--plant", "busy", "--plant-call", "update", "--plant-us", "500", "--assert-max-us", "400"], EXIT_MAX_US),
    ("alloc-send", ["--plant", "alloc", "--plant-call", "send", "--assert-zero-alloc", "1"], EXIT_ALLOC),
]


class Row(NamedTuple):
    failures: List[str]
    report_fail: bool
    line: str


def host_load_and_cores() -> Tuple[Optional[float], int]:
    """The 1-minute load average (None where the platform has none) and the core count."""
    cores = os.cpu_count() or 1
    if not hasattr(os, "getloadavg"):
        return None, cores
    try:
        return os.getloadavg()[0], cores
    except OSError:
        return None, cores


def is_loaded(load1: Optional[float], cores: int) -> bool:
    """A host whose 1-minute load is over cores / 4 cannot time a frame (the same
    bound as tools/check_frame_budget.py); an unknown load never voids."""
    return load1 is not None and load1 > cores / 4.0


def wait_for_quiet(wait_s: float, poll_s: float, load_fn: Callable[[], Tuple[Optional[float], int]],
                   sleep_fn: Callable[[float], None]) -> Tuple[Optional[float], int, float]:
    """Polls the load every poll_s until the host is quiet or wait_s has passed.
    Returns (load1, cores, seconds waited) at the last reading."""
    waited = 0.0
    load1, cores = load_fn()
    while is_loaded(load1, cores) and waited + poll_s <= wait_s:
        sleep_fn(poll_s)
        waited += poll_s
        load1, cores = load_fn()
    return load1, cores, waited


def void_reason(before: Tuple[Optional[float], int], after: Tuple[Optional[float], int]) -> Optional[str]:
    """Why the worst frame cannot be judged, or None when the host was quiet before and after the row."""
    for when, (load1, cores) in (("before", before), ("after", after)):
        if is_loaded(load1, cores):
            return "host load %.2f over %g (%d cores / 4) %s the row" % (load1, cores / 4.0, cores, when)
    return None


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


def judge_row(code: int, output: str, max_us: float, report_only: bool, void: Optional[str] = None) -> Row:
    """The row's verdict from the client's exit code and output. With
    report_only the client ran without --assert-max-us, so the worst frame is
    compared here. With void (the host was loaded before or after the row) the
    worst frame is not judged: a refusal of it is not a failure and the verdict
    is VOID, never PASS; the allocation assertion still holds."""
    transport, p99, worst, frames, allocs = parse(output)
    failures = []
    if void is not None and code == EXIT_MAX_US:
        code = 0
    if code == EXIT_ALLOC:
        failures.append("%s allocations inside client calls in steady state" % allocs)
    elif code == EXIT_MAX_US:
        failures.append("worst client frame %.2f us is over %.1f us" % (worst if worst is not None else -1.0, max_us))
    elif code != 0:
        failures.append("client exit %d" % code)
    elif worst is None or allocs is None:
        failures.append("client printed no frame or allocation line")
    elif allocs != 0:
        failures.append("%d allocations inside client calls in steady state (the client did not refuse them)" % allocs)
    elif void is None and not report_only and worst > max_us:
        failures.append("worst client frame %.2f us is over %.1f us (the client did not refuse it)" % (worst, max_us))
    report_fail = void is None and report_only and not failures and worst is not None and worst > max_us
    line = "CLIENT-FRAME-TIME transport=%s frames=%s frame_p99_us=%s frame_max_us=%s max_us=%.1f%s allocs=%s verdict=%s" % (
        transport,
        frames,
        "%.2f" % p99 if p99 is not None else "na",
        "%.2f" % worst if worst is not None else "na",
        max_us,
        " report-only" if report_only else "",
        allocs,
        "FAIL" if failures or report_fail else ("VOID" if void is not None else "PASS"),
    )
    return Row(failures, report_fail, line)


def judge_proof(name: str, code: int, expected: int) -> Optional[str]:
    """None when the plant failed its assertion as it must; otherwise the failure."""
    if code == expected:
        return None
    return "red proof %s exited %d, not %d" % (name, code, expected)


def self_test() -> int:
    clean = "cft transport=gso condition=c\ncft call=frame frames=3600 p50_us=26.03 p99_us=46.97 p999_us=51.69 max_us=57.85\n" "cft messages_per_frame=16.00 allocs_in_calls=0 allocs_elsewhere=0\n"
    slow = clean.replace("max_us=57.85", "max_us=512.50")
    leaky = clean.replace("allocs_in_calls=0", "allocs_in_calls=600")
    cases = [
        ("clean, enforced", judge_row(0, clean, 400.0, False), [], False),
        ("clean, report-only", judge_row(0, clean, 400.0, True), [], False),
        ("assert-max-us refused it", judge_row(EXIT_MAX_US, slow, 400.0, False), ["worst client frame 512.50 us is over 400.0 us"], False),
        ("assert-zero-alloc refused it", judge_row(EXIT_ALLOC, leaky, 400.0, True), ["600 allocations inside client calls in steady state"], False),
        ("slow, report-only: FAIL verdict, exit 0", judge_row(0, slow, 400.0, True), [], True),
        ("slow but the client passed it", judge_row(0, slow, 400.0, False), ["worst client frame 512.50 us is over 400.0 us (the client did not refuse it)"], False),
        ("allocations the client passed", judge_row(0, leaky, 400.0, True), ["600 allocations inside client calls in steady state (the client did not refuse them)"], False),
        ("no output", judge_row(0, "", 400.0, False), ["client printed no frame or allocation line"], False),
        ("client crashed", judge_row(-11, clean, 400.0, False), ["client exit -11"], False),
        ("loaded, the client refused the frame: VOID", judge_row(EXIT_MAX_US, slow, 400.0, False, "load"), [], False),
        ("loaded, slow, enforced: VOID", judge_row(0, slow, 400.0, False, "load"), [], False),
        ("loaded, slow, report-only: VOID, no report fail", judge_row(0, slow, 400.0, True, "load"), [], False),
        ("loaded, clean: VOID, never PASS", judge_row(0, clean, 400.0, False, "load"), [], False),
        ("loaded, allocations refused: FAIL", judge_row(EXIT_ALLOC, leaky, 400.0, False, "load"), ["600 allocations inside client calls in steady state"], False),
        ("loaded, allocations passed: FAIL", judge_row(0, leaky, 400.0, False, "load"), ["600 allocations inside client calls in steady state (the client did not refuse them)"], False),
        ("loaded, client crashed: FAIL", judge_row(-11, clean, 400.0, False, "load"), ["client exit -11"], False),
    ]
    failed = 0
    for name, row, want_failures, want_report_fail in cases:
        voided = name.startswith("loaded") and not want_failures
        verdict_ok = ("verdict=FAIL" in row.line) == bool(want_failures or want_report_fail) and ("verdict=VOID" in row.line) == voided
        if row.failures != want_failures or row.report_fail != want_report_fail or not verdict_ok:
            print("SELF-TEST FAIL %s: failures=%s report_fail=%s line=%s" % (name, row.failures, row.report_fail, row.line))
            failed += 1
    loads = iter([(101.6, 32), (40.0, 32), (8.0, 32), (0.1, 32)])
    slept: List[float] = []
    quiet = wait_for_quiet(1200.0, 30.0, lambda: next(loads), slept.append)
    stuck = wait_for_quiet(90.0, 30.0, lambda: (101.6, 32), lambda s: None)
    unknown = wait_for_quiet(1200.0, 30.0, lambda: (None, 8), lambda s: None)
    load_cases = [
        ("waits until load reaches cores/4", quiet, (8.0, 32, 60.0)),
        ("waited 30 s twice", slept, [30.0, 30.0]),
        ("gives up after the wait, still loaded", stuck, (101.6, 32, 90.0)),
        ("no load average never waits", unknown, (None, 8, 0.0)),
        ("quiet before and after", void_reason((8.0, 32), (0.1, 32)), None),
        ("loaded before", void_reason((101.6, 32), (0.1, 32)), "host load 101.60 over 8 (32 cores / 4) before the row"),
        ("loaded after", void_reason((0.1, 32), (8.5, 32)), "host load 8.50 over 8 (32 cores / 4) after the row"),
        ("no load average never voids", void_reason((None, 8), (None, 8)), None),
    ]
    for name, got, want in load_cases:
        if got != want:
            print("SELF-TEST FAIL %s: got %s want %s" % (name, got, want))
            failed += 1
    proof_cases = [
        ("busy plant failed its assertion", judge_proof("busy-update", EXIT_MAX_US, EXIT_MAX_US), None),
        ("busy plant passed", judge_proof("busy-update", 0, EXIT_MAX_US), "red proof busy-update exited 0, not 3"),
        ("alloc plant failed the wrong assertion", judge_proof("alloc-send", EXIT_MAX_US, EXIT_ALLOC), "red proof alloc-send exited 3, not 2"),
    ]
    for name, got, want in proof_cases:
        if got != want:
            print("SELF-TEST FAIL %s: got %s want %s" % (name, got, want))
            failed += 1
    total = len(cases) + len(load_cases) + len(proof_cases)
    if failed:
        print("check_client_frame_time self-test: %d of %d cases failed" % (failed, total))
        return 1
    print("check_client_frame_time self-test: %d cases passed" % total)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bin", help="path to client_frame_time")
    parser.add_argument("--max-us", type=float, default=400.0, help="worst client frame allowed, us")
    parser.add_argument("--frames", type=int, default=3600)
    parser.add_argument("--warmup", type=int, default=600)
    parser.add_argument("--port", type=int, default=40600)
    parser.add_argument("--report-only", action="store_true", help="a worst frame over --max-us is a FAIL verdict that exits 0")
    parser.add_argument("--quiet-wait-s", type=float, default=1200.0,
                        help="how long to wait for the host load to fall to cores/4 before the row (default 1200 s)")
    parser.add_argument("--self-test", action="store_true", help="check the verdicts on synthetic output and exit")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not args.bin or not os.path.isfile(args.bin) or not os.access(args.bin, os.X_OK):
        print("REFUSED check_client_frame_time: --bin %s is not an executable file" % args.bin)
        return 2

    token = os.path.abspath("client_frame_time.token")
    seconds = int((args.frames + args.warmup) / 60) + 120

    # the row

    # a loaded host cannot time a frame: wait for it to go quiet, and judge the
    # worst frame only when it was quiet before and after the row
    load1, cores, waited = wait_for_quiet(args.quiet_wait_s, 30.0, host_load_and_cores, time.sleep)
    print("CLIENT-FRAME-TIME-LOAD before load=%s cores=%d waited_s=%g" % ("%.2f" % load1 if load1 is not None else "unavailable", cores, waited))
    before = (load1, cores)

    row_args = ["--frames", str(args.frames), "--warmup", str(args.warmup), "--condition", "nightly-clean", "--assert-zero-alloc", "1"]
    if not args.report_only and not is_loaded(*before):
        row_args += ["--assert-max-us", "%.1f" % args.max_us]
    code, output = run_pair(args.bin, args.port, token, row_args, seconds)
    sys.stdout.write(output)
    after = host_load_and_cores()
    print("CLIENT-FRAME-TIME-LOAD after load=%s cores=%d" % ("%.2f" % after[0] if after[0] is not None else "unavailable", after[1]))
    void = void_reason(before, after)
    row = judge_row(code, output, args.max_us, args.report_only, void)
    failures = list(row.failures)

    # the red proofs: each plant must fail its assertion

    for index, (name, plant_args, expected) in enumerate(PROOFS):
        proof_args = ["--frames", "600", "--warmup", "300", "--condition", "red-proof-" + name] + plant_args
        code, proof_output = run_pair(args.bin, args.port + 1 + index, token, proof_args, 120)
        failure = judge_proof(name, code, expected)
        print("CLIENT-FRAME-TIME-PROOF plant=%s exit=%d expected=%d %s" % (name, code, expected, "MISSED" if failure else "RED"))
        if failure:
            sys.stdout.write(proof_output)
            failures.append(failure)

    line = row.line if not failures or "verdict=FAIL" in row.line else row.line.replace("verdict=PASS", "verdict=FAIL").replace("verdict=VOID", "verdict=FAIL")
    print(line)
    for failure in failures:
        print("FAIL: " + failure)
    if void is not None and not failures:
        # the worst frame was not judged: a void is never a pass. The job declares
        # ROCKETNET_MAY_NOT_MEASURE, and the nightly watch reads this annotation
        # (tools/check_nightly_dispositions.py, UNMEASURED; NO-MEASUREMENT after 7 nights)
        print("::warning title=UNMEASURED::client frame time VOID: %s; the worst frame (%s us) is not judged; "
              "allocations and the red proofs were" % (void, line.split("frame_max_us=")[1].split()[0]))
    if row.report_fail and not failures:
        # report-only: exit 0 on the FAIL verdict; the nightly watch reads this annotation
        # (tools/check_nightly_dispositions.py) and lists the night red
        print("::warning title=REPORT-ONLY-FAIL::" + line)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
