#!/usr/bin/env python3
"""Capture honest 1000-client Release load timing or macOS CPU samples.

This is evidence collection, not the final per-main-call budget gate. In
particular, current load.c reports only server_update p50/p99. The script
never emits a frame-budget PASS from that one measurement.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import resource
import shutil
import subprocess
import sys
import time

CLIENTS = 1000
MIN_FD_HARD = 2 * CLIENTS + 64
MIN_TOTAL_BYTES = 14 * 1024**3
MIN_AVAILABLE_BYTES = 12 * 1024**3
MIN_DISK_BYTES = 8 * 1024**3
CRITERIA = (
    "connect_within_10s", "no_unexpected_disconnect", "reliable_delivery",
    "unreliable_integrity", "counters_agree",
)


def command(args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, text=True, stderr=subprocess.STDOUT).strip()


def linux_memory():
    fields = {}
    for line in Path("/proc/meminfo").read_text().splitlines():
        name, _, value = line.partition(":")
        if name in ("MemTotal", "MemAvailable"):
            fields[name] = int(value.strip().split()[0]) * 1024
    return fields["MemTotal"], fields["MemAvailable"]


def mac_memory():
    total = int(command(["sysctl", "-n", "hw.memsize"]))
    output = command(["vm_stat"])
    match = re.search(r"page size of (\d+) bytes", output)
    if not match:
        raise ValueError("vm_stat page size unavailable")
    page_size = int(match.group(1))
    pages = {}
    for line in output.splitlines():
        name, _, value = line.partition(":")
        if value:
            pages[name.strip()] = int(value.strip().rstrip("."))
    available = sum(pages.get(name, 0) for name in
                    ("Pages free", "Pages inactive", "Pages speculative")) * page_size
    return total, available


def preflight(out_dir):
    system = platform.system()
    if system == "Linux":
        total, available = linux_memory()
    elif system == "Darwin":
        total, available = mac_memory()
    else:
        raise ValueError(f"capacity guard not implemented for {system}; no run")
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    disk = shutil.disk_usage(out_dir).free
    cores = os.cpu_count() or 1
    load1 = os.getloadavg()[0]
    result = dict(system=system, host=platform.node(), total_memory_bytes=total,
                  available_memory_bytes=available, free_disk_bytes=disk,
                  fd_soft=soft, fd_hard=hard, load1=load1, cores=cores)
    failures = []
    if total < MIN_TOTAL_BYTES:
        failures.append("physical memory below 14 GiB")
    if available < MIN_AVAILABLE_BYTES:
        failures.append("available memory below 12 GiB")
    if disk < MIN_DISK_BYTES:
        failures.append("free output disk below 8 GiB")
    if hard != resource.RLIM_INFINITY and hard < MIN_FD_HARD:
        failures.append(f"hard fd limit below {MIN_FD_HARD}")
    if load1 > cores / 4:
        failures.append("one-minute load exceeds cores/4")
    result["failures"] = failures
    return result


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def parse_load(stdout, server_transport, client_transport):
    summaries = [line for line in stdout.splitlines()
                 if line.startswith("load: connected=") and "verdict=" in line]
    if len(summaries) != 1:
        raise ValueError(f"expected one final load summary, found {len(summaries)}")
    fields = dict(re.findall(r"\b([A-Za-z_][A-Za-z_0-9]*)=([^\s]+)", summaries[0]))
    if fields.get("connected") != "1000" or fields.get("verdict") != "PASS":
        raise ValueError("population or load verdict failed")
    if any(fields.get(name) != "PASS" for name in CRITERIA):
        raise ValueError("one or more load criteria failed")
    if fields.get("server_transport_io") != server_transport or fields.get("client_transport_io") != client_transport:
        raise ValueError("granted transport differs from requested transport")
    for name in ("fds", "threads"):
        if fields.get(f"{name}_start") != fields.get(f"{name}_end"):
            raise ValueError(f"{name} did not return to baseline")
    try:
        server_p99 = float(fields["server_update_p99_us"])
    except (KeyError, ValueError) as error:
        raise ValueError("server_update_p99_us absent or invalid") from error
    reported_calls = {}
    for name, value in fields.items():
        if name.startswith("frame_") and name.endswith("_p99_us"):
            try:
                reported_calls[name] = float(value)
            except ValueError as error:
                raise ValueError(f"invalid {name}") from error
    return dict(raw=summaries[0], fields=fields, server_update_p99_us=server_p99,
                server_update_under_100us=server_p99 < 100.0,
                reported_frame_call_p99_us=reported_calls,
                reported_calls_at_or_above_100us=sorted(name for name, value in reported_calls.items()
                                                      if value >= 100.0),
                every_main_call_budget="UNMEASURED")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--mode", choices=("timings", "samples"), required=True)
    p.add_argument("--source", type=Path, required=True)
    p.add_argument("--build-dir", type=Path, required=True)
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--expected-head", required=True)
    p.add_argument("--server-transport", required=True)
    p.add_argument("--client-transport", default="portable")
    p.add_argument("--seconds", type=int, default=60)
    args = p.parse_args()
    if args.seconds < 60:
        p.error("use at least 60 seconds of traffic")
    source, build_dir, binary = (path.resolve() for path in
                                 (args.source, args.build_dir, args.binary))
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    record = dict(mode=args.mode, source=str(source), build_dir=str(build_dir),
                  binary=str(binary), requested_head=args.expected_head,
                  server_transport=args.server_transport,
                  client_transport=args.client_transport, clients=CLIENTS,
                  seconds=args.seconds, started_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    try:
        head = command(["git", "rev-parse", "HEAD"], source)
        status = command(["git", "status", "--porcelain"], source)
        record["head"] = head
        if head != args.expected_head or status:
            raise ValueError("source is dirty or differs from expected head")
        cache = (build_dir / "CMakeCache.txt").read_text()
        if not re.search(r"^CMAKE_BUILD_TYPE:STRING=Release$", cache, re.M):
            raise ValueError("CMake build type is not Release")
        cmake_source = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$", cache, re.M)
        if not cmake_source or Path(cmake_source.group(1)).resolve() != source:
            raise ValueError("CMake build source differs from requested source")
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise ValueError("load binary missing or not executable")
        record["binary_sha256"] = sha256(binary)
        record["preflight"] = preflight(out)
        if record["preflight"]["failures"]:
            raise ValueError("capacity guard: " + "; ".join(record["preflight"]["failures"]))
        if args.mode == "samples" and platform.system() != "Darwin":
            raise ValueError("Time Profiler sampling mode requires macOS")
        env = os.environ.copy()
        env.update(LOAD_PROFILE="light", ROCKETNET_SERVER_TRANSPORT=args.server_transport,
                   ROCKETNET_CLIENT_TRANSPORT=args.client_transport)
        runs = 3 if args.mode == "timings" else 1
        record["runs"] = []
        for index in range(runs):
            guard = preflight(out)
            if guard["failures"]:
                raise ValueError(f"capacity guard before run {index + 1}: {guard['failures']}")
            run = dict(index=index + 1, preflight=guard)
            argv = [str(binary), str(CLIENTS), str(args.seconds)]
            if args.mode == "samples":
                trace = out / "time-profiler.trace"
                argv = ["xcrun", "xctrace", "record", "--template", "Time Profiler",
                        "--output", str(trace), "--time-limit", f"{args.seconds + 30}s",
                        "--target-stdout", str(out / "load-samples.stdout.log"),
                        "--target-stdin", "/dev/null", "--env", "LOAD_PROFILE=light",
                        "--env", f"ROCKETNET_SERVER_TRANSPORT={args.server_transport}",
                        "--env", f"ROCKETNET_CLIENT_TRANSPORT={args.client_transport}",
                        "--launch", "--"] + argv
            run["command"] = argv
            proc = subprocess.run(argv, cwd=source, env=env, text=True, capture_output=True,
                                  timeout=args.seconds + 180)
            (out / f"run-{index + 1}.stdout.log").write_text(proc.stdout)
            (out / f"run-{index + 1}.stderr.log").write_text(proc.stderr)
            run["exit"] = proc.returncode
            if args.mode == "timings":
                run["load"] = parse_load(proc.stdout, args.server_transport, args.client_transport)
            else:
                target_stdout = out / "load-samples.stdout.log"
                if not target_stdout.is_file():
                    raise ValueError("xctrace did not preserve target load stdout")
                run["load"] = parse_load(target_stdout.read_text(), args.server_transport,
                                         args.client_transport)
                if not trace.is_dir():
                    raise ValueError("xctrace did not create the Time Profiler trace")
                run["trace"] = str(trace)
                run["load_stdout"] = str(target_stdout)
            record["runs"].append(run)
            if proc.returncode != 0:
                raise ValueError(f"run {index + 1} exited {proc.returncode}")
        record["verdict"] = "COLLECTED_NOT_QUALIFIED"
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired, ValueError) as error:
        record["verdict"] = "INCOMPLETE"
        record["error"] = str(error)
    (out / "receipt.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    print(f"profile_1000_load: {record['verdict']} receipt={out / 'receipt.json'}")
    if "error" in record:
        print(record["error"], file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
