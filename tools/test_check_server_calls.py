#!/usr/bin/env python3
"""Unit cases for tools/check_server_calls.py: parsing, refusals and verdicts."""

import io
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_server_calls as check  # noqa: E402


def log(total=1000, p99=None, drop=None, extra="", transport="gso", run_id="0123456789abcdef"):
    p99 = p99 or {}
    lines = ["split_ready: role=server"]
    for call in check.CALLS:
        if call == drop:
            continue
        value = p99.get(call, 10)
        lines.append(f"split_server_call: run_id={run_id} total={total} transport={transport} "
                     f"scope=traffic_plus_drain call={call} count=100 p50_upper_us=1 "
                     f"p99_upper_us={value} max_us=200.000 overbudget_count=1 budget_us=100")
    return "\n".join(lines) + "\n" + extra


class ServerCallsTest(unittest.TestCase):
    def test_all_calls_under_budget_pass(self):
        out = io.StringIO()
        self.assertEqual(check.report([("a", log())], out), [])
        self.assertIn("1000 gso server_update 1 10 200.000 100 PASS", out.getvalue())

    def test_budget_is_inclusive(self):
        self.assertEqual(check.report([("a", log(p99={"server_update": 100}))], io.StringIO()), [])

    def test_one_call_over_budget_fails(self):
        over = check.report([("a", log(p99={"server_send_packets": 101}))], io.StringIO())
        self.assertEqual(over, [(1000, "gso", "server_send_packets", 101)])

    def test_rows_sort_by_population(self):
        out = io.StringIO()
        check.report([("b", log(total=1000)), ("a", log(total=10))], out)
        rows = out.getvalue().splitlines()[1:]
        self.assertTrue(rows[0].startswith("10 "))
        self.assertTrue(rows[-1].startswith("1000 "))

    def test_missing_call_is_invalid(self):
        with self.assertRaisesRegex(ValueError, "missing calls: server_finish_message"):
            check.report([("a", log(drop="server_finish_message"))], io.StringIO())

    def test_duplicated_call_is_invalid(self):
        text = log()
        duplicate = [line for line in text.splitlines() if "call=server_update" in line][0]
        with self.assertRaisesRegex(ValueError, "duplicated"):
            check.report([("a", text + duplicate + "\n")], io.StringIO())

    def test_mixed_run_is_invalid(self):
        text = log()
        other = log(run_id="fedcba9876543210").splitlines()
        lines = text.splitlines()
        lines[1] = [line for line in other if "call=server_update" in line][0]
        with self.assertRaisesRegex(ValueError, "mixed"):
            check.report([("a", "\n".join(lines))], io.StringIO())

    def test_contradicting_quantiles_are_invalid(self):
        text = log().replace("p50_upper_us=1 p99_upper_us=10", "p50_upper_us=11 p99_upper_us=10", 1)
        with self.assertRaisesRegex(ValueError, "contradict"):
            check.report([("a", text)], io.StringIO())

    def test_zero_samples_are_invalid(self):
        text = log().replace("count=100", "count=0", 1)
        with self.assertRaisesRegex(ValueError, "no samples"):
            check.report([("a", text)], io.StringIO())

    def test_main_exit_codes(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            good = os.path.join(directory, "good.log")
            bad = os.path.join(directory, "bad.log")
            broken = os.path.join(directory, "broken.log")
            with open(good, "w", encoding="utf-8") as handle:
                handle.write(log())
            with open(bad, "w", encoding="utf-8") as handle:
                handle.write(log(p99={"server_update": 2760}))
            with open(broken, "w", encoding="utf-8") as handle:
                handle.write(log(drop="server_update"))
            stdout = sys.stdout
            sys.stdout = io.StringIO()
            try:
                self.assertEqual(check.main([good]), 0)
                self.assertEqual(check.main([bad]), 1)
                self.assertEqual(check.main([bad, "--report-only"]), 0)
                self.assertEqual(check.main([broken, "--report-only"]), 2)
            finally:
                sys.stdout = stdout


if __name__ == "__main__":
    unittest.main()
