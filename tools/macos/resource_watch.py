#!/usr/bin/env python3
"""resource_watch.py: sample one macOS process's fds, threads and memory and judge them flat.

usage:
  resource_watch.py --pid PID --csv OUT.csv [--interval N] [--verdict OUT.txt]
  resource_watch.py --csv OUT.csv [--interval N] -- command [args...]
  resource_watch.py --planted-leak-check [--csv OUT.csv] [--seconds S]

Every --interval seconds (default 5) it writes one CSV row for the process:
time_s, fds, threads, rss_bytes, phys_footprint_bytes. fds is the length of
the PROC_PIDLISTFDS list, threads is pti_threadnum from PROC_PIDTASKINFO, rss
is pti_resident_size, and phys_footprint is ri_phys_footprint from
proc_pid_rusage, all read through ctypes over libproc (lsof -p and ps -o rss=
are the fallback when libproc refuses). Sampling ends when the process exits
(or on SIGINT/SIGTERM to the watcher).

The verdict, judged at exit over the samples after a 10 percent warm-up (the
first tenth of the samples is discarded), is the least-squares slope of each
series against time, in units per hour:
  fds            red when |slope| > 2 fds per hour
  threads        red when |slope| > 2 threads per hour
  rss            red when slope > 1 MiB per hour
  phys_footprint red when slope > 1 MiB per hour
It prints one line per series and a last line "resource_watch: verdict FLAT"
or "resource_watch: verdict RED (<series>,...)"; fewer than 4 samples after
the warm-up is "resource_watch: verdict INSUFFICIENT". The exit code is 0 for
FLAT, 1 for RED or INSUFFICIENT, 2 for a usage error. In command mode the
watcher runs the command, watches its pid and, on a FLAT verdict, exits with
the command's own exit code.

--planted-leak-check is the red proof: it starts a scratch Python child that
opens one fd per second and never closes it, watches it for --seconds (default
40) at a 1 second interval, and passes (exit 0) only when the verdict is RED
on fds.
"""

import argparse
import ctypes
import ctypes.util
import os
import signal
import subprocess
import sys
import time

PROC_PIDLISTFDS = 1
PROC_PIDTASKINFO = 4
PROC_FDINFO_SIZE = 8
RUSAGE_INFO_V0 = 0
MIB = 1024.0 * 1024.0

FD_LIMIT_PER_HOUR = 2.0
THREAD_LIMIT_PER_HOUR = 2.0
MEMORY_LIMIT_BYTES_PER_HOUR = 1.0 * MIB
WARMUP_FRACTION = 0.10


class ProcTaskInfo(ctypes.Structure):
    _fields_ = [
        ("pti_virtual_size", ctypes.c_uint64),
        ("pti_resident_size", ctypes.c_uint64),
        ("pti_total_user", ctypes.c_uint64),
        ("pti_total_system", ctypes.c_uint64),
        ("pti_threads_user", ctypes.c_uint64),
        ("pti_threads_system", ctypes.c_uint64),
        ("pti_policy", ctypes.c_int32),
        ("pti_faults", ctypes.c_int32),
        ("pti_pageins", ctypes.c_int32),
        ("pti_cow_faults", ctypes.c_int32),
        ("pti_messages_sent", ctypes.c_int32),
        ("pti_messages_received", ctypes.c_int32),
        ("pti_syscalls_mach", ctypes.c_int32),
        ("pti_syscalls_unix", ctypes.c_int32),
        ("pti_csw", ctypes.c_int32),
        ("pti_threadnum", ctypes.c_int32),
        ("pti_numrunning", ctypes.c_int32),
        ("pti_priority", ctypes.c_int32),
    ]


class RusageInfoV0(ctypes.Structure):
    _fields_ = [
        ("ri_uuid", ctypes.c_uint8 * 16),
        ("ri_user_time", ctypes.c_uint64),
        ("ri_system_time", ctypes.c_uint64),
        ("ri_pkg_idle_wkups", ctypes.c_uint64),
        ("ri_interrupt_wkups", ctypes.c_uint64),
        ("ri_pageins", ctypes.c_uint64),
        ("ri_wired_size", ctypes.c_uint64),
        ("ri_resident_size", ctypes.c_uint64),
        ("ri_phys_footprint", ctypes.c_uint64),
        ("ri_proc_start_abstime", ctypes.c_uint64),
        ("ri_proc_exit_abstime", ctypes.c_uint64),
    ]


def load_libproc():
    path = ctypes.util.find_library("proc") or "/usr/lib/libproc.dylib"
    try:
        lib = ctypes.CDLL(path, use_errno=True)
    except OSError:
        return None
    lib.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_int]
    lib.proc_pidinfo.restype = ctypes.c_int
    lib.proc_pid_rusage.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
    lib.proc_pid_rusage.restype = ctypes.c_int
    return lib


LIBPROC = load_libproc()


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    # a zombie child still answers kill 0; reap it when it is ours
    try:
        done, _ = os.waitpid(pid, os.WNOHANG)
        if done == pid:
            return False
    except ChildProcessError:
        pass
    return True


def fd_count(pid):
    if LIBPROC is not None:
        need = LIBPROC.proc_pidinfo(pid, PROC_PIDLISTFDS, 0, None, 0)
        if need > 0:
            size = need + 64 * PROC_FDINFO_SIZE
            buf = ctypes.create_string_buffer(size)
            got = LIBPROC.proc_pidinfo(pid, PROC_PIDLISTFDS, 0, buf, size)
            if got > 0:
                return got // PROC_FDINFO_SIZE
    out = subprocess.run(["lsof", "-n", "-P", "-p", str(pid)], capture_output=True, text=True).stdout
    lines = [line for line in out.splitlines()[1:] if line.split() and line.split()[3][:1].isdigit()]
    return len(lines) if out else None


def task_info(pid):
    if LIBPROC is not None:
        info = ProcTaskInfo()
        got = LIBPROC.proc_pidinfo(pid, PROC_PIDTASKINFO, 0, ctypes.byref(info), ctypes.sizeof(info))
        if got == ctypes.sizeof(info):
            return info.pti_threadnum, info.pti_resident_size
    out = subprocess.run(["ps", "-M", "-p", str(pid)], capture_output=True, text=True).stdout
    threads = max(0, len(out.splitlines()) - 1) if out else None
    rss = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    return threads, (int(rss) * 1024 if rss.isdigit() else None)


def phys_footprint(pid):
    if LIBPROC is None:
        return None
    info = RusageInfoV0()
    if LIBPROC.proc_pid_rusage(pid, RUSAGE_INFO_V0, ctypes.byref(info)) != 0:
        return None
    return info.ri_phys_footprint


def sample(pid):
    fds = fd_count(pid)
    threads, rss = task_info(pid)
    footprint = phys_footprint(pid)
    if fds is None or threads is None or rss is None:
        return None
    return fds, threads, rss, footprint if footprint is not None else ""


def slope_per_hour(times, values):
    n = len(times)
    mean_t = sum(times) / n
    mean_v = sum(values) / n
    sxx = sum((t - mean_t) ** 2 for t in times)
    if sxx == 0:
        return 0.0
    sxy = sum((t - mean_t) * (v - mean_v) for t, v in zip(times, values))
    return sxy / sxx * 3600.0


def judge(rows, out=sys.stdout):
    """rows: (time_s, fds, threads, rss, footprint). Returns (verdict, red series list)."""
    skip = int(len(rows) * WARMUP_FRACTION)
    kept = rows[skip:]
    print("resource_watch: %d samples, %d after the 10 percent warm-up" % (len(rows), len(kept)), file=out)
    if len(kept) < 4:
        print("resource_watch: verdict INSUFFICIENT", file=out)
        return "INSUFFICIENT", []
    times = [r[0] for r in kept]
    red = []
    checks = [
        ("fds", 1, FD_LIMIT_PER_HOUR, True, "fds/h"),
        ("threads", 2, THREAD_LIMIT_PER_HOUR, True, "threads/h"),
        ("rss", 3, MEMORY_LIMIT_BYTES_PER_HOUR, False, "MiB/h"),
        ("phys_footprint", 4, MEMORY_LIMIT_BYTES_PER_HOUR, False, "MiB/h"),
    ]
    for name, index, limit, two_sided, unit in checks:
        values = [r[index] for r in kept]
        if any(v == "" for v in values):
            print("resource_watch: %-14s not sampled" % name, file=out)
            continue
        slope = slope_per_hour(times, values)
        bad = abs(slope) > limit if two_sided else slope > limit
        if unit == "MiB/h":
            shown, bound = slope / MIB, ("> %.1f MiB/h" % (limit / MIB))
        else:
            shown, bound = slope, ("|slope| > %.0f %s" % (limit, unit))
        print("resource_watch: %-14s first %s last %s slope %+.3f %s (red when %s) %s" % (
            name, values[0], values[-1], shown, unit, bound, "RED" if bad else "flat"), file=out)
        if bad:
            red.append(name)
    if red:
        print("resource_watch: verdict RED (%s)" % ",".join(red), file=out)
        return "RED", red
    print("resource_watch: verdict FLAT", file=out)
    return "FLAT", []


def watch(pid, csv_path, interval, stop_after=None, until=None, child=None):
    """sample pid until it exits, stop_after seconds pass, or until() turns true.
    child, when pid is our own Popen, is asked through poll(), so its exit
    status stays with it (alive() would reap it and lose the status). The
    SIGINT and SIGTERM handlers it installs are put back on return."""
    rows = []
    stopping = [False]

    def on_signal(signum, frame):
        stopping[0] = True

    if child is not None:
        def running():
            return child.poll() is None
    else:
        def running():
            return alive(pid)

    previous = {number: signal.signal(number, on_signal) for number in (signal.SIGINT, signal.SIGTERM)}
    try:
        return watch_rows(pid, csv_path, interval, stop_after, until, rows, stopping, running)
    finally:
        for number, handler in previous.items():
            signal.signal(number, handler)


def watch_rows(pid, csv_path, interval, stop_after, until, rows, stopping, running):
    start = time.monotonic()
    with open(csv_path, "w") as csv:
        csv.write("time_s,fds,threads,rss_bytes,phys_footprint_bytes\n")
        next_at = start
        while not stopping[0] and running():
            now = time.monotonic()
            if stop_after is not None and now - start >= stop_after:
                break
            if until is not None and until():
                break
            if now >= next_at:
                s = sample(pid)
                if s is None:
                    if not running():
                        break
                else:
                    t = round(now - start, 3)
                    rows.append((t,) + s)
                    csv.write("%s,%s,%s,%s,%s\n" % ((t,) + s))
                    csv.flush()
                next_at += interval
            time.sleep(min(0.2, max(0.0, next_at - time.monotonic())))
    return rows


LEAKER = (
    "import os, time\n"
    "held = []\n"
    "while True:\n"
    "    held.append(os.open('/dev/null', os.O_RDONLY))\n"
    "    time.sleep(1.0)\n"
)


def planted_leak_check(csv_path, seconds):
    child = subprocess.Popen([sys.executable, "-I", "-c", LEAKER])
    try:
        rows = watch(child.pid, csv_path, 1.0, stop_after=seconds, child=child)
    finally:
        child.kill()
        child.wait()
    verdict, red = judge(rows)
    if verdict == "RED" and "fds" in red:
        print("resource_watch: planted leak check PASSED: a child leaking one fd per second turns the watcher red on fds")
        return 0
    print("resource_watch: planted leak check FAILED: the watcher did not turn red on fds")
    return 1


def main(argv):
    parser = argparse.ArgumentParser(description="sample a macOS process and judge its fds, threads and memory flat")
    parser.add_argument("--pid", type=int)
    parser.add_argument("--csv", default="resource_watch.csv")
    parser.add_argument("--interval", type=float, default=5.0)
    parser.add_argument("--verdict", help="also write the verdict lines to this file")
    parser.add_argument("--planted-leak-check", action="store_true")
    parser.add_argument("--seconds", type=float, default=40.0, help="duration of the planted leak check")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command

    if args.planted_leak_check:
        return planted_leak_check(args.csv, args.seconds)
    if args.interval <= 0:
        print("resource_watch: --interval must be positive", file=sys.stderr)
        return 2
    if (args.pid is None) == (not command):
        print("resource_watch: give exactly one of --pid PID or -- command", file=sys.stderr)
        return 2

    child = None
    pid = args.pid
    if command:
        child = subprocess.Popen(command)
        pid = child.pid
    rows = watch(pid, args.csv, args.interval, child=child)
    child_rc = None
    if child is not None:
        if child.poll() is None:
            child.send_signal(signal.SIGINT)
        child_rc = child.wait()

    lines = []

    class Tee:
        def write(self, text):
            sys.stdout.write(text)
            lines.append(text)

    verdict, _ = judge(rows, out=Tee())
    if args.verdict:
        with open(args.verdict, "w") as f:
            f.write("".join(lines))
    if verdict != "FLAT":
        return 1
    return child_rc if child_rc is not None else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
