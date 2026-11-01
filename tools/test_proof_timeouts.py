#!/usr/bin/env python3
"""Execution watchdog controls; proof domains and memory limits stay separate."""
import os
from pathlib import Path
import subprocess
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
WRAPPER = ROOT / "proofs/cbmc/run_bounded.sh"
TIMEOUT = ROOT / "proofs/cbmc/run_with_timeout.sh"


class ProofTimeoutTests(unittest.TestCase):
    def run_command(self, wrapper, args, nightly=False):
        env = os.environ.copy()
        env.pop("ROCKETNET_NIGHTLY_NO_TIMEOUT", None)
        if nightly:
            env["ROCKETNET_NIGHTLY_NO_TIMEOUT"] = "1"
        return subprocess.run(
            ["bash", str(wrapper), *args], env=env,
            text=True, capture_output=True,
            # The caller's nightly policy controls this outer watchdog; the
            # separate child environment still exercises both wrapper modes.
            timeout=None if os.environ.get("ROCKETNET_NIGHTLY_NO_TIMEOUT") == "1" else 20,
        )

    def test_default_watchdog_still_expires(self):
        got = self.run_command(WRAPPER, ["262144", "1", "sleep", "4"])
        self.assertEqual(got.returncode, 124, got)

    def test_nightly_exceeds_old_limit_and_keeps_exit(self):
        started = time.monotonic()
        got = self.run_command(
            WRAPPER, ["262144", "1", "bash", "-c", "sleep 2; exit 7"], nightly=True
        )
        self.assertEqual(got.returncode, 7, got)
        self.assertGreaterEqual(time.monotonic() - started, 2)

    def test_nightly_memory_limit_remains(self):
        # Linux rejects allocation under RLIMIT_AS; Darwin's existing RSS
        # monitor terminates the live process after it touches the allocation.
        got = self.run_command(
            WRAPPER,
            ["131072", "1", sys.executable, "-c",
             "import time; allocation = bytearray(256 * 1024 * 1024); time.sleep(8)"],
            nightly=True,
        )
        self.assertNotEqual(got.returncode, 0, got)
        self.assertNotEqual(got.returncode, 124, got)
        if sys.platform == "darwin":
            self.assertEqual(got.returncode, 137, got)
            self.assertIn("out of memory", got.stdout)

    def test_generic_nightly_keeps_exit(self):
        got = self.run_command(
            TIMEOUT, ["1", "bash", "-c", "sleep 2; exit 9"], nightly=True
        )
        self.assertEqual(got.returncode, 9, got)

    @unittest.skipUnless(sys.platform.startswith("linux"), "GNU timeout path is Linux")
    def test_generic_default_watchdog(self):
        got = self.run_command(TIMEOUT, ["1", "sleep", "4"])
        self.assertEqual(got.returncode, 124, got)


if __name__ == "__main__":
    unittest.main()
