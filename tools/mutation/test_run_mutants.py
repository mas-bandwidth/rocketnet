#!/usr/bin/env python3
"""Controls for run_mutants.py's process cleanup: no test process outlives a run.

A fake ./bin/test stands in for the suite runner: it starts a job in a process
group of its own (POSIX_SPAWN_SETPGROUP, as tests/suite_runner.h does) and
sleeps far past the deadline. Real processes, bounded waits, a few seconds in
all; macOS and Linux.

And the run's own judgement: a run that judged nothing fails. An empty
selection and a run whose every mutant did not compile each fail with the
nightly's arguments (--platform linux --baseline tools/mutation/survivors.tsv),
in main() and in --rejudge; the same run with its mutants detected passes. The
workers are stubbed (no build): the stub sets each mutant's status as a build
or test would.
"""
import contextlib
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_mutants  # noqa: E402

FAKE_TEST = r'''
import os, sys, time
mode, pid_file = sys.argv[1], sys.argv[2]
pids = [os.getpid()]
if mode != "sleep":
    quiet = [(os.POSIX_SPAWN_OPEN, fd, os.devnull, os.O_WRONLY, 0) for fd in (1, 2)]
    pids.append(os.posix_spawn(sys.executable, [sys.executable, "-c", "import time; time.sleep(300)"],
                               os.environ, file_actions=quiet, setpgroup=0))
with open(pid_file + ".tmp", "w") as f:
    f.write(" ".join(map(str, pids)))
os.replace(pid_file + ".tmp", pid_file)
if mode == "exit":
    sys.exit(0)
time.sleep(300)
'''

RUNNER = r'''
import sys, threading
sys.path.insert(0, sys.argv[1])
import run_mutants
run_mutants.install_signal_cleanup()
worker = threading.Thread(target=run_mutants.run, args=(sys.argv[2:], sys.argv[1], 300))
worker.start()
worker.join()
'''


def alive(pid):
    """True while pid is a process that is not a zombie."""
    stat = subprocess.run(["ps", "-o", "stat=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    return bool(stat) and not stat.startswith("Z")


def wait_for(predicate, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if predicate():
            return True
        time.sleep(0.05)
    return predicate()


class ProcessCleanupTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        (self.root / "fake_test.py").write_text(FAKE_TEST)
        self.pid_file = self.root / "pids"
        self.pids = []

    def tearDown(self):
        for pid in self.pids:
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass
        self.dir.cleanup()

    def command(self, mode):
        return [sys.executable, str(self.root / "fake_test.py"), mode, str(self.pid_file)]

    def read_pids(self):
        self.assertTrue(wait_for(self.pid_file.exists, 10), "the fake test never started")
        pids = [int(p) for p in self.pid_file.read_text().split()]
        self.pids += pids
        return pids

    def assert_none_left(self):
        pids = self.read_pids()
        self.assertTrue(wait_for(lambda: not any(alive(p) for p in pids), 10),
                        f"left running: {[p for p in pids if alive(p)]}")

    def test_deadline_kills_a_hung_test(self):
        code, _, _ = run_mutants.run(self.command("sleep"), self.root, 1)
        self.assertIsNone(code)  # Worker.mutant counts None as timed_out
        self.assert_none_left()

    def test_deadline_kills_jobs_in_their_own_process_groups(self):
        code, _, _ = run_mutants.run(self.command("spawn"), self.root, 1)
        self.assertIsNone(code)
        self.assert_none_left()

    def test_a_job_left_behind_by_an_exited_test_is_killed(self):
        code, _, _ = run_mutants.run(self.command("exit"), self.root, 60)
        self.assertEqual(code, 0)
        self.assert_none_left()

    def test_a_signal_to_the_tool_kills_every_test_process(self):
        tools = str(Path(__file__).resolve().parent)
        for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            with self.subTest(signal=signum.name):
                if self.pid_file.exists():
                    self.pid_file.unlink()
                tool = subprocess.Popen([sys.executable, "-c", RUNNER, tools] + self.command("spawn"),
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                try:
                    self.read_pids()
                    tool.send_signal(signum)
                    code = tool.wait(timeout=30)
                finally:
                    if tool.poll() is None:
                        tool.kill()
                        tool.wait(timeout=10)
                self.assert_none_left()
                self.assertEqual(code, 128 + signum)


# a source the Linux build compiles, with a few mutants and no baseline entries
SMALL = "sdk/rocketnet_hash.cpp"
# a source with no mutable lines
EMPTY = "sdk/rocketnet_hash.h"
BASELINE = str(run_mutants.ROOT / "tools" / "mutation" / "survivors.tsv")


def stub_worker(status):
    class Stub:
        def __init__(self, index, args, files):
            pass

        def build(self):
            return 0, 0.0, ""

        def test(self, timeout):
            return 0, 1.0, ""

        def mutant(self, mutant, timeout):
            mutant["status"] = status
    return Stub


class JudgementTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.out = Path(self.dir.name) / "results.json"
        self.saved = run_mutants.Worker

    def tearDown(self):
        run_mutants.Worker = self.saved
        self.dir.cleanup()

    def main(self, status, files, baseline=True):
        run_mutants.Worker = stub_worker(status)
        argv = ["run_mutants.py", "--max-mutants", "0", "--workers", "1", "--work-dir", self.dir.name,
                "--out", str(self.out), "--files"] + files
        if baseline:
            argv[1:1] = ["--platform", "linux", "--baseline", BASELINE]
        text = io.StringIO()
        saved_argv = sys.argv
        sys.argv = argv
        try:
            with contextlib.redirect_stdout(text):
                code = run_mutants.main()
        finally:
            sys.argv = saved_argv
        return code, text.getvalue()

    def test_an_empty_selection_fails(self):
        self.assertEqual(run_mutants.generate(EMPTY, (run_mutants.ROOT / EMPTY).read_text()), [])
        for baseline in (True, False):
            with self.subTest(baseline=baseline):
                code, text = self.main("detected", [EMPTY], baseline)
                self.assertEqual(code, 1, text)
                self.assertIn("mutation run judged 0 mutants (0 chosen, 0 did not compile)", text)

    def test_every_mutant_not_compiling_fails(self):
        for baseline in (True, False):
            with self.subTest(baseline=baseline):
                code, text = self.main("did_not_compile", [SMALL], baseline)
                self.assertEqual(code, 1, text)
                self.assertIn("mutation run judged 0 mutants (7 chosen, 7 did not compile)", text)
                self.assertIn("7 of 7 chosen mutants did not compile (100.0%), over the 28% bound", text)

    def test_a_judged_run_passes_and_prints_its_share(self):
        for baseline in (True, False):
            with self.subTest(baseline=baseline):
                code, text = self.main("detected", [SMALL], baseline)
                self.assertEqual(code, 0, text)
                self.assertIn("MUTATION-DID-NOT-COMPILE 0 of 7 chosen (0.0%; bound 28%); judged 7", text)

    def test_the_share_bound(self):
        def run_of(failed, judged):
            return [{"status": "did_not_compile"}] * failed + [{"status": "detected"}] * judged
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_mutants.judgement_errors(run_of(28, 72)), [])
            self.assertEqual(len(run_mutants.judgement_errors(run_of(29, 71))), 1)
            self.assertEqual(len(run_mutants.judgement_errors(run_of(0, 0))), 1)
            self.assertEqual(run_mutants.judgement_errors([{"status": "survived"}, {"status": "not_run"}]), [])

    def rejudge(self, status):
        mutants = [dict(m, status=status) for m in run_mutants.generate(SMALL, (run_mutants.ROOT / SMALL).read_text())]
        self.out.write_text(json.dumps({"head": "test", "files": [SMALL], "mutants": mutants}))
        text = io.StringIO()
        with contextlib.redirect_stdout(text):
            code = run_mutants.rejudge(str(self.out), BASELINE)
        return code, text.getvalue()

    def test_rejudge_applies_the_same_rule(self):
        code, text = self.rejudge("did_not_compile")
        self.assertEqual(code, 1, text)
        self.assertIn("FAILING: mutation run judged 0 mutants (7 chosen, 7 did not compile)", text)
        self.assertIn("over the 28% bound", text)
        code, text = self.rejudge("detected")
        self.assertEqual(code, 0, text)
        self.assertIn("MUTATION-DID-NOT-COMPILE 0 of 7 chosen", text)


if __name__ == "__main__":
    unittest.main()
