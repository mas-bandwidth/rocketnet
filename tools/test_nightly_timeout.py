#!/usr/bin/env python3
"""Controls for the shared nightly execution opt-in; no protocol work."""
import os
from pathlib import Path
import subprocess
import sys
import textwrap
import tempfile
import unittest
from unittest import mock
from types import SimpleNamespace

import nightly_timeout as policy
import run_examples
import run_backend_examples
import check_frame_budget
import check_test_environment
from macos import soak_matrix


class NightlyTimeoutTests(unittest.TestCase):
    def test_only_exact_opt_in_disables_execution_cap(self):
        for value in ("", "0", "true", "01", "1"):
            with self.subTest(value=value), mock.patch.dict(os.environ, {"ROCKETNET_NIGHTLY_NO_TIMEOUT": value}):
                self.assertEqual(policy.execution_timeout(120), None if value == "1" else 120)
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(policy.execution_timeout(120), 120)

    def test_real_process_default_expires_and_opt_in_keeps_failure(self):
        command = [sys.executable, "-c", "import time; time.sleep(.05); raise SystemExit(7)"]
        with mock.patch.dict(os.environ, {"ROCKETNET_NIGHTLY_NO_TIMEOUT": "0"}):
            with self.assertRaises(subprocess.TimeoutExpired):
                subprocess.run(command, timeout=policy.execution_timeout(.001), check=False)
        with mock.patch.dict(os.environ, {"ROCKETNET_NIGHTLY_NO_TIMEOUT": "1"}):
            result = subprocess.run(command, timeout=policy.execution_timeout(.001), check=False)
            self.assertEqual(result.returncode, 7)

    def test_embedded_macos_leaks_watchdog_and_verdict(self):
        workflow = (Path(__file__).resolve().parents[1] / ".github/workflows/nightly-macos-sanitizers.yml").read_text()
        start = workflow.index("          one() {\n")
        end = workflow.index("          export -f one;", start)
        function = textwrap.dedent(workflow[start:end])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            leaks = root / "leaks"
            leaks.write_text("#!/bin/sh\nsleep .15\ncase $LEAKS_CONTROL in\nclean) echo 'Process 123: 0 leaks for 0 total leaked bytes';;\nleak) echo 'Process 123: 1 leak for 8 total leaked bytes';;\nmissing) echo 'inspection failed';;\nesac\n")
            leaks.chmod(0o755)
            script = root / "control.sh"
            script.write_text('set -u\nbin=fixture; logs="$1"; run=""; limit=1\nsleep() { command sleep .01; }\n' + function + '\none test_control\n')
            for opt_in, verdict, expected in (("", "clean", 1), ("0", "clean", 1), ("true", "clean", 1), ("1", "clean", 0), ("1", "leak", 1), ("1", "missing", 1)):
                with self.subTest(opt_in=opt_in, verdict=verdict):
                    env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ["PATH"], ROCKETNET_NIGHTLY_NO_TIMEOUT=opt_in, LEAKS_CONTROL=verdict)
                    result = subprocess.run(["/bin/bash", str(script), str(root)], env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
                    self.assertEqual(result.returncode, expected, result.stdout)
                    self.assertEqual("TIMEOUT test_control" in result.stdout, opt_in != "1", result.stdout)

    def test_example_subprocess_cap_and_original_oracle(self):
        for value, expected in (("0", 120), ("1", None)):
            with mock.patch.dict(os.environ, {"ROCKETNET_NIGHTLY_NO_TIMEOUT": value}), mock.patch.object(run_examples.subprocess, "run") as run:
                run.return_value = subprocess.CompletedProcess([], 9, "", "failure")
                result = run_examples.run_single("config_env", Path("/fixture"))
                self.assertEqual(run.call_args.kwargs["timeout"], expected)
                self.assertFalse(run_examples.check_result("control", result))

    def test_real_example_wrapper_preserves_exit_and_output_checks(self):
        command = ["-c", "import time; time.sleep(.05); print('PASS'); raise SystemExit(7)"]
        with mock.patch.object(run_examples, "TIMEOUT_SECONDS", .001):
            with mock.patch.dict(os.environ, {"ROCKETNET_NIGHTLY_NO_TIMEOUT": "0"}):
                self.assertIsNone(run_examples.run_single("control", Path(sys.executable), command))
            with mock.patch.dict(os.environ, {"ROCKETNET_NIGHTLY_NO_TIMEOUT": "1"}):
                result = run_examples.run_single("control", Path(sys.executable), command)
                self.assertEqual(result.returncode, 7)
                self.assertFalse(run_examples.check_result("control", result))
                missing = run_examples.run_single("control", Path(sys.executable), ["-c", "print('not a verdict')"])
                self.assertFalse(run_examples.check_result("control", missing))

    def test_macos_child_discovery_exact_opt_in_and_parent_exit(self):
        parent = mock.Mock(pid=123)
        parent.poll.return_value = None
        for value in ("0", "true", "1"):
            with self.subTest(value=value), mock.patch.dict(os.environ, {"ROCKETNET_NIGHTLY_NO_TIMEOUT": value}), mock.patch.object(soak_matrix.time, "monotonic", return_value=31), mock.patch.object(soak_matrix.time, "sleep"), mock.patch.object(soak_matrix.subprocess, "run") as run:
                run.side_effect = [subprocess.CompletedProcess([], 0, ""), subprocess.CompletedProcess([], 0, "456")]
                self.assertEqual(soak_matrix.child_pid(parent, 30), 456 if value == "1" else None)
                self.assertEqual(run.call_count, 2 if value == "1" else 0)
        parent.poll.return_value = 7
        with mock.patch.dict(os.environ, {"ROCKETNET_NIGHTLY_NO_TIMEOUT": "1"}), mock.patch.object(soak_matrix.subprocess, "run") as run:
            self.assertIsNone(soak_matrix.child_pid(parent, 30))
            run.assert_not_called()

    def test_macos_sampling_and_leaks_caps_but_not_verdicts(self):
        for value in ("0", "true", "1"):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as directory, mock.patch.dict(os.environ, {"ROCKETNET_NIGHTLY_NO_TIMEOUT": value}), mock.patch.object(soak_matrix.subprocess, "Popen") as spawn, mock.patch.object(soak_matrix, "child_pid", return_value=456), mock.patch.object(soak_matrix.resource_watch, "watch", return_value=[]) as watch, mock.patch.object(soak_matrix.resource_watch, "judge", return_value=("INSUFFICIENT", [])):
                args = SimpleNamespace(out=directory, soak="/fixture", seconds=1800, interval=5, leaks_timeout=600)
                spawn.return_value.wait.return_value = 0
                row = soak_matrix.run_cell(args, "control", "portable", "portable", None, False)
                self.assertEqual(watch.call_args.kwargs["stop_after"], None if value == "1" else 2100)
                self.assertTrue(callable(watch.call_args.kwargs["until"]))
                self.assertFalse(watch.call_args.kwargs["until"]())
                Path(directory, "control.log").write_text("\nsoak finished: fixture\n")
                self.assertTrue(watch.call_args.kwargs["until"]())
                spawn.return_value.wait.assert_called_once_with(timeout=None if value == "1" else 600)
                self.assertFalse(row["pass"])

    def test_example_readiness_default_and_opt_in(self):
        process = mock.Mock()
        process.poll.return_value = None
        for value, expected in (("0", "timeout"), ("1", "ready")):
            with mock.patch.dict(os.environ, {"ROCKETNET_NIGHTLY_NO_TIMEOUT": value}), mock.patch.object(run_examples.time, "monotonic", side_effect=[0, 31]), mock.patch.object(run_examples.time, "sleep"), mock.patch.object(run_examples, "keys_written", side_effect=[False, True]):
                self.assertEqual(run_examples.wait_server_ready("control", process, None, ""), expected)
        process.poll.return_value = 2
        with mock.patch.dict(os.environ, {"ROCKETNET_NIGHTLY_NO_TIMEOUT": "1"}), mock.patch.object(run_examples, "keys_written", return_value=False):
            self.assertEqual(run_examples.wait_server_ready("control", process, None, ""), "exited")

    def test_backend_readiness_default_and_opt_in(self):
        process = mock.Mock()
        process.poll.return_value = None
        for value, expected in (("0", None), ("1", "ready")):
            with mock.patch.dict(os.environ, {"ROCKETNET_NIGHTLY_NO_TIMEOUT": value}), mock.patch.object(run_backend_examples.time, "monotonic", side_effect=[0, 31]), mock.patch.object(run_backend_examples.time, "sleep"):
                condition = mock.Mock(side_effect=[None, "ready"])
                self.assertEqual(run_backend_examples.wait_for(condition, process, 30), expected)

    def test_frame_budget_actual_runner_cap_and_performance_oracle(self):
        stdout = "packets_per_frame=32 total_us_p99=101\n"
        for value, expected in (("0", 300), ("1", None)):
            with mock.patch.dict(os.environ, {"ROCKETNET_NIGHTLY_NO_TIMEOUT": value}), mock.patch.object(check_frame_budget.subprocess, "run") as run:
                run.return_value = subprocess.CompletedProcess([], 0, stdout, "")
                code, report = check_frame_budget.run_benchmark_with_load_checks("/fixture", "portable", load_fn=lambda: (0, 8))
                self.assertEqual(code, 1)
                self.assertIn("verdict=FAIL", report)
                self.assertEqual(len(run.call_args_list), 3)
                self.assertTrue(all(call.kwargs["timeout"] == expected for call in run.call_args_list))

    def test_environment_checker_preserves_child_status(self):
        for value, expected in (("0", 180), ("1", None)):
            with mock.patch.dict(os.environ, {"ROCKETNET_NIGHTLY_NO_TIMEOUT": value}), mock.patch.object(check_test_environment.subprocess, "run") as run:
                run.return_value = subprocess.CompletedProcess([], 17, "")
                result = check_test_environment.run_binary("/fixture", {}, 180)
                self.assertEqual(result.returncode, 17)
                self.assertEqual(run.call_args.kwargs["timeout"], expected)


if __name__ == "__main__":
    unittest.main()
