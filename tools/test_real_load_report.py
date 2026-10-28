#!/usr/bin/env python3
"""Unit cases for tools/real_load_report.py on canned run directories."""

import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import real_load_report as report  # noqa: E402

CALLS = report.CALLS


def server_log(connected=1000, unexpected=0, disconnects=0, p99=None, p999=None, drop_call=None):
    p99 = p99 or {}
    p999 = p999 or {}
    lines = ["split_ready: schema=split_v1 role=server run_id=0123456789abcdef total=1000"]
    for call in CALLS:
        if call == drop_call:
            continue
        lines.append(f"split_server_call: run_id=0123456789abcdef total=1000 transport=gso "
                     f"scope=traffic_plus_drain load=proof call={call} count=100 p50_upper_us=3 "
                     f"p99_upper_us={p99.get(call, 40)} p999_upper_us={p999.get(call, 90)} "
                     f"max_us=310.500 overbudget_count=2 budget_us=100")
    lines.append(f"split_proof: role=server run_id=0123456789abcdef transport=gso disconnects={disconnects} "
                 f"verdict={'PASS' if disconnects == 0 else 'FAIL'}")
    lines.append(f"split_receipt: schema=split_v1 role=server run_id=0123456789abcdef total=1000 first=0 "
                 f"count=1000 connected={connected} c2s_accepted=12000 c2s_received=11990 "
                 f"s2c_accepted=12000 s2c_received=12000 unexpected_disconnects={unexpected} verdict=FAIL")
    return "\n".join(lines) + "\n"


MERGE_PASS = ("split_proof_cell: verdict=PASS transport=gso total=1000 streams=26000 "
              "messages_sent=24000 messages_delivered=23990 unreliable_lost=10 sequenced_skipped=0 "
              "timing_verdict=FAIL\n")


class RealLoadReportTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(dir=HERE, prefix="real-load-test-")

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def cell(self, name, server=None, merge=MERGE_PASS, console=""):
        path = os.path.join(self.root, name)
        os.makedirs(path)
        if server is not None:
            with open(os.path.join(path, "server.log"), "w") as handle:
                handle.write(server)
        if merge is not None:
            with open(os.path.join(path, "merge.log"), "w") as handle:
                handle.write(merge)
        with open(os.path.join(path, "lan-proof.console"), "w") as handle:
            handle.write(console)
        return path

    def run_report(self, *cells):
        out = os.path.join(self.root, "out")
        with redirect_stdout(io.StringIO()) as text:
            status = report.main(["--out", out, "--head", "abc", "--date", "2026-10-27"] + list(cells))
        return status, out, text.getvalue()

    def test_clean_cell_passes_and_reports_quantiles(self):
        row = report.summarise("gso", self.cell("gso", server_log()))
        self.assertEqual(row["status"], "PASS", row["reason"])
        self.assertEqual((row["update_p99_us"], row["update_p999_us"], row["update_max_us"]),
                         ("40", "90", "310.500"))
        self.assertEqual(row["unreliable_lost"], "10")
        self.assertEqual(row["target"], "inside")

    def test_target_is_reported_never_gated(self):
        row = report.summarise("gso", self.cell("gso", server_log(p99={"server_update": 150})))
        self.assertEqual(row["status"], "PASS")
        self.assertEqual(row["target"], "outside")

    def test_short_connect_fails(self):
        row = report.summarise("gso", self.cell("gso", server_log(connected=998)))
        self.assertEqual(row["status"], "FAIL")
        self.assertIn("connected 998 of 1000", row["reason"])

    def test_disconnect_fails(self):
        row = report.summarise("gso", self.cell("gso", server_log(disconnects=3, unexpected=3)))
        self.assertEqual(row["status"], "FAIL")
        self.assertIn("disconnects=3", row["reason"])

    def test_refused_proof_cell_fails(self):
        merge = "split_proof_cell: verdict=FAIL transport=gso reason=stream_mismatch\n"
        row = report.summarise("gso", self.cell("gso", server_log(), merge=merge))
        self.assertEqual(row["status"], "FAIL")
        self.assertIn("stream_mismatch", row["reason"])

    def test_missing_server_log_names_the_refusal(self):
        console = "cell gso\nlan-proof: FAIL: a host is hot: load 9 on 16 cores\n"
        row = report.summarise("gso", self.cell("gso", None, merge=None, console=console))
        self.assertEqual(row["status"], "FAIL")
        self.assertIn("a host is hot", row["reason"])

    def test_missing_timing_fails(self):
        row = report.summarise("gso", self.cell("gso", server_log(drop_call="server_send_packets")))
        self.assertEqual(row["status"], "FAIL")
        self.assertEqual(row["target"], "unknown")

    def test_main_writes_artifacts_and_exit_status(self):
        good = self.cell("gso", server_log())
        status, out, text = self.run_report(f"gso={good}")
        self.assertEqual(status, 0)
        self.assertIn("real_load: cell=gso status=PASS", text)
        with open(os.path.join(out, "real-load.json")) as handle:
            data = json.load(handle)
        self.assertEqual(data["cells"][0]["calls"]["server_update"]["p999_upper_us"], "90")
        with open(os.path.join(out, "real-load.tsv")) as handle:
            self.assertEqual(len(handle.read().splitlines()), 2)
        bad = self.cell("rio", None, merge=None)
        status, _, _ = self.run_report(f"gso={good}", f"rio={bad}")
        self.assertEqual(status, 1)

    def test_bad_usage(self):
        status, _, _ = self.run_report("gso")
        self.assertEqual(status, 2)


if __name__ == "__main__":
    unittest.main()
