#!/usr/bin/env python3
"""
Unit tests for tools/check_frame_budget.py covering the four required cases:
1. Median <= 100.0 passes (exit 0, verdict=PASS, prints 3 raw lines + verdict).
2. Median > 100.0 fails naming the value (exit 1, verdict=FAIL; exit 0 with --report-only).
3. Missing or malformed P=32 / packets_per_frame=32 line fails (exit 1, verdict=FAIL).
4. Load > cores / 4 voids before any of the 3 runs and is never a pass (exit 3, verdict=VOID;
   and Windows load=unavailable does not void).
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_frame_budget as cfb


def make_stdout(p32_total_p99: float, transport: str = "portable") -> str:
    return (
        f"client_main_thread_cost transport={transport} packets_per_frame=32 frames=2000 "
        f"send_us_p50=18.200 send_us_p99=34.500 send_us_max=51.000 "
        f"update_us_p50=12.400 update_us_p99=24.100 update_us_max=40.000 "
        f"total_us_p50=30.600 total_us_p99={p32_total_p99:.3f} total_us_max=91.000 "
        f"received=64000\n"
    )


class TestCheckFrameBudget(unittest.TestCase):
    def test_median_at_or_below_100_passes(self) -> None:
        outputs = [make_stdout(62.4), make_stdout(58.1), make_stdout(65.9)]
        code, report = cfb.evaluate_gate(
            transport="portable",
            run_outputs=outputs,
            load1=1.0,
            cores=16,
        )
        self.assertEqual(code, 0, msg=report)
        lines = report.splitlines()
        self.assertEqual(len(lines), 4, msg=report)
        self.assertIn("packets_per_frame=32", lines[0])
        self.assertIn("packets_per_frame=32", lines[1])
        self.assertIn("packets_per_frame=32", lines[2])
        self.assertIn("total_us_p99_median=62.40", lines[3])
        self.assertIn("runs=62.40,58.10,65.90", lines[3])
        self.assertIn("verdict=PASS", lines[3])

        # Exact boundary 100.0 also passes:
        outputs_boundary = [make_stdout(100.0), make_stdout(95.0), make_stdout(112.0)]
        code_b, report_b = cfb.evaluate_gate(
            transport="portable",
            run_outputs=outputs_boundary,
            load1=4.0,
            cores=16,
        )
        self.assertEqual(code_b, 0, msg=report_b)
        self.assertIn("total_us_p99_median=100.00", report_b)
        self.assertIn("verdict=PASS", report_b)

    def test_median_above_100_fails_naming_value(self) -> None:
        outputs = [make_stdout(101.25), make_stdout(118.40), make_stdout(92.00)]
        code, report = cfb.evaluate_gate(
            transport="portable",
            run_outputs=outputs,
            load1=1.5,
            cores=16,
        )
        self.assertEqual(code, 1, msg=report)
        self.assertIn("total_us_p99_median=101.25", report)
        self.assertIn("gate=100", report)
        self.assertIn("verdict=FAIL", report)

        # In report-only mode, verdict stays FAIL and names the value, exit is 0:
        code_ro, report_ro = cfb.evaluate_gate(
            transport="msgx",
            run_outputs=outputs,
            load1=1.5,
            cores=16,
            report_only=True,
        )
        self.assertEqual(code_ro, 0, msg=report_ro)
        self.assertIn("total_us_p99_median=101.25", report_ro)
        self.assertIn("verdict=FAIL", report_ro)
        self.assertNotIn("verdict=PASS", report_ro)

    def test_malformed_or_missing_line_fails(self) -> None:
        missing_p32 = (
            "client_main_thread_cost transport=portable packets_per_frame=1 frames=2000 "
            "total_us_p50=3.100 total_us_p99=6.200 total_us_max=10.000 received=2000\n"
        )
        code_missing, report_missing = cfb.evaluate_gate(
            transport="portable",
            run_outputs=[make_stdout(50.0), missing_p32, make_stdout(52.0)],
            load1=0.5,
            cores=16,
        )
        self.assertEqual(code_missing, 1, msg=report_missing)
        self.assertIn("verdict=FAIL", report_missing)
        self.assertNotIn("verdict=PASS", report_missing)

        malformed_p32 = "client_main_thread_cost transport=portable packets_per_frame=32 frames=2000 total_us_p99=notanumber\n"
        code_malformed, report_malformed = cfb.evaluate_gate(
            transport="portable",
            run_outputs=[malformed_p32, make_stdout(50.0), make_stdout(52.0)],
            load1=0.5,
            cores=16,
        )
        self.assertEqual(code_malformed, 1, msg=report_malformed)
        self.assertIn("verdict=FAIL", report_malformed)
        self.assertNotIn("verdict=PASS", report_malformed)

    def test_load_above_quarter_cores_voids_and_is_never_pass(self) -> None:
        outputs = [make_stdout(40.0), make_stdout(41.0), make_stdout(42.0)]
        # 16 cores -> threshold is 4.0; load 4.25 exceeds cores/4 on run 1
        code, report = cfb.evaluate_gate(
            transport="portable",
            run_outputs=outputs,
            load1=4.25,
            cores=16,
        )
        self.assertEqual(code, 3, msg=report)
        self.assertIn("VOID load=4.25 cores=16", report)
        self.assertIn("verdict=VOID", report)
        self.assertNotIn("verdict=PASS", report)

        # Load spike before run 2 also voids:
        code_r2, report_r2 = cfb.evaluate_gate(
            transport="portable",
            run_outputs=outputs,
            load1=1.0,
            cores=16,
            load_samples=[(1.0, 16), (5.50, 16), (1.0, 16)],
        )
        self.assertEqual(code_r2, 3, msg=report_r2)
        self.assertIn("VOID load=5.50 cores=16", report_r2)
        self.assertIn("verdict=VOID", report_r2)
        self.assertNotIn("verdict=PASS", report_r2)

        # Even in --report-only mode, a void is NEVER reported as PASS:
        code_ro, report_ro = cfb.evaluate_gate(
            transport="portable",
            run_outputs=outputs,
            load1=4.25,
            cores=16,
            report_only=True,
        )
        self.assertEqual(code_ro, 0, msg=report_ro)
        self.assertIn("VOID load=4.25 cores=16", report_ro)
        self.assertIn("verdict=VOID", report_ro)
        self.assertNotIn("verdict=PASS", report_ro)

        # Windows (load1=None) prints load=unavailable and does not void:
        code_win, report_win = cfb.evaluate_gate(
            transport="portable",
            run_outputs=outputs,
            load1=None,
            cores=16,
            report_only=True,
        )
        self.assertEqual(code_win, 0, msg=report_win)
        self.assertIn("load=unavailable", report_win)
        self.assertIn("verdict=PASS", report_win)

    def test_run_benchmark_with_load_checks_delegates_to_evaluate_gate(self) -> None:
        outputs = [make_stdout(45.0), make_stdout(55.0), make_stdout(50.0)]
        calls = []

        def fake_run(binary: str, transport: str):
            idx = len(calls)
            calls.append((binary, transport))
            return 0, outputs[idx], ""

        code, report = cfb.run_benchmark_with_load_checks(
            binary="/fake/client_main_thread_cost",
            transport="portable",
            load_fn=lambda: (1.0, 16),
            run_fn=fake_run,
        )
        self.assertEqual(code, 0, msg=report)
        self.assertEqual(len(calls), 3)
        self.assertIn("total_us_p99_median=50.00", report)
        self.assertIn("verdict=PASS", report)

        # Load spike before run 2 stops early without invoking run 2 or 3:
        loads = iter([(1.0, 16), (6.0, 16), (1.0, 16)])
        spike_calls = []

        def fake_run_spike(binary: str, transport: str):
            spike_calls.append((binary, transport))
            return 0, make_stdout(45.0), ""

        code_void, report_void = cfb.run_benchmark_with_load_checks(
            binary="/fake/client_main_thread_cost",
            transport="portable",
            load_fn=lambda: next(loads),
            run_fn=fake_run_spike,
        )
        self.assertEqual(code_void, 3, msg=report_void)
        self.assertEqual(len(spike_calls), 1)
        self.assertIn("VOID load=6.00 cores=16", report_void)
        self.assertIn("verdict=VOID", report_void)

        # Non-zero exit from binary fails immediately and names the exit status:
        err_calls = []

        def fake_run_err(binary: str, transport: str):
            err_calls.append((binary, transport))
            return 2, "", "bind failed"

        code_err, report_err = cfb.run_benchmark_with_load_checks(
            binary="/fake/client_main_thread_cost",
            transport="portable",
            load_fn=lambda: (1.0, 16),
            run_fn=fake_run_err,
        )
        self.assertEqual(code_err, 1, msg=report_err)
        self.assertEqual(len(err_calls), 1)
        self.assertIn("run 1/3 exited 2: bind failed", report_err)


if __name__ == "__main__":
    unittest.main()
