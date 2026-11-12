#!/usr/bin/env python3
"""Run independent commands at once and fail when any one fails.

    python3 tools/run_parallel.py 'COMMAND' ['COMMAND' ...]
    python3 tools/run_parallel.py --self-test

Each COMMAND is one bash -eo pipefail script, started together with the
others in the current directory and environment. Their output is captured
and printed whole, one block per command in the order given, each headed by
the command and closed by its exit status and seconds, so a CI log reads as
if they had run one after another. The exit status is 1 when any command
exits nonzero (each red one is named on the last line), 2 when no command is
given; 0 only when every command ran and exited 0.

For a CI step whose commands share no state (two builds in their own
directories, a checker and its self-test): the step takes as long as its
slowest command instead of their sum. Commands that write the same files
must not be given together.
"""

import os
import subprocess
import sys
import tempfile
import time


def run(commands, out=sys.stdout):
    if not commands:
        out.write("run_parallel: no command given\n")
        return 2
    started = []
    for command in commands:
        log = tempfile.TemporaryFile()
        process = subprocess.Popen(["bash", "-eo", "pipefail", "-c", command], stdout=log, stderr=subprocess.STDOUT)
        started.append((command, process, log, time.monotonic()))
    red = []
    for index, (command, process, log, start) in enumerate(started):
        status = process.wait()
        seconds = time.monotonic() - start
        log.seek(0)
        text = log.read().decode("utf-8", "replace")
        log.close()
        out.write("::group::[%d] %s\n" % (index + 1, command))
        out.write(text)
        if text and not text.endswith("\n"):
            out.write("\n")
        out.write("::endgroup::\n")
        out.write("run_parallel: [%d] exit %d after %.1f s: %s\n" % (index + 1, status, seconds, command))
        if status != 0:
            red.append(index + 1)
    if red:
        out.write("run_parallel: FAIL, %d of %d red: %s\n" % (len(red), len(commands), " ".join("[%d]" % n for n in red)))
        return 1
    out.write("run_parallel: ok, %d of %d green\n" % (len(commands), len(commands)))
    return 0


class Capture:
    def __init__(self):
        self.text = ""

    def write(self, text):
        self.text += text


def self_test():
    failed = 0
    with tempfile.TemporaryDirectory() as directory:
        ready = os.path.join(directory, "ready")
        cases = [
            ("all green", ["echo one", "echo two"], 0, ["one", "two", "ok, 2 of 2 green"]),
            ("one red is named", ["true", "echo bad; exit 3", "true"], 1, ["exit 3", "FAIL, 1 of 3 red: [2]"]),
            ("a red pipe stage is red", ["false | true"], 1, ["FAIL, 1 of 1 red: [1]"]),
            ("no command", [], 2, ["no command given"]),
            # the first waits for a file only the second makes: run one after
            # another it would time out red; run together it is green
            ("they run together",
             ["for i in $(seq 1 200); do test -f %s && exit 0; sleep 0.05; done; exit 9" % ready, "touch %s" % ready],
             0, ["ok, 2 of 2 green"]),
            ("output kept in order", ["sleep 0.3; echo first", "echo second"], 0, ["first"]),
        ]
        for name, commands, expected, needles in cases:
            capture = Capture()
            got = run(commands, capture)
            missing = [n for n in needles if n not in capture.text]
            order_ok = name != "output kept in order" or capture.text.index("first") < capture.text.index("second")
            verdict = "ok" if got == expected and not missing and order_ok else "FAIL"
            if verdict != "ok":
                failed += 1
                sys.stdout.write(capture.text)
            print("self-test %-26s expected exit %d, got %d%s: %s" %
                  (name, expected, got, "" if not missing else ", missing %r" % missing, verdict))
    if failed:
        print("SELF-TEST FAILED %d" % failed)
        return 1
    print("SELF-TEST OK (%d cases)" % len(cases))
    return 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    if argv[1:2] in (["-h"], ["--help"]):
        print(__doc__)
        return 0
    return run(argv[1:])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
