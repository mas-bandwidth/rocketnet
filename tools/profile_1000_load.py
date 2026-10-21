#!/usr/bin/env python3
"""Capture honest Release load timing or macOS CPU samples.

This collects the load harness's per-call timing summaries and resource
counts. Its measured call set is incomplete, so the script never emits an
every-main-call frame-budget PASS.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import resource
import signal
import shutil
import subprocess
import sys
import time

CLIENTS = 1000
MAX_CLIENTS = 1000
FD_MARGIN = 64
MIN_TOTAL_BYTES = 14 * 1024**3
MIN_AVAILABLE_BYTES = 12 * 1024**3
MIN_DISK_BYTES = 8 * 1024**3
CRITERIA = (
    "connect_within_10s", "no_unexpected_disconnect", "reliable_delivery",
    "unreliable_integrity", "counters_agree",
)
EXPECTED_FRAME_CALLS = (
    "client_update", "client_send_packets", "client_state", "client_index",
    "client_receive_messages", "client_receipts", "client_start_message",
    "client_finish_message", "server_update", "server_send_packets",
    "server_client_connected", "server_num_connected_clients",
    "server_receive_messages", "server_start_message", "server_finish_message",
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


def parse_mac_vm_stat(output):
    """Count reclaimable pages from vm_stat, ignoring its textual header."""
    match = re.search(r"page size of (\d+) bytes", output)
    if not match:
        raise ValueError("vm_stat page size unavailable")
    page_size = int(match.group(1))
    if page_size <= 0:
        raise ValueError("vm_stat page size invalid")
    wanted = ("Pages free", "Pages inactive", "Pages speculative")
    pages = {}
    for line in output.splitlines():
        name, _, value = line.partition(":")
        name = name.strip()
        if name not in wanted:
            continue
        value_match = re.fullmatch(r"\s*(\d+)\.?\s*", value)
        if not value_match or name in pages:
            raise ValueError(f"vm_stat {name} missing, duplicated, or invalid")
        pages[name] = int(value_match.group(1))
    if any(name not in pages for name in wanted):
        raise ValueError("vm_stat reclaimable page counts unavailable")
    return sum(pages.values()) * page_size


def mac_memory():
    total = int(command(["sysctl", "-n", "hw.memsize"]))
    available = parse_mac_vm_stat(command(["vm_stat"]))
    return total, available


def preflight(out_dir, clients=CLIENTS):
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
    min_fd_hard = 2 * clients + FD_MARGIN
    if hard != resource.RLIM_INFINITY and hard < min_fd_hard:
        failures.append(f"hard fd limit below {min_fd_hard}")
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


def run_capture(argv, source, env, timeout):
    """Keep partial output and kill the whole launched family on timeout."""
    proc = subprocess.Popen(argv, cwd=source, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            start_new_session=True)
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
        return proc.returncode, stdout, stderr, False
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        stdout, stderr = proc.communicate()
        return proc.returncode, stdout, stderr, True


def validate_resource_baseline(fields):
    phase_names = ("pre_init", "post_init", "pre_term", "post_term")
    has_phase_field = any(f"{kind}_{phase}" in fields
                          for kind in ("fds", "threads") for phase in phase_names)
    schema = fields.get("resource_baseline_schema")
    has_new_marker = (has_phase_field or schema is not None or
                      "resource_baseline" in fields or "term_status_code" in fields)
    if not has_new_marker:
        # Historical receipts used start after init and end after term. Keep
        # their old strict rule only when no new resource marker is present.
        for kind in ("fds", "threads"):
            try:
                start = int(fields[f"{kind}_start"])
                end = int(fields[f"{kind}_end"])
            except (KeyError, ValueError) as error:
                raise ValueError(f"{kind} baseline counts missing or invalid") from error
            if start < 0 or end < 0 or start != end:
                raise ValueError(f"{kind} did not return to baseline")
        return "legacy_start_end"
    if schema != "phase_pairs_v1":
        raise ValueError("resource phase fields require phase_pairs_v1 schema")
    if fields.get("resource_baseline") != "PASS" or fields.get("term_status_code") != "0":
        raise ValueError("resource baseline or term status failed")
    for kind in ("fds", "threads"):
        try:
            values = {phase: int(fields[f"{kind}_{phase}"]) for phase in phase_names}
            start = int(fields[f"{kind}_start"])
            end = int(fields[f"{kind}_end"])
        except (KeyError, ValueError) as error:
            raise ValueError(f"{kind} phase counts missing or invalid") from error
        if any(value < 0 for value in values.values()) or start < 0 or end < 0:
            raise ValueError(f"{kind} phase counts must be nonnegative")
        if start != values["post_init"] or end != values["post_term"]:
            raise ValueError(f"{kind} legacy fields disagree with phase counts")
        if values["post_init"] != values["pre_term"] or values["pre_init"] != values["post_term"]:
            raise ValueError(f"{kind} did not return to both phase baselines")
    return schema


def parse_load(stdout, server_transport, client_transport, expected_clients=CLIENTS):
    summaries = [line for line in stdout.splitlines()
                 if line.startswith("load: connected=") and "verdict=" in line]
    if len(summaries) != 1:
        raise ValueError(f"expected one final load summary, found {len(summaries)}")
    pairs = re.findall(r"\b([A-Za-z_][A-Za-z_0-9]*)=([^\s]+)", summaries[0])
    fields = dict(pairs)
    if len(fields) != len(pairs):
        raise ValueError("duplicate fields in final load summary")
    if fields.get("connected") != str(expected_clients) or fields.get("verdict") != "PASS":
        raise ValueError("population or load verdict failed")
    if any(fields.get(name) != "PASS" for name in CRITERIA):
        raise ValueError("one or more load criteria failed")
    if fields.get("server_transport_io") != server_transport or fields.get("client_transport_io") != client_transport:
        raise ValueError("granted transport differs from requested transport")
    resource_baseline_schema = validate_resource_baseline(fields)
    try:
        server_p99 = float(fields["server_update_p99_us"])
    except (KeyError, ValueError) as error:
        raise ValueError("server_update_p99_us absent or invalid") from error
    if not math.isfinite(server_p99) or server_p99 < 0:
        raise ValueError("server_update_p99_us nonfinite or negative")
    reported_calls = {}
    histogram_suffixes = ("p50_upper_us", "p99_upper_us", "max_us", "overbudget_count", "count")
    present_stems = set()
    for name in fields:
        if not name.startswith("frame_"):
            continue
        suffix = next((part for part in histogram_suffixes if name.endswith("_" + part)), None)
        if suffix is None:
            raise ValueError(f"unrecognized frame histogram field {name}")
        present_stems.add(name.removesuffix("_" + suffix))
    for stem in sorted(present_stems):
        name = stem + "_p99_upper_us"
        try:
            count = int(fields[f"{stem}_count"])
            overbudget = int(fields[f"{stem}_overbudget_count"])
            p50 = float(fields[f"{stem}_p50_upper_us"])
            p99 = float(fields[name])
            maximum = float(fields[f"{stem}_max_us"])
        except (KeyError, ValueError) as error:
            raise ValueError(f"incomplete or invalid histogram fields for {name}") from error
        if count < 0 or overbudget < 0 or overbudget > count:
            raise ValueError(f"invalid histogram counts for {name}")
        if any(not math.isfinite(x) or x < 0 for x in (p50, p99, maximum)):
            raise ValueError(f"nonfinite or negative histogram values for {name}")
        if count == 0 and any(x != 0 for x in (p50, p99, maximum, overbudget)):
            raise ValueError(f"zero-sample histogram has nonzero values for {name}")
        reported_calls[stem] = dict(count=count, p50_upper_us=p50,
                                    p99_upper_us=p99, max_us=maximum,
                                    overbudget_count=overbudget,
                                    status="UNMEASURED" if count == 0 else "MEASURED")
    return dict(raw=summaries[0], fields=fields, expected_clients=expected_clients,
                resource_baseline_schema=resource_baseline_schema,
                server_update_p99_us=server_p99,
                server_update_under_100us=server_p99 < 100.0,
                reported_frame_calls=reported_calls,
                missing_frame_calls=sorted("frame_" + name for name in EXPECTED_FRAME_CALLS
                                           if "frame_" + name not in reported_calls),
                unmeasured_frame_calls=sorted(name for name, values in reported_calls.items()
                                              if values["status"] == "UNMEASURED"),
                reported_calls_at_or_above_100us=sorted(name for name, values in reported_calls.items()
                                                      if values["overbudget_count"] > 0),
                every_main_call_budget="UNMEASURED")


def positive_integer(value):
    try:
        parsed = int(value, 10)
    except (TypeError, ValueError) as error:
        raise argparse.ArgumentTypeError("must be a positive integer") from error
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def positive_finite_rate(value):
    try:
        parsed = float(value)
    except (TypeError, ValueError) as error:
        raise argparse.ArgumentTypeError("must be a positive finite rate") from error
    if not math.isfinite(parsed) or parsed <= 0:
        raise argparse.ArgumentTypeError("must be a positive finite rate")
    return parsed


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--mode", choices=("timings", "samples"), required=True)
    p.add_argument("--source", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--expected-head", required=True)
    p.add_argument("--server-transport", required=True)
    p.add_argument("--client-transport", default="portable")
    p.add_argument("--seconds", type=int, default=60)
    p.add_argument("--clients", type=positive_integer, default=CLIENTS)
    p.add_argument("--messages-per-client-second", type=positive_finite_rate)
    args = p.parse_args()
    if args.seconds < 60:
        p.error("use at least 60 seconds of traffic")
    source = args.source.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    build_dir = out / "build"
    binary = out / "bin" / "load"
    record = dict(mode=args.mode, source=str(source), build_dir=str(build_dir),
                  binary=str(binary), requested_head=args.expected_head,
                  server_transport=args.server_transport,
                  client_transport=args.client_transport, clients=args.clients,
                  max_supported_clients=MAX_CLIENTS,
                  messages_per_client_second=args.messages_per_client_second,
                  messages_per_client_second_source=("harness default; environment unset"
                                                     if args.messages_per_client_second is None else
                                                     "LOAD_MESSAGES_PER_CLIENT_SECOND"),
                  rate_environment=({} if args.messages_per_client_second is None else
                                    {"LOAD_MESSAGES_PER_CLIENT_SECOND": str(args.messages_per_client_second)}),
                  seconds=args.seconds, started_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    try:
        head = command(["git", "rev-parse", "HEAD"], source)
        status = command(["git", "status", "--porcelain"], source)
        record["head"] = head
        if head != args.expected_head or status:
            raise ValueError("source is dirty or differs from expected head")
        record["preflight"] = preflight(out, args.clients)
        if record["preflight"]["failures"]:
            raise ValueError("capacity guard: " + "; ".join(record["preflight"]["failures"]))
        if args.mode == "samples" and platform.system() != "Darwin":
            raise ValueError("Time Profiler sampling mode requires macOS")
        configure = ["cmake", "-S", str(source), "-B", str(build_dir),
                     "-DCMAKE_BUILD_TYPE=Release",
                     f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={out / 'bin'}"]
        build = ["cmake", "--build", str(build_dir), "--target", "load", "--parallel", "4"]
        record["configure_command"] = configure
        record["build_command"] = build
        for label, argv in (("configure", configure), ("build", build)):
            code, stdout, stderr, timed_out = run_capture(argv, source, os.environ.copy(), 1200)
            (out / f"{label}.stdout.log").write_text(stdout)
            (out / f"{label}.stderr.log").write_text(stderr)
            record[f"{label}_exit"] = code
            if timed_out or code != 0:
                raise ValueError(f"{label} failed: exit={code} timeout={timed_out}")
        cache = (build_dir / "CMakeCache.txt").read_text()
        if not re.search(r"^CMAKE_BUILD_TYPE:STRING=Release$", cache, re.M):
            raise ValueError("CMake build type is not Release")
        cmake_source = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$", cache, re.M)
        if not cmake_source or Path(cmake_source.group(1)).resolve() != source:
            raise ValueError("CMake build source differs from requested source")
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise ValueError("load binary missing or not executable")
        if command(["git", "rev-parse", "HEAD"], source) != head or command(["git", "status", "--porcelain"], source):
            raise ValueError("source changed during configure/build")
        record["cmake_cache_sha256"] = sha256(build_dir / "CMakeCache.txt")
        record["binary_sha256"] = sha256(binary)
        record["binary_provenance"] = "fresh Release CMake configure and target load build in this receipt"
        env = os.environ.copy()
        env.update(LOAD_PROFILE="light", ROCKETNET_SERVER_TRANSPORT=args.server_transport,
                   ROCKETNET_CLIENT_TRANSPORT=args.client_transport)
        if args.messages_per_client_second is not None:
            env["LOAD_MESSAGES_PER_CLIENT_SECOND"] = str(args.messages_per_client_second)
        runs = 3 if args.mode == "timings" else 1
        record["runs"] = []
        for index in range(runs):
            guard = preflight(out, args.clients)
            if guard["failures"]:
                raise ValueError(f"capacity guard before run {index + 1}: {guard['failures']}")
            run = dict(index=index + 1, preflight=guard)
            argv = [str(binary), str(args.clients), str(args.seconds)]
            if args.mode == "samples":
                trace = out / "time-profiler.trace"
                argv = ["xcrun", "xctrace", "record", "--template", "Time Profiler",
                        "--output", str(trace), "--time-limit", f"{args.seconds + 30}s",
                        "--target-stdout", str(out / "load-samples.stdout.log"),
                        "--target-stdin", "/dev/null", "--env", "LOAD_PROFILE=light",
                        "--env", f"ROCKETNET_SERVER_TRANSPORT={args.server_transport}",
                        "--env", f"ROCKETNET_CLIENT_TRANSPORT={args.client_transport}",
                        *([] if args.messages_per_client_second is None else
                          ["--env", f"LOAD_MESSAGES_PER_CLIENT_SECOND={args.messages_per_client_second}"]),
                        "--launch", "--"] + argv
            run["command"] = argv
            exit_code, stdout, stderr, timed_out = run_capture(argv, source, env,
                                                                args.seconds + 180)
            (out / f"run-{index + 1}.stdout.log").write_text(stdout)
            (out / f"run-{index + 1}.stderr.log").write_text(stderr)
            run["exit"] = exit_code
            run["timed_out"] = timed_out
            record["runs"].append(run)
            if timed_out:
                raise ValueError(f"run {index + 1} timed out; process group killed")
            if args.mode == "timings":
                run["load"] = parse_load(stdout, args.server_transport, args.client_transport, args.clients)
            else:
                target_stdout = out / "load-samples.stdout.log"
                if not target_stdout.is_file():
                    raise ValueError("xctrace did not preserve target load stdout")
                run["load"] = parse_load(target_stdout.read_text(), args.server_transport,
                                         args.client_transport, args.clients)
                if not trace.is_dir():
                    raise ValueError("xctrace did not create the Time Profiler trace")
                run["trace"] = str(trace)
                run["load_stdout"] = str(target_stdout)
            if exit_code != 0:
                raise ValueError(f"run {index + 1} exited {exit_code}")
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
