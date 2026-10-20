#!/usr/bin/env python3
"""Run rocketnet binaries under the macOS leaks tool.

LeakSanitizer has no Apple runtime, so leaks are checked on a Release build
without sanitizers: every run sets MallocStackLogging=1 and goes through
    /usr/bin/leaks --atExit -- <binary> [args...]
leaks --atExit returns its own status, not the target's, so each binary is
first run once on its own with the same environment and arguments: a run
fails when that plain run exits non-zero or leaks reports a non-zero leak
count; the report carries the stacks leaks printed.

leaks --atExit stops the target at exit and attaches to it, which needs
Developer Mode: with it disabled, leaks waits for an authorization a
non-interactive session never grants and the stopped target never exits.
So the script prints `DevToolsSecurity -status` first and refuses to run
(exit 2) when it reports disabled, and every leaks call runs in its own
process group under a hard timeout (--timeout, default 120 s) that kills
leaks and its stopped target and fails the run.

leaks reads a target's writable memory, and prints the allocation stacks,
only when the target is debuggable: the script ad-hoc signs each binary with
the com.apple.security.get-task-allow entitlement before it runs it, and a
run whose output says the process "is not debuggable" fails.

./bin/functional runs each case in a child it spawns, and leaks sees only the
process it launched, so the matrix runs every functional case on its own as
./bin/functional --run <case> under leaks, --jobs at a time (default 8, the
suite runner's own cap). The case names are read from the
rocketnet_functional_tests table in functional/rocketnet_functional_tests.cpp;
the table holds rows for other platforms behind #if, and a case this build
does not hold answers "unknown functional test" with exit 2 on its plain run
(functional/rocketnet_functional_tests.cpp:8689),
so the matrix lists it as SKIP and runs no leaks on it.

Modes:
  --binary PATH [-- arg...]   run one binary once
  --matrix                    run test_lifecycle, functional and a 100000
                              iteration soak under each macOS transport pin
                              and with ROCKETNET_PUMP=portable on auto
  --self-test                 compile a scratch binary that leaks one
                              allocation and confirm the run turns red
"""

import argparse
import concurrent.futures
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
DEFAULT_JOBS = 8
FUNCTIONAL_TABLE = REPO_ROOT / "functional" / "rocketnet_functional_tests.cpp"
CASE_ROW = re.compile(r'^\s*\{\s*"([A-Za-z0-9_]+)"\s*,\s*[A-Za-z0-9_]+\s*\},')
NOT_DEBUGGABLE = "is not debuggable"
UNKNOWN_CASE = "unknown functional test"
ENTITLEMENTS = (
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
    '<plist version="1.0"><dict><key>com.apple.security.get-task-allow</key><true/></dict></plist>\n'
)
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


def make_debuggable(binary):
    # leaks needs the target debuggable to read its writable memory and stacks
    with tempfile.TemporaryDirectory(prefix="leaks-run-sign-") as tmp:
        plist = Path(tmp) / "get-task-allow.plist"
        plist.write_text(ENTITLEMENTS)
        sign = subprocess.run(["codesign", "-s", "-", "-f", "--entitlements", str(plist), str(binary)],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if sign.returncode != 0:
        sys.exit("leaks_run.py: codesign could not add get-task-allow to %s: %s"
                 % (binary, sign.stdout.decode("utf-8", "replace").strip()))


def run_leaks(binary, args, env_extra, timeout):
    lines = []
    ok = run_leaks_quiet(binary, args, env_extra, timeout, lines)
    sys.stdout.write("".join(lines))
    sys.stdout.flush()
    return ok


def run_group(command, env, timeout):
    # its own process group, so a timeout kills the command and every process it started
    proc = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        raw, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        raw, _ = proc.communicate()
        return None, raw.decode("utf-8", "replace")
    return proc.returncode, raw.decode("utf-8", "replace")


def run_leaks_quiet(binary, args, env_extra, timeout, lines, skip_unknown=False):
    def say(text):
        lines.append(text if text.endswith("\n") else text + "\n")
    env = dict(os.environ)
    for name in PIN_VARIABLES:
        env.pop(name, None)
    env.update(env_extra)
    name = " ".join([str(binary)] + list(args))
    say("leaks_run.py: %s" % " ".join("%s=%s" % item for item in env_extra.items()))
    # leaks returns its own status, so the target status comes from a plain run first
    status, output = run_group([str(binary)] + list(args), env, timeout)
    if status is None:
        say(output)
        say("leaks_run.py: FAIL %s: the plain run did not return within %.0f s; killed it" % (name, timeout))
        return False
    if skip_unknown and status == 2 and UNKNOWN_CASE in output:
        say("leaks_run.py: SKIP %s: this build holds no such case" % name)
        return True
    if status != 0:
        say(output)
        say("leaks_run.py: FAIL %s: binary exited %d on its plain run" % (name, status))
        return False
    env["MallocStackLogging"] = "1"
    command = [LEAKS, "--atExit", "--", str(binary)] + list(args)
    say("leaks_run.py: %s" % " ".join(command))
    status, output = run_group(command, env, timeout)
    say(output)
    if status is None:
        say("leaks_run.py: FAIL %s: leaks did not return within %.0f s; killed it and its target" % (name, timeout))
        return False
    if NOT_DEBUGGABLE in output:
        say("leaks_run.py: FAIL %s: the target is not debuggable, so leaks could not read its memory" % name)
        return False
    counts = leak_counts(output)
    leaked = sum(count for count, _ in counts)
    if not counts:
        say("leaks_run.py: FAIL %s: leaks printed no leak summary" % name)
        return False
    if leaked:
        say("leaks_run.py: FAIL %s: %d leak(s) across %d process report(s)" % (name, leaked, len(counts)))
        return False
    if status != 0:
        say("leaks_run.py: FAIL %s: leaks exited %d" % (name, status))
        return False
    say("leaks_run.py: PASS %s: 0 leaks" % name)
    return True


def binary_path(name):
    path = REPO_ROOT / "bin" / name
    if not path.is_file():
        sys.exit("leaks_run.py: missing binary %s (build Release without sanitizers first)" % path)
    make_debuggable(path)
    return path


def functional_cases():
    names = []
    inside = False
    for line in FUNCTIONAL_TABLE.read_text().splitlines():
        if "rocketnet_functional_tests[] =" in line:
            inside = True
            continue
        if inside and line.startswith("};"):
            break
        match = CASE_ROW.match(line) if inside else None
        if match:
            names.append(match.group(1))
    if not names:
        sys.exit("leaks_run.py: found no case rows in %s" % FUNCTIONAL_TABLE)
    return names


def run_functional(binary, pin, timeout, jobs):
    # each case in its own leaks run: the suite runner's children are out of leaks' sight
    cases = functional_cases()
    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {}
        for case in cases:
            lines = []
            futures[pool.submit(run_leaks_quiet, binary, ["--run", case], pin, timeout, lines, True)] = (case, lines)
        for future in concurrent.futures.as_completed(futures):
            case, lines = futures[future]
            if future.result():
                print(lines[-1].rstrip("\n"), flush=True)
            else:
                sys.stdout.write("".join(lines))
                sys.stdout.flush()
                failed.append(case)
    print("leaks_run.py: functional %d case(s), %d failed%s" % (len(cases), len(failed),
          (": " + " ".join(sorted(failed))) if failed else ""), flush=True)
    return not failed


def matrix(timeout, jobs):
    ok = True
    lifecycle = binary_path("test_lifecycle")
    functional = binary_path("functional")
    soak = binary_path("soak")
    for pin in PINS:
        label = " ".join("%s=%s" % item for item in pin.items())
        print("leaks_run.py: pin %s" % label, flush=True)
        ok = run_leaks(lifecycle, [], pin, timeout) and ok
        ok = run_functional(functional, pin, timeout, jobs) and ok
        ok = run_leaks(soak, ["100000"], pin, timeout) and ok
    return ok


def self_test(timeout):
    # scrub() overwrites the stack lose() used, so no stale copy of the lost
    # pointer is left for leaks to read as a reference
    source = (
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "static void *volatile held;\n"
        "__attribute__((noinline)) static void lose(void) {\n"
        "    char *p = (char *)malloc(32);\n"
        "    if (!p) return;\n"
        "    p[0] = 1;\n"
        "    held = p;\n"
        "}\n"
        "__attribute__((noinline)) static void scrub(void) {\n"
        "    volatile char pad[4096];\n"
        "    memset((char *)pad, 0, sizeof(pad));\n"
        "}\n"
        "int main(void) {\n"
        "    lose();\n"
        "    held = 0;\n"
        "    scrub();\n"
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
        make_debuggable(binary)
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
    parser.add_argument("--jobs", type=int, default=DEFAULT_JOBS,
                        help="functional cases run under leaks at once in --matrix (default 8)")
    parser.add_argument("args", nargs=argparse.REMAINDER, help="arguments for --binary after --")
    options = parser.parse_args()
    check_platform()
    check_developer_mode()
    if options.self_test:
        return 0 if self_test(options.timeout) else 1
    if options.matrix:
        return 0 if matrix(options.timeout, max(1, options.jobs)) else 1
    args = list(options.args)
    if args and args[0] == "--":
        args = args[1:]
    return 0 if run_leaks(binary_path(options.binary), args, {}, options.timeout) else 1


if __name__ == "__main__":
    sys.exit(main())
