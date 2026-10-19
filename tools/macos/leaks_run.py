#!/usr/bin/env python3
"""Run rocketnet binaries under the macOS leaks tool.

LeakSanitizer has no Apple runtime, so leaks are checked on a Release build
without sanitizers: every run sets MallocStackLogging=1 and goes through
    /usr/bin/leaks --atExit -- <binary> [args...]
A run fails when leaks reports a non-zero leak count or the binary exits
non-zero; the report carries the stacks leaks printed.

Modes:
  --binary PATH [-- arg...]   run one binary once
  --matrix                    run test_lifecycle, functional and a 100000
                              iteration soak under each macOS transport pin
  --self-test                 compile a scratch binary that leaks one
                              allocation and confirm the run turns red
"""

import argparse
import os
import platform
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
LEAKS = "/usr/bin/leaks"
PINS = (
    ("portable", "portable"),
    ("portable", "msgx"),
    ("msgx", "portable"),
    ("msgx", "msgx"),
)
LEAK_LINE = re.compile(r"Process\s+\d+:\s+(\d+)\s+leaks?\s+for\s+(\d+)\s+total leaked bytes")


def check_platform():
    if platform.system() != "Darwin":
        sys.exit("leaks_run.py: this script runs on macOS only (found %s)" % platform.system())
    if not os.access(LEAKS, os.X_OK):
        sys.exit("leaks_run.py: %s is missing or not executable" % LEAKS)


def leak_counts(text):
    counts = [(int(m.group(1)), int(m.group(2))) for m in LEAK_LINE.finditer(text)]
    return counts


def run_leaks(binary, args, env_extra):
    env = dict(os.environ)
    env["MallocStackLogging"] = "1"
    env.update(env_extra)
    command = [LEAKS, "--atExit", "--", str(binary)] + list(args)
    print("leaks_run.py: %s" % " ".join("%s=%s" % item for item in env_extra.items()), flush=True)
    print("leaks_run.py: %s" % " ".join(command), flush=True)
    proc = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = proc.stdout.decode("utf-8", "replace")
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


def matrix():
    ok = True
    for server, client in PINS:
        pin = {"ROCKETNET_SERVER_TRANSPORT": server, "ROCKETNET_CLIENT_TRANSPORT": client}
        label = "%s/%s" % (server, client)
        print("leaks_run.py: pin %s" % label, flush=True)
        ok = run_leaks(binary_path("test_lifecycle"), [], pin) and ok
        ok = run_leaks(binary_path("functional"), [], pin) and ok
        ok = run_leaks(binary_path("soak"), ["100000"], pin) and ok
    return ok


def self_test():
    source = (
        "#include <stdlib.h>\n"
        "int main(void) {\n"
        "    volatile char *p = (volatile char *)malloc(32);\n"
        "    if (!p) return 1;\n"
        "    p[0] = 1;\n"
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
        red = not run_leaks(binary, [], {})
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
    parser.add_argument("args", nargs=argparse.REMAINDER, help="arguments for --binary after --")
    options = parser.parse_args()
    check_platform()
    if options.self_test:
        return 0 if self_test() else 1
    if options.matrix:
        return 0 if matrix() else 1
    args = list(options.args)
    if args and args[0] == "--":
        args = args[1:]
    return 0 if run_leaks(binary_path(options.binary), args, {}) else 1


if __name__ == "__main__":
    sys.exit(main())
