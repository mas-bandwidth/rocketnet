#!/usr/bin/env python3
"""Run rocketnet binaries under the macOS leaks tool.

LeakSanitizer has no Apple runtime, so leaks are checked on a Release build
without sanitizers: every run sets MallocStackLogging=1 and goes through
    /usr/bin/leaks --atExit -- <binary> [args...]
A run fails when leaks reports a non-zero leak count or the binary exits
non-zero; the report carries the stacks leaks printed.

leaks --atExit stops the target at exit and attaches to it, which needs
Developer Mode: with it disabled, leaks waits for an authorization a
non-interactive session never grants and the stopped target never exits.
So the script prints `DevToolsSecurity -status` first and refuses to run
(exit 2) when it reports disabled, and every leaks call runs in its own
process group under a hard timeout (--timeout, default 120 s) that kills
leaks and its stopped target and fails the run.

Modes:
  --binary PATH [-- arg...]   run one binary once
  --matrix                    run test_lifecycle, functional and a 100000
                              iteration soak under each macOS transport pin
                              and with ROCKETNET_PUMP=portable on auto
  --self-test                 compile a scratch binary that leaks one
                              allocation and confirm the run turns red
"""

import argparse
import os
import platform
import re
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
LEAKS = "/usr/bin/leaks"
# each pin is the environment one run adds; the last leaves both transports on
# auto with the batch pump forced off, the degraded path of a msgx socket
PINS = (
    {"ROCKETNET_SERVER_TRANSPORT": "portable", "ROCKETNET_CLIENT_TRANSPORT": "portable"},
    {"ROCKETNET_SERVER_TRANSPORT": "portable", "ROCKETNET_CLIENT_TRANSPORT": "msgx"},
    {"ROCKETNET_SERVER_TRANSPORT": "msgx", "ROCKETNET_CLIENT_TRANSPORT": "portable"},
    {"ROCKETNET_SERVER_TRANSPORT": "msgx", "ROCKETNET_CLIENT_TRANSPORT": "msgx"},
    {"ROCKETNET_PUMP": "portable"},
)
PIN_VARIABLES = ("ROCKETNET_SERVER_TRANSPORT", "ROCKETNET_CLIENT_TRANSPORT", "ROCKETNET_PUMP")
DEFAULT_TIMEOUT_SECONDS = 120.0
LEAK_LINE = re.compile(r"Process\s+\d+:\s+(\d+)\s+leaks?\s+for\s+(\d+)\s+total leaked bytes")


def check_platform():
    if platform.system() != "Darwin":
        sys.exit("leaks_run.py: this script runs on macOS only (found %s)" % platform.system())
    if not os.access(LEAKS, os.X_OK):
        sys.exit("leaks_run.py: %s is missing or not executable" % LEAKS)


def check_developer_mode():
    try:
        proc = subprocess.run(["DevToolsSecurity", "-status"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        status = proc.stdout.decode("utf-8", "replace").strip()
    except (OSError, subprocess.TimeoutExpired) as error:
        status = "DevToolsSecurity -status did not answer: %s" % error
    print("leaks_run.py: DevToolsSecurity -status: %s" % status, flush=True)
    if "disabled" in status:
        print("leaks_run.py: BLOCKED Developer Mode is disabled, so leaks --atExit cannot attach to the stopped target", flush=True)
        sys.exit(2)


def leak_counts(text):
    counts = [(int(m.group(1)), int(m.group(2))) for m in LEAK_LINE.finditer(text)]
    return counts


def run_leaks(binary, args, env_extra, timeout):
    env = dict(os.environ)
    for name in PIN_VARIABLES:
        env.pop(name, None)
    env["MallocStackLogging"] = "1"
    env.update(env_extra)
    command = [LEAKS, "--atExit", "--", str(binary)] + list(args)
    print("leaks_run.py: %s" % " ".join("%s=%s" % item for item in env_extra.items()), flush=True)
    print("leaks_run.py: %s" % " ".join(command), flush=True)
    # its own process group, so a timeout kills leaks and the target it stopped
    proc = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        raw, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        raw, _ = proc.communicate()
        sys.stdout.write(raw.decode("utf-8", "replace"))
        print("leaks_run.py: FAIL %s: leaks did not return within %.0f s; killed it and its target" % (binary, timeout), flush=True)
        return False
    output = raw.decode("utf-8", "replace")
    sys.stdout.write(output)
    sys.stdout.flush()
    counts = leak_counts(output)
    leaked = sum(count for count, _ in counts)
    if not counts:
        print("leaks_run.py: FAIL %s: leaks printed no leak summary" % binary, flush=True)
        return False
    if leaked:
        print("leaks_run.py: FAIL %s: %d leak(s) across %d process report(s)" % (binary, leaked, len(counts)), flush=True)
        return False
    if proc.returncode != 0:
        print("leaks_run.py: FAIL %s: binary exited %d" % (binary, proc.returncode), flush=True)
        return False
    print("leaks_run.py: PASS %s: 0 leaks" % binary, flush=True)
    return True


def binary_path(name):
    path = REPO_ROOT / "bin" / name
    if not path.is_file():
        sys.exit("leaks_run.py: missing binary %s (build Release without sanitizers first)" % path)
    return path


def matrix(timeout):
    ok = True
    for pin in PINS:
        label = " ".join("%s=%s" % item for item in pin.items())
        print("leaks_run.py: pin %s" % label, flush=True)
        ok = run_leaks(binary_path("test_lifecycle"), [], pin, timeout) and ok
        ok = run_leaks(binary_path("functional"), [], pin, timeout) and ok
        ok = run_leaks(binary_path("soak"), ["100000"], pin, timeout) and ok
    return ok


def self_test(timeout):
    source = (
        "#include <stdlib.h>\n"
        "static void *volatile held;\n"
        "__attribute__((noinline)) static void lose(void) {\n"
        "    char *p = (char *)malloc(32);\n"
        "    if (!p) return;\n"
        "    p[0] = 1;\n"
        "    held = p;\n"
        "}\n"
        "int main(void) {\n"
        "    lose();\n"
        "    held = 0;\n"
        "    return 0;\n"
        "}\n"
    )
    with tempfile.TemporaryDirectory(prefix="leaks-run-") as tmp:
        src = Path(tmp) / "leak_one.c"
        binary = Path(tmp) / "leak_one"
        src.write_text(source)
        build = subprocess.run(["cc", "-O0", "-g", "-o", str(binary), str(src)])
        if build.returncode != 0:
            print("leaks_run.py: self-test could not compile the scratch leak", flush=True)
            return False
        red = not run_leaks(binary, [], {}, timeout)
        if red:
            print("leaks_run.py: PASS self-test: the scratch leak turned the run red", flush=True)
        else:
            print("leaks_run.py: FAIL self-test: the scratch leak was not reported", flush=True)
        return red


def main():
    parser = argparse.ArgumentParser(description="Run rocketnet under the macOS leaks tool")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--binary", help="binary under bin/ to run once")
    mode.add_argument("--matrix", action="store_true", help="run the three suites under every transport pin")
    mode.add_argument("--self-test", action="store_true", help="confirm a one-allocation leak turns a run red")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS,
                        help="seconds one leaks run may take before it and its target are killed (default 120)")
    parser.add_argument("args", nargs=argparse.REMAINDER, help="arguments for --binary after --")
    options = parser.parse_args()
    check_platform()
    check_developer_mode()
    if options.self_test:
        return 0 if self_test(options.timeout) else 1
    if options.matrix:
        return 0 if matrix(options.timeout) else 1
    args = list(options.args)
    if args and args[0] == "--":
        args = args[1:]
    return 0 if run_leaks(binary_path(options.binary), args, {}, options.timeout) else 1


if __name__ == "__main__":
    sys.exit(main())
