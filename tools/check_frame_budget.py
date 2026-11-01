#!/usr/bin/env python3
"""
Nightly client per-frame main-thread budget gate for RocketNet.

Runs ./bin/client_main_thread_cost <transport> 32 2000 three times, checking the
1-minute load average before each run, parses the client_main_thread_cost
output line for total_us_p99=<float>, takes the median across the 3 runs, and
gates total_us_p99 <= 100.0 us.

Before each of the three runs, reads the 1-minute load average (os.getloadavg
where available; on Windows prints load=unavailable and does not void). If
load > cores / 4.0, prints "VOID load=<x> cores=<n>" and a VOID verdict line
and exits 3 (unless --report-only is set, which exits 0). A void is never a
pass.
"""

import argparse
import os
import re
import statistics
import subprocess
import sys
from typing import Callable, List, Optional, Tuple

from nightly_timeout import execution_timeout

GATE_US = 100.0
DEFAULT_RUNS = 3
PACKETS_PER_FRAME = 32
FRAMES = 2000

# Matches the real client_main_thread_cost output line:
#   client_main_thread_cost transport=<d> packets_per_frame=32 frames=2000 ... total_us_p99=<float> ...
# Also accepts synthetic P=32 lines of the form:
#   ... P=32 ... total_us p99=<float> ... or total_us_p99=<float>
LINE_RE = re.compile(
    r"(?:packets_per_frame=32\b|\bP\s*=\s*32\b).*?\btotal_us(?:_p99|\s+.*?p99)=([0-9]+(?:\.[0-9]+)?)"
)


def get_host_load_and_cores() -> Tuple[Optional[float], int]:
    cores = os.cpu_count() or 1
    if not hasattr(os, "getloadavg"):
        return None, cores
    try:
        load1 = os.getloadavg()[0]
    except OSError:
        return None, cores
    return load1, cores


def is_quiet_host(load1: Optional[float], cores: int) -> bool:
    if cores <= 0:
        return False
    if load1 is None:
        return True
    return load1 <= (cores / 4.0)


def parse_p32_total_p99(stdout: str) -> Tuple[float, str]:
    """
    Extract (total_us_p99, raw_line) for the P=32 / packets_per_frame=32 line
    from client_main_thread_cost output.
    Raises ValueError if missing or malformed.
    """
    if not stdout:
        raise ValueError("empty stdout from client_main_thread_cost")
    for line in stdout.splitlines():
        stripped = line.strip()
        if "packets_per_frame=32" in stripped or re.search(r"\bP\s*=\s*32\b", stripped):
            m = LINE_RE.search(stripped)
            if not m:
                raise ValueError(f"malformed P=32 line (missing total_us_p99): {stripped!r}")
            val = float(m.group(1))
            if val <= 0.0 or val != val:
                raise ValueError(f"invalid total_us_p99 value in P=32 line: {stripped!r}")
            return val, stripped
    raise ValueError("missing packets_per_frame=32 (P=32) line in client_main_thread_cost output")


def evaluate_gate(
    transport: str,
    run_outputs: List[str],
    load1: Optional[float],
    cores: int,
    gate_us: float = GATE_US,
    report_only: bool = False,
    load_samples: Optional[List[Tuple[Optional[float], int]]] = None,
) -> Tuple[int, str]:
    """
    Evaluate the frame-budget gate from pre-collected or synthetic inputs.
    Returns (exit_code, formatted_report).
    Exit codes:
      0: PASS (or --report-only on PASS/FAIL/VOID)
      1: FAIL (median > gate_us, or missing/malformed output line)
      3: VOID (host load > cores / 4)
    """
    lines_out: List[str] = []
    samples: List[float] = []
    raw_lines: List[str] = []

    if load_samples is None:
        count = max(len(run_outputs), 1)
        load_samples = [(load1, cores)] * count

    for idx, (cur_load, cur_cores) in enumerate(load_samples):
        if cur_load is None:
            lines_out.append("load=unavailable")
        elif not is_quiet_host(cur_load, cur_cores):
            lines_out.append(f"VOID load={cur_load:.2f} cores={cur_cores}")
            lines_out.append(
                f"frame_budget transport={transport} P=32 total_us_p99_median=VOID "
                f"runs=- gate={gate_us:g} verdict=VOID"
            )
            return (0 if report_only else 3), "\n".join(lines_out)

        if idx >= len(run_outputs):
            lines_out.append(
                f"frame_budget transport={transport} P=32 total_us_p99_median=FAIL "
                f"runs=- gate={gate_us:g} verdict=FAIL (no runs collected)"
            )
            return 1, "\n".join(lines_out)

        out = run_outputs[idx]
        try:
            val, raw_line = parse_p32_total_p99(out)
        except ValueError as exc:
            lines_out.extend(raw_lines)
            lines_out.append(
                f"frame_budget transport={transport} P=32 total_us_p99_median=FAIL "
                f"runs=- gate={gate_us:g} verdict=FAIL ({exc})"
            )
            return 1, "\n".join(lines_out)
        raw_lines.append(raw_line)
        samples.append(val)

    if not samples:
        lines_out.append(
            f"frame_budget transport={transport} P=32 total_us_p99_median=FAIL "
            f"runs=- gate={gate_us:g} verdict=FAIL (no runs collected)"
        )
        return 1, "\n".join(lines_out)

    lines_out.extend(raw_lines)
    median_us = float(statistics.median(samples))
    runs_str = ",".join(f"{v:.2f}" for v in samples)
    passed = median_us <= gate_us
    verdict = "PASS" if passed else "FAIL"
    lines_out.append(
        f"frame_budget transport={transport} P=32 total_us_p99_median={median_us:.2f} "
        f"runs={runs_str} gate={gate_us:g} verdict={verdict}"
    )
    if passed or report_only:
        return 0, "\n".join(lines_out)
    return 1, "\n".join(lines_out)


def run_benchmark_with_load_checks(
    binary: str,
    transport: str,
    runs: int = DEFAULT_RUNS,
    gate_us: float = GATE_US,
    report_only: bool = False,
    load_fn: Callable[[], Tuple[Optional[float], int]] = get_host_load_and_cores,
    run_fn: Optional[Callable[[str, str], Tuple[int, str, str]]] = None,
) -> Tuple[int, str]:
    def default_runner(bin_path: str, drv: str) -> Tuple[int, str, str]:
        proc = subprocess.run(
            [bin_path, drv, str(PACKETS_PER_FRAME), str(FRAMES)],
            capture_output=True,
            text=True,
            timeout=execution_timeout(300),
        )
        return proc.returncode, proc.stdout, proc.stderr

    runner = run_fn or default_runner
    lines_out: List[str] = []
    raw_lines: List[str] = []
    samples: List[float] = []

    for i in range(runs):
        load1, cores = load_fn()
        if load1 is None:
            lines_out.append("load=unavailable")
        elif not is_quiet_host(load1, cores):
            lines_out.append(f"VOID load={load1:.2f} cores={cores}")
            lines_out.append(
                f"frame_budget transport={transport} P=32 total_us_p99_median=VOID "
                f"runs=- gate={gate_us:g} verdict=VOID"
            )
            return (0 if report_only else 3), "\n".join(lines_out)

        rc, stdout, stderr = runner(binary, transport)
        if rc != 0:
            lines_out.extend(raw_lines)
            err_msg = (stderr or stdout or "").strip()
            lines_out.append(
                f"frame_budget transport={transport} P=32 total_us_p99_median=FAIL "
                f"runs=- gate={gate_us:g} verdict=FAIL (run {i + 1}/{runs} exited {rc}: {err_msg})"
            )
            return 1, "\n".join(lines_out)

        try:
            val, raw_line = parse_p32_total_p99(stdout)
        except ValueError as exc:
            lines_out.extend(raw_lines)
            lines_out.append(
                f"frame_budget transport={transport} P=32 total_us_p99_median=FAIL "
                f"runs=- gate={gate_us:g} verdict=FAIL ({exc})"
            )
            return 1, "\n".join(lines_out)

        raw_lines.append(raw_line)
        samples.append(val)

    lines_out.extend(raw_lines)
    median_us = float(statistics.median(samples))
    runs_str = ",".join(f"{v:.2f}" for v in samples)
    passed = median_us <= gate_us
    verdict = "PASS" if passed else "FAIL"
    lines_out.append(
        f"frame_budget transport={transport} P=32 total_us_p99_median={median_us:.2f} "
        f"runs={runs_str} gate={gate_us:g} verdict={verdict}"
    )
    if passed or report_only:
        return 0, "\n".join(lines_out)
    return 1, "\n".join(lines_out)


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Check client per-frame main-thread cost budget.")
    parser.add_argument("pos_bin", nargs="?", help="Optional positional path to client_main_thread_cost binary.")
    parser.add_argument("pos_transport", nargs="?", help="Optional positional transport name.")
    parser.add_argument(
        "--bin",
        default=None,
        help="Path to client_main_thread_cost binary (default: ./bin/client_main_thread_cost).",
    )
    parser.add_argument(
        "--transport",
        default=None,
        help="Transport name (default: portable).",
    )
    parser.add_argument(
        "--runs",
        type=int,
        default=DEFAULT_RUNS,
        help="Number of benchmark runs to take median over (default: 3).",
    )
    parser.add_argument(
        "--gate-us",
        type=float,
        default=GATE_US,
        help="Maximum allowed P=32 total_us p99 median in microseconds (default: 100.0).",
    )
    parser.add_argument(
        "--report-only",
        action="store_true",
        help="Print results and exit 0 on PASS, FAIL, or VOID (for non-quiet or report-only legs).",
    )
    args = parser.parse_args(argv)

    binary = args.bin or args.pos_bin or "./bin/client_main_thread_cost"
    transport = args.transport or args.pos_transport or "portable"

    code, report = run_benchmark_with_load_checks(
        binary=binary,
        transport=transport,
        runs=args.runs,
        gate_us=args.gate_us,
        report_only=args.report_only,
    )
    print(report)
    return code


if __name__ == "__main__":
    sys.exit(main())
