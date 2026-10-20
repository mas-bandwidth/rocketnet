#!/usr/bin/env python3
"""Sample one process's resources from /proc and judge whether they grow.

usage:
    resource_watch.py --pid PID [--interval N] [--csv FILE]
    resource_watch.py [--interval N] [--csv FILE] -- COMMAND [ARG...]
    resource_watch.py --plant-fd-leak SECONDS [--interval N] [--csv FILE]

Every N seconds (default 10) it reads /proc/PID/fd (the open descriptor
count), /proc/PID/status Threads, VmRSS and VmLck, and appends one CSV row:
seconds since the first sample, fds, threads, vmrss_kib, vmlck_kib. It stops
when the process exits. With -- COMMAND it starts the command, watches it and
exits with the command's failure when the command fails.

The verdict, printed as one "RESOURCE-WATCH verdict=..." line: the first 10
percent of the samples are the warm-up and are dropped; over the rest a
least-squares line is fitted to each column. The run is "growing" when the
fitted change of fds or of threads across the judged window exceeds plus or
minus 2, or when the fitted slope of VmRSS or VmLck exceeds +1 MiB per hour;
otherwise it is "flat". Fewer than 10 judged samples is "short", never flat.

--plant-fd-leak SECONDS is the red proof: it starts a scratch child that opens
one descriptor per second for SECONDS seconds, watches it at a 1 second
interval, and must print verdict=growing.

exit: 0 flat, 1 growing, short, or the watched command failed, 2 usage or a
/proc read refused at the start.
"""

import argparse
import os
import subprocess
import sys
import time

FD_THREAD_LIMIT = 2.0
MEMORY_LIMIT_KIB_PER_HOUR = 1024.0
WARM_UP_FRACTION = 0.10
MINIMUM_JUDGED = 10


def sample(pid):
    try:
        fds = len(os.listdir("/proc/%d/fd" % pid))
        fields = {}
        with open("/proc/%d/status" % pid) as status:
            for line in status:
                key, _, value = line.partition(":")
                fields[key] = value.strip()
    except (FileNotFoundError, ProcessLookupError):
        return None
    def kib(name):
        value = fields.get(name, "0 kB").split()
        return int(value[0]) if value else 0
    return fds, int(fields.get("Threads", "0")), kib("VmRSS"), kib("VmLck")


def slope(xs, ys):
    n = len(xs)
    mean_x = sum(xs) / n
    mean_y = sum(ys) / n
    sxx = sum((x - mean_x) ** 2 for x in xs)
    if sxx == 0:
        return 0.0
    return sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys)) / sxx


def verdict(rows):
    judged = rows[int(len(rows) * WARM_UP_FRACTION):]
    if len(judged) < MINIMUM_JUDGED:
        return "short", "judged=%d samples (need %d)" % (len(judged), MINIMUM_JUDGED)
    ts = [row[0] for row in judged]
    window = ts[-1] - ts[0]
    fd_change = slope(ts, [row[1] for row in judged]) * window
    thread_change = slope(ts, [row[2] for row in judged]) * window
    rss_per_hour = slope(ts, [row[3] for row in judged]) * 3600.0
    lck_per_hour = slope(ts, [row[4] for row in judged]) * 3600.0
    growing = (abs(fd_change) > FD_THREAD_LIMIT or abs(thread_change) > FD_THREAD_LIMIT
               or rss_per_hour > MEMORY_LIMIT_KIB_PER_HOUR or lck_per_hour > MEMORY_LIMIT_KIB_PER_HOUR)
    detail = ("judged=%d window=%.0fs fd_change=%+.2f thread_change=%+.2f "
              "vmrss_kib_per_hour=%+.1f vmlck_kib_per_hour=%+.1f last_fds=%d last_threads=%d last_vmrss_kib=%d"
              % (len(judged), window, fd_change, thread_change, rss_per_hour, lck_per_hour,
                 judged[-1][1], judged[-1][2], judged[-1][3]))
    return ("growing" if growing else "flat"), detail


def watch(pid, interval, csv_path, child=None):
    rows = []
    out = open(csv_path, "w") if csv_path else None
    if out:
        out.write("seconds,fds,threads,vmrss_kib,vmlck_kib\n")
    start = time.monotonic()
    next_time = start
    while True:
        if child is not None and child.poll() is not None:
            break
        values = sample(pid)
        if values is None:
            break
        row = (time.monotonic() - start,) + values
        rows.append(row)
        if out:
            out.write("%.1f,%d,%d,%d,%d\n" % row)
            out.flush()
        next_time += interval
        while True:
            remaining = next_time - time.monotonic()
            if remaining <= 0:
                break
            if child is not None:
                try:
                    child.wait(timeout=min(remaining, 1.0))
                    break
                except subprocess.TimeoutExpired:
                    continue
            time.sleep(min(remaining, 1.0))
            if not os.path.exists("/proc/%d" % pid):
                break
    if out:
        out.close()
    return rows


LEAKER = "import os, sys, time\nfds = []\nfor _ in range(int(sys.argv[1])):\n    fds.append(os.open(os.devnull, os.O_RDONLY))\n    time.sleep(1.0)\n"


def main():
    parser = argparse.ArgumentParser(description="sample a pid from /proc and judge fd, thread and memory growth")
    parser.add_argument("--pid", type=int)
    parser.add_argument("--interval", type=float, default=10.0)
    parser.add_argument("--csv")
    parser.add_argument("--plant-fd-leak", type=int, metavar="SECONDS")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command

    modes = sum([args.pid is not None, bool(command), args.plant_fd_leak is not None])
    if modes != 1 or args.interval <= 0:
        parser.error("give exactly one of --pid, -- COMMAND or --plant-fd-leak, and a positive --interval")

    child = None
    if args.plant_fd_leak is not None:
        args.interval = 1.0
        child = subprocess.Popen([sys.executable, "-I", "-c", LEAKER, str(args.plant_fd_leak)])
        pid = child.pid
    elif command:
        child = subprocess.Popen(command)
        pid = child.pid
    else:
        pid = args.pid
        if sample(pid) is None:
            print("RESOURCE-WATCH error: pid %d is not readable in /proc" % pid)
            return 2

    rows = watch(pid, args.interval, args.csv, child)
    code = child.wait() if child is not None else 0
    result, detail = verdict(rows)
    print("RESOURCE-WATCH verdict=%s pid=%d samples=%d interval=%gs %s" % (result, pid, len(rows), args.interval, detail))
    sys.stdout.flush()
    if child is not None and code != 0:
        print("RESOURCE-WATCH command exit=%d" % code)
        return 1
    return 0 if result == "flat" else 1


if __name__ == "__main__":
    sys.exit(main())
