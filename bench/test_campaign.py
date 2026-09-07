#!/usr/bin/env python3
"""Deterministic checks for campaign result and process-status handling."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from check_windows import validate

HERE = Path(__file__).resolve().parent


def window(number=1, rate=100000):
    return (f"WINDOW {number} secs=5.000 pkts={rate * 5} pps={rate} bytes={rate * 6000} "
            "p50_ns=1 p99_ns=2 syscalls=1 empty=0 timed=1 timed_pkts=1 fold=ff\n")


class CampaignTests(unittest.TestCase):
    def test_window_contract(self):
        self.assertEqual(validate(window(), 1, 100000), (100000, 100000, 100000))
        for text in ("", window() * 2, window().replace("secs=5.000", "secs=nan"),
                     window().replace("pps=100000", "pps=0"), window().replace("fold=ff", ""),
                     window().replace("pkts=500000", "pkts=1")):
            with self.assertRaises(ValueError):
                validate(text, 1, 100000)
        with self.assertRaises(ValueError):
            validate(window(rate=50000), 1, 100000)
        self.assertEqual(validate(window(1, 50000) + window(2, 100000), 2), (75000, 50000, 100000))

    def test_pair_statuses(self):
        with tempfile.TemporaryDirectory(dir=HERE, prefix=".campaign-test-") as directory:
            root = Path(directory)
            for name in ("pair_common.sh", "run_pair.sh", "run_pair_darwin.sh", "smoke_pair.sh"):
                shutil.copyfile(HERE / name, root / name)
            (root / "fake").write_text('#!/bin/sh\ncase "$1" in recv) exit "${RECV_STATUS:-0}";; send) exit "${SEND_STATUS:-0}";; esac\n')
            (root / "fake").chmod(0o755)
            (root / "sleep").write_text("#!/bin/sh\nexit 0\n")
            (root / "sleep").chmod(0o755)
            for runner in ("run_pair.sh", "run_pair_darwin.sh", "smoke_pair.sh"):
                for recv, send in ((0, 0), (7, 0), (0, 9)):
                    env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ["PATH"],
                               RECV_STATUS=str(recv), SEND_STATUS=str(send))
                    result = subprocess.run(["sh", runner, "fake", "40000", "", "", ""],
                                            cwd=root, env=env, capture_output=True, timeout=5)
                    self.assertEqual(result.returncode, recv or send, result.stderr)

    def test_campaign_stops_before_measurement(self):
        with tempfile.TemporaryDirectory(dir=HERE, prefix=".campaign-test-") as directory:
            root = Path(directory)
            for name in ("run_all.sh", "check_windows.py"):
                shutil.copyfile(HERE / name, root / name)
            (root / "bench_portable").write_text("#!/bin/sh\nexit 0\n")
            (root / "bench_portable").chmod(0o755)
            control = '''#!/bin/sh
case "${CASE:-}" in process) exit 7;; empty) exit 0;; esac
rate=${5#pace=}
if [ "${CASE:-}" = badrate ]; then rate=1; fi
echo "WINDOW 1 secs=5.000 pkts=$((rate*5)) pps=$rate bytes=$((rate*6000)) p50_ns=1 p99_ns=2 syscalls=1 empty=0 timed=1 timed_pkts=1 fold=ff"
'''
            (root / "smoke_pair.sh").write_text(control)
            (root / "smoke_pair.sh").chmod(0o755)
            matrix = '#!/bin/sh\ntouch matrix-ran\nif [ "${CASE:-}" = matrix ]; then exit 9; fi\n'
            matrix += "printf '%s' '" + "".join(window(i) for i in range(1, 6)) + "'\n"
            for name in ("run_pair.sh", "run_pair_darwin.sh"):
                (root / name).write_text(matrix)
                (root / name).chmod(0o755)
            for case in ("process", "empty", "badrate", "matrix", "success"):
                marker = root / "matrix-ran"
                marker.unlink(missing_ok=True)
                result = subprocess.run(["sh", "run_all.sh", "results"], cwd=root,
                                        env=dict(os.environ, CASE=case), capture_output=True, timeout=10)
                self.assertEqual(result.returncode == 0, case == "success", result.stderr)
                self.assertEqual(marker.exists(), case in ("matrix", "success"))


if __name__ == "__main__":
    unittest.main()
