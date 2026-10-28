#!/usr/bin/env python3
"""
Unit tests for tools/perf_trend.py. Fixtures only: no build, no clock, no
sockets, no network.

1. The instruction report parses per region, and a not-comparable report is
   recorded as such rather than as missing regions.
2. The campaign summary parses the matrix rows and ignores the honesty plants;
   a refused or truncated campaign is recorded incomplete (its instruction
   counts are kept), and a malformed row is refused.
3. A wall-clock row inside the baseline spread is tied; outside it is above or
   below the spread; with no baseline it says so. Nothing here fails.
4. record appends to the history and reports the change since the previous
   night of the same OS.
5. repin writes one OS's rows with its reason and keeps the other OS; it
   refuses an empty reason and an incomplete campaign.
"""

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import perf_trend as pt

INSTRUCTIONS = (
    "isolation mode=client_requests\n"
    "instructions region=client_frame ir=788000 per_unit=12312.50 baseline=787980 delta_pct=+0.00% verdict=PASS\n"
    "instructions region=server_tick ir=120000000 per_unit=1875000.00 baseline=114119350 delta_pct=+5.15% verdict=FAIL\n"
)

CAMPAIGN = (
    "rocketnet transport bench campaign\n"
    "host            Linux 6.17.0 x86_64 b2\n"
    "cores           recv=15 send=14\n"
    "--- harness honesty plants\n"
    "honesty_bench_portable_100000.txt: pps median=100012 min=100012 max=100012\n"
    "--- the matrix (30 s warm-up, 5 x 60 s windows per harness)\n"
    "bench_portable.txt: pps median=900000 min=880000 max=910000\n"
    "bench_gso.txt: pps median=3e+06 min=2.9e+06 max=3.1e+06\n"
    "campaign complete -- /tmp/x\n"
)


class ParseTests(unittest.TestCase):
    def test_instructions_regions(self) -> None:
        got = pt.parse_instructions(INSTRUCTIONS)
        self.assertTrue(got["comparable"])
        self.assertEqual(got["regions"]["client_frame"]["ir"], 788000)
        self.assertEqual(got["regions"]["server_tick"]["verdict"], "FAIL")
        self.assertAlmostEqual(got["regions"]["server_tick"]["delta_pct"], 5.15)

    def test_instructions_not_comparable(self) -> None:
        got = pt.parse_instructions("not comparable: GNU-13.3.0/valgrind-3.22.0 vs GNU-15.2.0/valgrind-3.26.0\n")
        self.assertFalse(got["comparable"])
        self.assertEqual(got["regions"], {})
        self.assertIn("not comparable", got["note"])

    def test_instructions_empty_refused(self) -> None:
        with self.assertRaises(ValueError):
            pt.parse_instructions("isolation mode=client_requests\n")

    def test_campaign_rows_skip_honesty(self) -> None:
        got = pt.parse_campaign(CAMPAIGN)
        self.assertEqual(sorted(got["rows"]), ["bench_gso", "bench_portable"])
        self.assertEqual(got["rows"]["bench_gso"]["median"], 3e6)
        self.assertEqual(got["cores"], "recv=15 send=14")

    def test_campaign_refused_is_incomplete_not_lost(self) -> None:
        text = CAMPAIGN.replace("campaign complete -- /tmp/x\n", "REFUSED: bench_uring control process failed; see x\n")
        got = pt.parse_campaign(text)
        self.assertFalse(got["complete"])
        self.assertEqual(got["refused"], "REFUSED: bench_uring control process failed; see x")
        self.assertEqual(sorted(got["rows"]), ["bench_gso", "bench_portable"])

    def test_campaign_truncated_is_incomplete(self) -> None:
        got = pt.parse_campaign(CAMPAIGN.replace("campaign complete -- /tmp/x\n", ""))
        self.assertFalse(got["complete"])
        self.assertIn("did not complete", got["refused"])

    def test_campaign_complete_without_rows_refused(self) -> None:
        with self.assertRaises(ValueError):
            pt.parse_campaign("campaign complete -- /tmp/x\n")

    def test_campaign_inconsistent_row_refused(self) -> None:
        with self.assertRaises(ValueError):
            pt.parse_campaign(CAMPAIGN.replace("median=900000", "median=990000"))


class CompareTests(unittest.TestCase):
    BASE = {"median": 1000.0, "min": 950.0, "max": 1050.0}

    def test_inside_spread_is_tied(self) -> None:
        status, delta = pt.compare_row({"median": 960.0, "min": 940.0, "max": 990.0}, self.BASE)
        self.assertEqual(status, "tied")
        self.assertAlmostEqual(delta, -4.0)

    def test_spreads_overlapping_on_baseline_median_is_tied(self) -> None:
        status, _ = pt.compare_row({"median": 940.0, "min": 900.0, "max": 1001.0}, self.BASE)
        self.assertEqual(status, "tied")

    def test_below_and_above(self) -> None:
        self.assertEqual(pt.compare_row({"median": 800.0, "min": 790.0, "max": 810.0}, self.BASE)[0], "below baseline spread")
        self.assertEqual(pt.compare_row({"median": 1200.0, "min": 1190.0, "max": 1210.0}, self.BASE)[0], "above baseline spread")

    def test_no_baseline(self) -> None:
        self.assertEqual(pt.compare_row({"median": 1.0, "min": 1.0, "max": 1.0}, None), ("no baseline", None))


class RecordAndRepinTests(unittest.TestCase):
    def test_record_appends_and_reports_previous(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bench = os.path.join(d, "run.txt")
            ins = os.path.join(d, "instructions.txt")
            base = os.path.join(d, "baseline.json")
            hist_in = os.path.join(d, "in.jsonl")
            hist_out = os.path.join(d, "out.jsonl")
            rec_out = os.path.join(d, "record.json")
            with open(bench, "w", encoding="utf-8") as f:
                f.write(CAMPAIGN)
            with open(ins, "w", encoding="utf-8") as f:
                f.write(INSTRUCTIONS)
            with open(base, "w", encoding="utf-8") as f:
                json.dump({"linux": None, "macos": None}, f)
            prev = {
                "os": "linux",
                "instructions": {"regions": {"client_frame": {"ir": 780000}}},
                "bench": {"rows": {"bench_portable": {"median": 1000000.0}}},
            }
            with open(hist_in, "w", encoding="utf-8") as f:
                f.write(json.dumps({"os": "macos", "bench": {"rows": {}}}) + "\n")
                f.write(json.dumps(prev) + "\n")
            args = [
                "record", "--os", "linux", "--host", "h", "--ref", "main", "--commit", "abc",
                "--date", "2026-01-01T00:00:00Z", "--bench", bench, "--instructions", ins,
                "--instructions-exit", "1", "--baseline", base, "--history", hist_in,
                "--out", hist_out, "--record-out", rec_out,
            ]
            self.assertEqual(pt.main(args), 0)
            with open(hist_out, encoding="utf-8") as f:
                lines = [json.loads(x) for x in f if x.strip()]
            self.assertEqual(len(lines), 3)
            self.assertEqual(lines[-1]["instructions"]["exit"], 1)
            self.assertEqual(lines[-1]["bench"]["rows"]["bench_portable"]["median"], 900000.0)
            text = pt.report(lines[-1], {"linux": None}, prev)
            self.assertIn("region=client_frame ir=788000 vs_baseline=+0.00% vs_previous=+1.03% verdict=PASS", text)
            self.assertIn("row=bench_portable pps_median=900000 min=880000 max=910000 vs_previous=-10.0% no baseline (report-only)", text)

    def test_record_without_history_is_first_night(self) -> None:
        self.assertEqual(pt.read_history(None), [])
        self.assertIsNone(pt.previous_record([{"os": "macos"}], "linux"))

    def test_repin_keeps_other_os(self) -> None:
        record = {
            "os": "linux", "host": "h", "commit": "abc", "date": "d",
            "bench": {"cores": "recv=15 send=14", "complete": True, "rows": {"bench_portable": {"median": 1.0, "min": 1.0, "max": 1.0}}},
        }
        out = pt.repin({"linux": None, "macos": {"bench": {}}}, record, "first night")
        self.assertEqual(out["linux"]["reason"], "first night")
        self.assertEqual(out["linux"]["bench"]["bench_portable"]["median"], 1.0)
        self.assertEqual(out["macos"], {"bench": {}})

    def test_repin_refuses_incomplete_campaign(self) -> None:
        record = {"os": "linux", "bench": {"complete": False, "rows": {}}}
        with self.assertRaises(ValueError):
            pt.repin({}, record, "first night")

    def test_repin_refuses_empty_reason(self) -> None:
        with self.assertRaises(ValueError):
            pt.repin({}, {"os": "linux"}, "  ")


if __name__ == "__main__":
    unittest.main()
