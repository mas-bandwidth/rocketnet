#!/usr/bin/env python3
"""Controls for run_mutants.py's process cleanup: no test process outlives a run.

A fake ./bin/test stands in for the suite runner: it starts a job in a process
group of its own (POSIX_SPAWN_SETPGROUP, as tests/suite_runner.h does) and
sleeps far past the deadline. Real processes, bounded waits, a few seconds in
all; macOS and Linux.
"""
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


if __name__ == "__main__":
    unittest.main()
