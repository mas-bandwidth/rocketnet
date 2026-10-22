#!/usr/bin/env python3
"""Adversarial receipt fixtures; no SDK endpoint or host benchmark."""

import hashlib
import io
import json
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

from tools.check_split_load import main, strict_object, validate

RUN = "0123456789abcdef"


def line(name, fields):
    return name + ": " + " ".join(f"{key}={value}" for key, value in fields.items())


def receipt(role, first, count, total=2):
    server = role == "server"
    window = 60.0
    max_rate = 10000 * 8 / (window * 1000)
    aggregate_rate = max_rate * count
    return dict(schema="split_v1", role=role, run_id=RUN, total=total,
                first=first, count=count, duration_target_s=60,
                connect_elapsed_s="1.0", start_elapsed_s="1.0",
                traffic_actual_s="60.01", drain_actual_s="3.01", barrier_actual_s="4.0",
                startup_utc_s=1000, traffic_start_utc_s=1002, traffic_end_utc_s=1062,
                connected=count, startup_complete=1, traffic_complete=1, drain_complete=1,
                barrier_complete=1, expected_offers=60 * count, missed_due_slots=0, load_complete=1,
                late_application_deliveries=0,
                c2s_offered=0 if server else 60 * count, c2s_accepted=0 if server else 60 * count,
                c2s_received=60 * count if server else 0,
                s2c_offered=60 * count if server else 0, s2c_accepted=60 * count if server else 0,
                s2c_received=0 if server else 60 * count,
                identity_errors=0, payload_errors=0, order_errors=0,
                duplicate_errors=0, unexpected_disconnects=0,
                offer_refusals=0, protocol_errors=0, wire_valid=1,
                wire_up=10000 * count, wire_down=10000 * count,
                max_peer_up_kbps=f"{max_rate:.6f}", max_peer_down_kbps=f"{max_rate:.6f}",
                aggregate_up_kbps=f"{aggregate_rate:.6f}", aggregate_down_kbps=f"{aggregate_rate:.6f}",
                aggregate_limit_kbps="1000.0", wire_rate_denominator_s=60,
                wire_scope="pre_run_to_traffic_end_conservative",
                workload_scope="traffic_plus_drain", transport="portable",
                profile="default", payload_bytes=32, rate_target_hz=1,
                fds_pre_init=97, fds_post_init=101, fds_pre_term=103, fds_post_term=99,
                threads_pre_init=4, threads_post_init=4,
                threads_pre_term=6, threads_post_term=6,
                resource_baseline_schema="census_v1", warm_cycle=1,
                survivor_threads_operational=0, survivor_handles_operational=0,
                survivor_threads_total=0, survivor_handles_total=0, os_pool_threads=2,
                term_status_code=0, resource_baseline="PASS", delivery_verdict="PASS",
                load_verdict="PASS", bandwidth_verdict="PASS",
                timing_verdict="PASS" if server else "NA", timing_budget_us=100,
                timing_budget_basis="server_update_send_p99_upper", verdict="PASS")


def stream_lines(role, clients):
    lines = []
    for client in clients:
        for combo in range(8):
            due = 60 // 8 + int((combo - client % 8) % 8 < 60 % 8)
            lines.append(line("split_stream", dict(role=role, run_id=RUN, client=client,
                        direction="c2s", combo=combo,
                        sent=due if role == "client-shard" else 0,
                        received=due if role == "server" else 0)))
        lines.append(line("split_stream", dict(role=role, run_id=RUN, client=client,
                    direction="s2c", combo=3, sent=60 if role == "server" else 0,
                    received=60 if role == "client-shard" else 0)))
    return lines


def wire_lines(role, clients):
    return [line("split_wire", dict(role=role, run_id=RUN, client=client,
                 sent_bytes=10000, received_bytes=10000)) for client in clients]


def allocation(server, shards):
    def entry(role, host, text, first=None, count=None):
        fields = dict(role=role, host_id=host, allocation_id=f"alloc-{host}",
                      allocation_ref=f"gate/{host}", runner_id=f"runner-{host}",
                      allocation_type="dedicated_server" if role == "server" else "client_host",
                      log_sha256=hashlib.sha256(text.encode()).hexdigest())
        if role == "server":
            fields["total"] = 2
        else:
            fields.update(first=first, count=count)
        return fields
    return dict(schema="split_allocation_v1", run_id=RUN,
                server=entry("server", "server-machine", server),
                shards=[entry("client-shard", f"client-machine-{index}", text,
                              index, 1) for index, text in enumerate(shards)])


def fixture():
    ready = line("split_ready", dict(schema="split_v1", role="server", run_id=RUN,
                 total=2, duration_s=60, profile="default", payload_bytes=32,
                 rate_target_hz=1, address="192.0.2.1:40000", key="ab" * 32,
                 transport="portable"))
    timing = line("split_server_timing", dict(run_id=RUN, scope="traffic_plus_drain",
                  server_update_count=100, server_update_p50_upper_us=20,
                  server_update_p99_upper_us=80, server_update_max_us="89.3",
                  server_send_count=100, server_send_p99_upper_us=80, clock_pair_count=10000))
    server = "\n".join([ready, *stream_lines("server", range(2)), *wire_lines("server", range(2)), timing,
                       line("split_receipt", receipt("server", 0, 2))])
    shards = []
    for client in range(2):
        shard_ready = line("split_shard_ready", dict(schema="split_v1", role="client-shard",
                           run_id=RUN, total=2, first=client, count=1, duration_s=60,
                           profile="default", payload_bytes=32, rate_target_hz=1))
        shards.append("\n".join([shard_ready, *stream_lines("client-shard", [client]),
                      *wire_lines("client-shard", [client]),
                      line("split_receipt", receipt("client-shard", client, 1))]))
    return server, shards


def timing_failure_fixture():
    server, shards = fixture()
    server = server.replace("server_update_p99_upper_us=80", "server_update_p99_upper_us=101")
    server = server.replace("server_update_max_us=89.3", "server_update_max_us=101.2")
    server = server.replace("timing_verdict=PASS", "timing_verdict=FAIL")
    server = server.replace(" verdict=PASS", " verdict=FAIL")
    return server, shards


def invoke_cli(server, shards):
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        (folder / "server.log").write_text(server)
        for index, text in enumerate(shards):
            (folder / f"shard{index}.log").write_text(text)
        (folder / "allocation.json").write_text(json.dumps(allocation(server, shards)))
        argv = ["check_split_load.py", "--server", str(folder / "server.log"),
                "--shard", str(folder / "shard0.log"), "--shard", str(folder / "shard1.log"),
                "--run-id", RUN, "--total", "2", "--duration", "60",
                "--aggregate-limit-kbps", "1000", "--server-transport", "portable",
                "--client-transport", "portable", "--allocation-manifest",
                str(folder / "allocation.json")]
        stdout, stderr = io.StringIO(), io.StringIO()
        with patch("sys.argv", argv), redirect_stdout(stdout), redirect_stderr(stderr):
            code = main()
        data = json.loads(stdout.getvalue().split("split_merge_json: ", 1)[1].splitlines()[0])
        return code, data, stderr.getvalue()


class SplitReceiptTests(unittest.TestCase):
    def check(self, server, shards):
        return validate(server, shards, RUN, 2, 60, 1000.0,
                        allocation_manifest=allocation(server, shards))

    def test_good_partition(self):
        self.assertEqual(self.check(*fixture())["verdict"], "PASS")

    def test_missing_shard(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "missing client shard"):
            self.check(server, shards[:1])

    def test_overlapping_shard(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "overlapping"):
            self.check(server, [shards[0], shards[0]])

    def test_duplicate_stream(self):
        server, shards = fixture()
        duplicate = stream_lines("server", [0])[0]
        with self.assertRaisesRegex(ValueError, "duplicate stream"):
            self.check(server + "\n" + duplicate, shards)

    def test_wrong_run_id(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "run_id"):
            self.check(server.replace(RUN, "fedcba9876543210"), shards)

    def test_failed_role(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "verdict"):
            self.check(server, [shards[0].replace("verdict=PASS", "verdict=FAIL"), shards[1]])

    def test_short_traffic_window(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "short traffic"):
            self.check(server.replace("traffic_actual_s=60.01", "traffic_actual_s=59.99"), shards)

    def test_short_drain_window(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "short traffic or drain"):
            self.check(server, [shards[0].replace("drain_actual_s=3.01", "drain_actual_s=2.99"), shards[1]])

    def test_late_start(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "startup deadline"):
            self.check(server, [shards[0].replace("start_elapsed_s=1.0", "start_elapsed_s=30.1"), shards[1]])

    def test_duplicate_final_receipt(self):
        server, shards = fixture()
        extra = line("split_receipt", receipt("server", 0, 2))
        with self.assertRaisesRegex(ValueError, "expected exactly one"):
            self.check(server + "\n" + extra, shards)

    def test_missing_stream(self):
        server, shards = fixture()
        removed = stream_lines("server", [0])[0] + "\n"
        with self.assertRaisesRegex(ValueError, "stream coverage"):
            self.check(server.replace(removed, ""), shards)

    def test_delivery_mismatch(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "per-client target rate"):
            self.check(server.replace("direction=c2s combo=0 sent=0 received=8",
                                      "direction=c2s combo=0 sent=0 received=0", 1), shards)

    def test_census_accepts_operating_system_count_churn(self):
        # captain 2026-10-22: counts 97/101/103/99 and 4/4/6/6 with every
        # survivor an OS thread pool worker; the census, not the count, decides
        server, shards = fixture()
        self.check(server, shards)

    def test_census_survivor_handle_fails(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "survivor_handles_total: resource survived"):
            self.check(server.replace("survivor_handles_total=0", "survivor_handles_total=1"), shards)

    def test_census_survivor_thread_fails(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "survivor_threads_operational: resource survived"):
            self.check(server, [shards[0].replace("survivor_threads_operational=0",
                                                  "survivor_threads_operational=1"), shards[1]])

    def test_census_requires_warm_cycle(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "warm_cycle"):
            self.check(server.replace("warm_cycle=1", "warm_cycle=0"), shards)

    def test_unknown_resource_schema_fails(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "resource_baseline_schema"):
            self.check(server.replace("resource_baseline_schema=census_v1",
                                      "resource_baseline_schema=guess"), shards)

    def test_resource_mismatch(self):
        # without a census the counts must return to both phase baselines
        server, shards = fixture()
        server = server.replace("resource_baseline_schema=census_v1", "resource_baseline_schema=phase_counts_v1")
        with self.assertRaisesRegex(ValueError, "resource phase mismatch"):
            self.check(server, shards)

    def test_missing_timing(self):
        server, shards = fixture()
        server = "\n".join(line for line in server.splitlines()
                           if not line.startswith("split_server_timing:"))
        with self.assertRaisesRegex(ValueError, "split_server_timing"):
            self.check(server, shards)

    def test_nonfinite_timing(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "nonfinite"):
            self.check(server.replace("server_update_max_us=89.3", "server_update_max_us=nan"), shards)

    def test_underload(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "target offer rate"):
            self.check(server.replace("s2c_offered=120", "s2c_offered=119")
                       .replace("s2c_accepted=120", "s2c_accepted=119"), shards)

    def test_missing_peer_wire(self):
        server, shards = fixture()
        server = "\n".join(line for line in server.splitlines()
                           if not line.startswith(f"split_wire: role=server run_id={RUN} client=0"))
        with self.assertRaisesRegex(ValueError, "wire coverage"):
            self.check(server, shards)

    def test_false_aggregate_rate(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "reported aggregate rate"):
            self.check(server.replace("aggregate_up_kbps=2.666667", "aggregate_up_kbps=2.0"), shards)

    def test_false_peer_max(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "reported max peer rate"):
            self.check(server.replace("max_peer_up_kbps=1.333333", "max_peer_up_kbps=0.0"), shards)

    def test_same_machine_allocation(self):
        server, shards = fixture()
        manifest = allocation(server, shards)
        manifest["shards"][0]["host_id"] = manifest["server"]["host_id"]
        with self.assertRaisesRegex(ValueError, "not allocated away"):
            validate(server, shards, RUN, 2, 60, 1000.0, allocation_manifest=manifest)

    def test_missing_allocation(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "allocation manifest"):
            validate(server, shards, RUN, 2, 60, 1000.0)

    def test_allocation_log_mismatch(self):
        server, shards = fixture()
        manifest = allocation(server, shards)
        manifest["server"]["log_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "exact role log"):
            validate(server, shards, RUN, 2, 60, 1000.0, allocation_manifest=manifest)

    def test_incomplete_barrier(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "barrier_complete"):
            self.check(server.replace("barrier_complete=1", "barrier_complete=0"), shards)

    def test_late_delivery_does_not_rescue_drain(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "after fixed drain"):
            self.check(server.replace("late_application_deliveries=0",
                                      "late_application_deliveries=1"), shards)

    def test_long_sample_cannot_dilute_bandwidth(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "wire rate denominator"):
            self.check(server.replace("wire_rate_denominator_s=60",
                                      "wire_rate_denominator_s=70"), shards)

    def test_per_client_s2c_underload_cannot_hide_in_aggregate(self):
        server, shards = fixture()
        server = server.replace("client=0 direction=s2c combo=3 sent=60",
                                "client=0 direction=s2c combo=3 sent=120")
        server = server.replace("client=1 direction=s2c combo=3 sent=60",
                                "client=1 direction=s2c combo=3 sent=0")
        shards[0] = shards[0].replace("client=0 direction=s2c combo=3 sent=0 received=60",
                                      "client=0 direction=s2c combo=3 sent=0 received=120")
        shards[1] = shards[1].replace("client=1 direction=s2c combo=3 sent=0 received=60",
                                      "client=1 direction=s2c combo=3 sent=0 received=0")
        with self.assertRaisesRegex(ValueError, "shard summary|per-client target rate"):
            self.check(server, shards)

    def test_per_shard_summary_cannot_cancel_across_shards(self):
        server, shards = fixture()
        for client in range(2):
            for combo in range(8):
                due = 60 // 8 + int((combo - client % 8) % 8 < 60 % 8)
                server = server.replace(
                    f"client={client} direction=c2s combo={combo} sent=0 received={due}",
                    f"client={client} direction=c2s combo={combo} sent=0 received={due * 2 if client == 0 else 0}")
                shards[client] = shards[client].replace(
                    f"client={client} direction=c2s combo={combo} sent={due} received=0",
                    f"client={client} direction=c2s combo={combo} sent={due * 2 if client == 0 else 0} received=0")
        with self.assertRaisesRegex(ValueError, "shard summary and local streams"):
            self.check(server, shards)

    def test_raw_wire_strict_cap_not_rounded_label(self):
        server, shards = fixture()
        # 750000 bytes / 60 s is exactly 100 kbps, while a six-decimal
        # receipt can claim an infinitesimally lower maximum.
        server = server.replace("client=0 sent_bytes=10000", "client=0 sent_bytes=750000")
        server = server.replace("wire_up=20000", "wire_up=760000")
        server = server.replace("aggregate_up_kbps=2.666667", "aggregate_up_kbps=101.333333")
        server = server.replace("max_peer_up_kbps=1.333333", "max_peer_up_kbps=99.999999")
        with self.assertRaisesRegex(ValueError, "raw wire bytes exceed"):
            self.check(server, shards)

    def test_crlf_log_hash_binds_exact_bytes(self):
        server, shards = fixture()
        crlf_server = server.replace("\n", "\r\n")
        manifest = allocation(crlf_server, shards)
        self.assertEqual(validate(crlf_server, shards, RUN, 2, 60, 1000.0,
                                  allocation_manifest=manifest)["verdict"], "PASS")

    def test_duplicate_manifest_key_rejected(self):
        with self.assertRaisesRegex(ValueError, "duplicates"):
            json.loads('{"host_id":"a","host_id":"b"}', object_pairs_hook=strict_object)

    def test_timing_only_failure_reports_non_timing_pass(self):
        result = self.check(*timing_failure_fixture())
        self.assertEqual(result["overall_verdict"], "FAIL")
        self.assertEqual(result["non_timing_verdict"], "PASS")
        self.assertEqual(result["timing_verdict"], "FAIL")
        self.assertEqual(result["reason"], "server_api_p99_budget")

    def test_timing_failure_still_checks_streams(self):
        server, shards = timing_failure_fixture()
        with self.assertRaisesRegex(ValueError, "per-client target rate"):
            self.check(server.replace("client=0 direction=s2c combo=3 sent=60",
                                      "client=0 direction=s2c combo=3 sent=59"), shards)

    def test_timing_failure_still_checks_allocation(self):
        server, shards = timing_failure_fixture()
        manifest = allocation(server, shards)
        manifest["shards"][0]["host_id"] = manifest["server"]["host_id"]
        with self.assertRaisesRegex(ValueError, "not allocated away"):
            validate(server, shards, RUN, 2, 60, 1000.0, allocation_manifest=manifest)

    def test_timing_failure_still_checks_bandwidth(self):
        server, shards = timing_failure_fixture()
        server = server.replace("client=0 sent_bytes=10000", "client=0 sent_bytes=750000")
        server = server.replace("wire_up=20000", "wire_up=760000")
        server = server.replace("aggregate_up_kbps=2.666667", "aggregate_up_kbps=101.333333")
        server = server.replace("max_peer_up_kbps=1.333333", "max_peer_up_kbps=99.999999")
        with self.assertRaisesRegex(ValueError, "raw wire bytes exceed"):
            self.check(server, shards)

    def test_measured_timing_and_reported_verdict_must_agree(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "contradicts measured p99"):
            self.check(server.replace("server_update_p99_upper_us=80",
                                      "server_update_p99_upper_us=101")
                       .replace("server_update_max_us=89.3", "server_update_max_us=101.2"), shards)
        server, shards = timing_failure_fixture()
        with self.assertRaisesRegex(ValueError, "verdict"):
            self.check(server.replace(" verdict=FAIL", " verdict=PASS"), shards)

    def test_timing_quantile_cannot_exceed_reported_maximum(self):
        server, shards = timing_failure_fixture()
        with self.assertRaisesRegex(ValueError, "timing quantiles contradict"):
            self.check(server.replace("server_update_max_us=101.2",
                                      "server_update_max_us=89.3"), shards)

    def test_timing_quantiles_require_integer_upper_bounds(self):
        server, shards = timing_failure_fixture()
        with self.assertRaisesRegex(ValueError, "malformed integer"):
            self.check(server.replace("server_update_p99_upper_us=101",
                                      "server_update_p99_upper_us=101.5"), shards)

    def test_timing_only_cli_is_nonzero_and_machine_readable(self):
        code, data, stderr = invoke_cli(*timing_failure_fixture())
        self.assertEqual(code, 1)
        self.assertEqual(data["non_timing_verdict"], "PASS")
        self.assertEqual(data["timing_verdict"], "FAIL")
        self.assertIn("verdict=FAIL", stderr)

    def test_timing_failure_and_bad_stream_cli_is_not_qualified(self):
        server, shards = timing_failure_fixture()
        server = server.replace("client=0 direction=s2c combo=3 sent=60",
                                "client=0 direction=s2c combo=3 sent=59")
        code, data, stderr = invoke_cli(server, shards)
        self.assertEqual(code, 1)
        self.assertEqual(data["non_timing_verdict"], "FAIL")
        self.assertEqual(data["timing_verdict"], "UNKNOWN")
        self.assertIn("per-client target rate", stderr)

    def test_full_pass_cli_remains_zero(self):
        code, data, stderr = invoke_cli(*fixture())
        self.assertEqual(code, 0)
        self.assertEqual(data["overall_verdict"], "PASS")
        self.assertEqual(data["non_timing_verdict"], "PASS")
        self.assertEqual(data["timing_verdict"], "PASS")
        self.assertEqual(stderr, "")


if __name__ == "__main__":
    unittest.main()
