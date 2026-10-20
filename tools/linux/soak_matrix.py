#!/usr/bin/env python3
"""The Linux soak matrix: every server transport soaked for a fixed time under
the resource watcher, impaired and clean, with the xdp cell inside a guest.

Two scripts:

  tools/linux/resource_watch.py samples one pid from /proc (fd count, thread
  count, VmRSS, VmLck) every N seconds into a CSV and prints one line,
  "RESOURCE-WATCH verdict=flat|growing|short ...". After a warm-up of the
  first 10 percent of samples it fits a least-squares line to each column:
  growing when the fitted change of fds or threads across the judged window
  exceeds plus or minus 2, or VmRSS or VmLck grows faster than 1 MiB per hour.
  Exit 0 is flat. `resource_watch.py --plant-fd-leak 40` is its red proof: a
  scratch child leaking one fd per second must read growing.

  tools/linux/soak_matrix.py (this file) runs `soak --seconds S` under the
  watcher, one run at a time, for each cell:

    portable/portable impaired   gso/portable impaired   io_uring/portable impaired
    portable/portable clean      gso/portable clean      io_uring/portable clean
    xdp/portable impaired        (root inside a virtme-ng guest of this
                                  kernel, the watcher inside the guest)

  impaired is the link simulator on both ends: loss 5 percent, latency 30 ms,
  jitter 20 ms, duplicate 2 percent (ROCKETNET_SERVER_SIMULATED_* and
  ROCKETNET_CLIENT_SIMULATED_*); clean sets none. The server transport is
  pinned with ROCKETNET_SERVER_TRANSPORT and the client with
  ROCKETNET_CLIENT_TRANSPORT=portable. The xdp cell builds a namespace and a
  veth pair inside the guest, the server on the near end and every client
  socket in the far namespace (ROCKETNET_SOAK_CLIENT_NETNS), and never runs
  as root on the host.

usage:
    python3 tools/linux/soak_matrix.py [--seconds S] [--interval N] [--cells a,b]
                                       [--soak PATH] [--out DIR] [--sanitized]

  --cells takes names from: portable-impaired, gso-impaired,
  io_uring-impaired, portable-clean, gso-clean, io_uring-clean, xdp-impaired
  (default: all seven). --soak is the soak binary (default bin/soak).
  --sanitized marks an ASan+LSan binary: the run is judged on a clean exit
  (no sanitizer report, leak check at exit on) and the watcher's verdict is
  recorded but not judged, since the sanitizers' own quarantine grows RSS.

Reading the verdicts. Each run prints one "SOAK-MATRIX <cell> PASS|FAIL ..."
line and the end prints the table. A run passes when soak exits 0 (zero
payload verify failures, counters conserved on the server and every client,
no unplanned disconnect, only the injected duplicates refused), the server
ran on the pinned transport, every disconnect seen is one of soak's planned
reconnects (DISCONNECTED == reconnects + 1), and the watcher reads flat.
"conservation" in the table is soak's own rocketnet_counters_conserved check
on both ends, with the sent, received and verified payload counts beside it.
Logs and CSVs land in --out (default build/soak-matrix), one
<cell>.log and <cell>.csv per run. Exit 0 when every run passed.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WATCH = os.path.join(HERE, "resource_watch.py")

IMPAIRED = {"SIMULATED_LOSS_PERCENT": "5", "SIMULATED_LATENCY_MS": "30",
            "SIMULATED_JITTER_MS": "20", "SIMULATED_DUPLICATE_PERCENT": "2"}

CELLS = ["portable-impaired", "gso-impaired", "io_uring-impaired",
         "portable-clean", "gso-clean", "io_uring-clean", "xdp-impaired"]

NETNS = "rnsoak"
NEAR = "rnsoak0"
FAR = "rnsoak1"
SERVER_HOST = "10.77.3.1"
SERVER_PORT = "40413"


def cell_env(cell):
    server, profile = cell.split("-")
    env = dict(os.environ)
    for key in list(env):
        if key.startswith("ROCKETNET_"):
            del env[key]
    env["ROCKETNET_SERVER_TRANSPORT"] = server
    env["ROCKETNET_CLIENT_TRANSPORT"] = "portable"
    if profile == "impaired":
        for field, value in IMPAIRED.items():
            env["ROCKETNET_SERVER_" + field] = value
            env["ROCKETNET_CLIENT_" + field] = value
    if server == "xdp":
        env["ROCKETNET_SERVER_NUM_XDP_QUEUES"] = "1"
        env["ROCKETNET_SERVER_NUM_OS_CPUS"] = "1"
        env["ROCKETNET_SOAK_SERVER_HOST"] = SERVER_HOST
        env["ROCKETNET_SOAK_SERVER_PORT"] = SERVER_PORT
        env["ROCKETNET_SOAK_CLIENT_NETNS"] = "/var/run/netns/" + NETNS
    return server, env


def judge(cell, log_text, exit_code, wall, sanitized):
    server = cell.split("-")[0]
    summary = dict(re.findall(r"^soak summary: (\S+) (\d+)$", log_text, re.M))
    finished = re.search(r"^soak finished: (\d+) iterations, (\d+) connects, (\d+) payloads verified, (\d+) failures", log_text, re.M)
    watch = re.search(r"^RESOURCE-WATCH verdict=(\S+) .*$", log_text, re.M)
    ran_on = re.search(r"^soak: server transport (\S+)$", log_text, re.M)
    reasons = {k: int(v) for k, v in summary.items() if k.startswith("ROCKETNET_CLIENT_STATE_")}
    reconnects = int(summary.get("reconnects", "0"))
    problems = []
    if exit_code != 0:
        problems.append("exit=%d" % exit_code)
    if not finished:
        problems.append("no finished line")
    elif int(finished.group(4)) != 0:
        problems.append("failures=%s" % finished.group(4))
    if "soak: conservation ok" not in log_text:
        problems.append("conservation not ok")
    if not ran_on or ran_on.group(1) != server:
        problems.append("server ran on %s" % (ran_on.group(1) if ran_on else "nothing"))
    unplanned = sum(v for k, v in reasons.items() if k != "ROCKETNET_CLIENT_STATE_DISCONNECTED")
    if unplanned or reasons.get("ROCKETNET_CLIENT_STATE_DISCONNECTED", 0) != reconnects + 1:
        problems.append("disconnects %s vs reconnects %d" % (reasons, reconnects))
    if re.search(r"ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:", log_text):
        problems.append("sanitizer report")
    verdict = watch.group(1) if watch else "none"
    if not sanitized and verdict != "flat":
        problems.append("watch=%s" % verdict)
    row = {"cell": cell, "result": "FAIL" if problems else "PASS", "wall": wall,
           "sent": summary.get("sent", "-"), "received": summary.get("received", "-"),
           "verified": summary.get("verified", "-"), "reconnects": reconnects,
           "unplanned": unplanned, "conservation": "ok" if "soak: conservation ok" in log_text else "FAILED",
           "watch": verdict, "problems": "; ".join(problems)}
    return row


def run_local(cell, args):
    server, env = cell_env(cell)
    log_path = os.path.join(args.out, cell + ".log")
    csv_path = os.path.join(args.out, cell + ".csv")
    if args.sanitized:
        env["ASAN_OPTIONS"] = "detect_leaks=1"
        supp = os.path.join(ROOT, "tools", "lsan.supp")
        if os.path.exists(supp):
            env["LSAN_OPTIONS"] = "suppressions=" + supp
    command = [sys.executable, "-I", WATCH, "--interval", str(args.interval), "--csv", csv_path,
               "--", args.soak, "--seconds", str(args.seconds)]
    start = time.monotonic()
    with open(log_path, "w") as log:
        code = subprocess.call(command, env=env, stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)
    return code, time.monotonic() - start, log_path


def sh(command, check=True):
    code = subprocess.call(command, shell=True)
    if check and code != 0:
        raise RuntimeError("command failed (%d): %s" % (code, command))


def guest_xdp(args):
    # runs as root inside the guest: the pair, the soak, the teardown
    if os.geteuid() != 0 or "virtme" not in open("/proc/cmdline").read():
        print("SOAK-MATRIX xdp refused: the xdp cell runs only as root inside a virtme-ng guest")
        return 2
    try:
        if subprocess.call("stat -f -c %T /sys/fs/bpf | grep -q bpf_fs", shell=True) != 0:
            sh("mount -t bpf bpf /sys/fs/bpf")
        sh("ip netns del %s 2>/dev/null; ip link del %s 2>/dev/null" % (NETNS, NEAR), check=False)
        sh("ip netns add " + NETNS)
        sh("ip link add %s mtu 1500 type veth peer name %s mtu 1500" % (NEAR, FAR))
        sh("ip link set %s netns %s" % (FAR, NETNS))
        sh("ip addr add %s/24 dev %s" % (SERVER_HOST, NEAR))
        sh("ip -n %s addr add 10.77.3.2/24 dev %s" % (NETNS, FAR))
        sh("ip link set %s up" % NEAR)
        sh("echo 2 > /sys/class/net/%s/napi_defer_hard_irqs" % NEAR)
        sh("echo 200000 > /sys/class/net/%s/gro_flush_timeout" % NEAR)
        sh("ip -n %s link set lo up" % NETNS)
        sh("ip -n %s link set %s up" % (NETNS, FAR))
        if shutil.which("ethtool"):
            sh("ip netns exec %s ethtool -K %s tx off >/dev/null" % (NETNS, FAR))
        code, wall, log_path = run_local(args.cells[0], args)
        with open(os.path.join(args.out, args.cells[0] + ".rc"), "w") as rc:
            rc.write("%d %.0f\n" % (code, wall))
        return code
    finally:
        sh("ip netns del %s 2>/dev/null; ip link del %s 2>/dev/null" % (NETNS, NEAR), check=False)


def run_xdp(cell, args):
    vm_kernel = os.path.expanduser("~/vm-kernels/vmlinuz-" + os.uname().release)
    log_path = os.path.join(args.out, cell + ".log")
    rc_path = os.path.join(args.out, cell + ".rc")
    if os.path.exists(rc_path):
        os.unlink(rc_path)
    if not shutil.which("vng") or not os.access(vm_kernel, os.R_OK) or not os.access("/dev/kvm", os.W_OK):
        with open(log_path, "w") as log:
            log.write("no VM tier: needs vng, a readable %s and a writable /dev/kvm\n" % vm_kernel)
        return 1, 0.0, log_path
    inner = [sys.executable, "-I", os.path.abspath(__file__), "--guest-xdp", "--cells", cell,
             "--seconds", str(args.seconds), "--interval", str(args.interval),
             "--soak", args.soak, "--out", args.out] + (["--sanitized"] if args.sanitized else [])
    command = ["timeout", str(args.seconds + 600), "vng", "--run", vm_kernel, "--memory", "2048M", "--cpus", "2",
               "--append", "irqaffinity=0 isolcpus=1 nohz_full=1", "--rwdir", ROOT, "--rwdir", args.out, "--", "sudo"] + inner
    start = time.monotonic()
    with open(os.path.join(args.out, cell + ".vm.log"), "w") as vm_log:
        subprocess.call(command, stdout=vm_log, stderr=subprocess.STDOUT, cwd=ROOT)
    wall = time.monotonic() - start
    try:
        code = int(open(rc_path).read().split()[0])
    except (OSError, ValueError, IndexError):
        code = 1
    return code, wall, log_path


def main():
    parser = argparse.ArgumentParser(description="the Linux soak matrix under the resource watcher")
    parser.add_argument("--seconds", type=int, default=1800)
    parser.add_argument("--interval", type=float, default=10.0)
    parser.add_argument("--cells", default=",".join(CELLS))
    parser.add_argument("--soak", default=os.path.join(ROOT, "bin", "soak"))
    parser.add_argument("--out", default=os.path.join(ROOT, "build", "soak-matrix"))
    parser.add_argument("--sanitized", action="store_true")
    parser.add_argument("--guest-xdp", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.cells = [c for c in args.cells.split(",") if c]
    args.soak = os.path.abspath(args.soak)
    args.out = os.path.abspath(args.out)
    for cell in args.cells:
        if cell not in CELLS:
            parser.error("unknown cell %s (cells: %s)" % (cell, ", ".join(CELLS)))
    os.makedirs(args.out, exist_ok=True)

    if args.guest_xdp:
        return guest_xdp(args)

    rows = []
    for cell in args.cells:
        if cell.startswith("xdp"):
            code, wall, log_path = run_xdp(cell, args)
        else:
            code, wall, log_path = run_local(cell, args)
        try:
            text = open(log_path, errors="replace").read()
        except OSError:
            text = ""
        row = judge(cell, text, code, wall, args.sanitized)
        rows.append(row)
        print("SOAK-MATRIX %s %s wall=%.0fs sent=%s received=%s verified=%s reconnects=%s unplanned=%s conservation=%s watch=%s %s"
              % (cell, row["result"], wall, row["sent"], row["received"], row["verified"], row["reconnects"],
                 row["unplanned"], row["conservation"], row["watch"], row["problems"]))
        sys.stdout.flush()

    print("\n| cell | result | wall | sent | received | verified | reconnects | unplanned | conservation | watch |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        print("| %s | %s | %.0fs | %s | %s | %s | %s | %s | %s | %s |" % (r["cell"], r["result"], r["wall"], r["sent"],
              r["received"], r["verified"], r["reconnects"], r["unplanned"], r["conservation"], r["watch"]))
    return 0 if all(r["result"] == "PASS" for r in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
