#!/usr/bin/env python3
"""Adversarial receipt fixtures; no SDK endpoint or host benchmark."""

import hashlib
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

# runnable as python3 tools/test_check_split_load.py from the repository root
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from tools.check_split_load import main, strict_object, validate  # noqa: E402

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


def allocation(server, shards, total=2):
    def entry(role, host, text, first=None, count=None):
        fields = dict(role=role, host_id=host, allocation_id=f"alloc-{host}",
                      allocation_ref=f"gate/{host}", runner_id=f"runner-{host}",
                      allocation_type="dedicated_server" if role == "server" else "client_host",
                      log_sha256=hashlib.sha256(text.encode()).hexdigest())
        if role == "server":
            fields["total"] = total
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


# The space profile: three clients so every delta stagger phase is present,
# one client a shard, two seconds (120 frames, 40 deltas a client). Its
# unreliable streams lose a few messages; loss is reported, never failed.
SPACE_TOTAL = 3
SPACE_DURATION = 2
SPACE_FRAMES = SPACE_DURATION * 60
SPACE_SHAPE = dict(profile="space", payload_bytes=360, rate_target_hz=60,
                   c2s_payload_bytes=360, c2s_rate_hz=60, s2c_player_bytes=100,
                   s2c_player_rate_hz=60, s2c_delta_bytes=1184, s2c_delta_rate_hz=20,
                   s2c_delta_stagger="client_mod_3", server_frame_hz=60,
                   client_up_cap_kbps=250, client_down_cap_kbps=350)
SPACE_STREAMS = (("c2s", 0), ("s2c", 0), ("s2c", 5))
SPACE_LOSS = {(0, "c2s", 0): 3, (2, "s2c", 0): 1, (1, "s2c", 5): 2}


def space_target(combo):
    return SPACE_FRAMES // 3 if combo == 5 else SPACE_FRAMES


def space_received(client, direction, combo, loss):
    return space_target(combo) - loss.get((client, direction, combo), 0)


def space_stream_lines(role, clients, loss):
    lines = []
    for client in clients:
        for direction, combo in SPACE_STREAMS:
            sender = (role == "client-shard") == (direction == "c2s")
            lines.append(line("split_stream", dict(
                role=role, run_id=RUN, client=client, direction=direction, combo=combo,
                sent=space_target(combo) if sender else 0,
                received=0 if sender else space_received(client, direction, combo, loss))))
    return lines


def space_receipt(role, first, count, loss, up_bytes, down_bytes, limit):
    """A client sends up_bytes and receives down_bytes over the window."""
    server = role == "server"
    clients = range(first, first + count)
    sent, received = (down_bytes, up_bytes) if server else (up_bytes, down_bytes)

    def rate(total_bytes):
        return total_bytes * 8 / (SPACE_DURATION * 1000)

    fields = receipt(role, first, count, total=SPACE_TOTAL)
    fields.update(SPACE_SHAPE)
    player = len(clients) * SPACE_FRAMES
    delta = len(clients) * SPACE_FRAMES // 3
    c2s_received = sum(space_received(c, "c2s", 0, loss) for c in clients)
    player_received = sum(space_received(c, "s2c", 0, loss) for c in clients)
    delta_received = sum(space_received(c, "s2c", 5, loss) for c in clients)
    delta_lost = sum(loss.get((c, "s2c", 5), 0) for c in clients)
    fields.update(
        duration_target_s=SPACE_DURATION, traffic_actual_s="2.01", aggregate_limit_kbps=f"{limit:.1f}",
        traffic_start_utc_s=1002, traffic_end_utc_s=1004, wire_rate_denominator_s=SPACE_DURATION,
        expected_offers=player + delta if server else player,
        c2s_offered=0 if server else player, c2s_accepted=0 if server else player,
        c2s_received=c2s_received if server else 0,
        s2c_offered=player + delta if server else 0, s2c_accepted=player + delta if server else 0,
        s2c_received=0 if server else player_received + delta_received,
        c2s_unreliable_offered=0 if server else player, c2s_unreliable_accepted=0 if server else player,
        c2s_unreliable_received=c2s_received if server else 0,
        s2c_player_offered=player if server else 0, s2c_player_accepted=player if server else 0,
        s2c_player_received=0 if server else player_received,
        s2c_delta_offered=delta if server else 0, s2c_delta_accepted=delta if server else 0,
        s2c_delta_received=0 if server else delta_received,
        s2c_delta_receipts_acked=delta - delta_lost if server else 0,
        s2c_delta_receipts_lost=delta_lost if server else 0, messages_dropped_over_budget=0,
        wire_up=sent * count, wire_down=received * count,
        max_peer_up_kbps=f"{rate(sent):.6f}", max_peer_down_kbps=f"{rate(received):.6f}",
        aggregate_up_kbps=f"{rate(sent * count):.6f}", aggregate_down_kbps=f"{rate(received * count):.6f}")
    return fields


def space_fixture(loss=None, up_bytes=50000, down_bytes=60000, limit=1000.0):
    """50000 bytes up is 200 kbps and 60000 down is 240 kbps over two
    seconds: over the default 100 kbps cap, under the space caps."""
    loss = SPACE_LOSS if loss is None else loss
    ready = line("split_ready", dict(schema="split_v1", role="server", run_id=RUN,
                 total=SPACE_TOTAL, duration_s=SPACE_DURATION, **SPACE_SHAPE,
                 address="192.0.2.1:40000", key="ab" * 32, transport="portable"))
    timing = line("split_server_timing", dict(run_id=RUN, scope="traffic_plus_drain",
                  server_update_count=100, server_update_p50_upper_us=20,
                  server_update_p99_upper_us=80, server_update_max_us="89.3",
                  server_send_count=100, server_send_p99_upper_us=80, clock_pair_count=10000))
    clients = range(SPACE_TOTAL)
    server_wire = [line("split_wire", dict(role="server", run_id=RUN, client=client,
                        sent_bytes=down_bytes, received_bytes=up_bytes)) for client in clients]
    server = "\n".join([ready, *space_stream_lines("server", clients, loss), *server_wire, timing,
                        line("split_receipt", space_receipt("server", 0, SPACE_TOTAL, loss, up_bytes, down_bytes, limit))])
    shards = []
    for client in clients:
        shard_ready = line("split_shard_ready", dict(schema="split_v1", role="client-shard",
                           run_id=RUN, total=SPACE_TOTAL, first=client, count=1,
                           duration_s=SPACE_DURATION, **SPACE_SHAPE))
        wire = line("split_wire", dict(role="client-shard", run_id=RUN, client=client,
                    sent_bytes=up_bytes, received_bytes=down_bytes))
        shards.append("\n".join([shard_ready, *space_stream_lines("client-shard", [client], loss), wire,
                      line("split_receipt", space_receipt("client-shard", client, 1, loss, up_bytes, down_bytes, limit))]))
    return server, shards


def invoke_cli(server, shards, profile=None, total=2, duration=60):
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        (folder / "server.log").write_text(server)
        shard_args = []
        for index, text in enumerate(shards):
            (folder / f"shard{index}.log").write_text(text)
            shard_args += ["--shard", str(folder / f"shard{index}.log")]
        (folder / "allocation.json").write_text(json.dumps(allocation(server, shards, total)))
        argv = ["check_split_load.py", "--server", str(folder / "server.log"), *shard_args,
                "--run-id", RUN, "--total", str(total), "--duration", str(duration),
                "--aggregate-limit-kbps", "1000", "--server-transport", "portable",
                "--client-transport", "portable", "--allocation-manifest",
                str(folder / "allocation.json")]
        if profile is not None:
            argv += ["--profile", profile]
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


class SpaceProfileTests(unittest.TestCase):
    def check(self, server, shards, profile="space", limit=1000.0):
        return validate(server, shards, RUN, SPACE_TOTAL, SPACE_DURATION, limit,
                        allocation_manifest=allocation(server, shards, SPACE_TOTAL), profile=profile)

    def test_space_loss_is_reported_not_failed(self):
        result = self.check(*space_fixture())
        self.assertEqual(result["verdict"], "PASS")
        self.assertEqual(result["delivery_verdict"], "PASS")
        self.assertEqual(result["profile"], "space")
        loss = result["loss"]
        self.assertEqual(loss["c2s_unreliable"]["offered"], 3 * SPACE_FRAMES)
        self.assertEqual(loss["c2s_unreliable"]["lost"], 3)
        self.assertEqual(loss["s2c_player"]["lost"], 1)
        self.assertEqual(loss["s2c_delta"]["offered"], 3 * SPACE_FRAMES // 3)
        self.assertEqual(loss["s2c_delta"]["lost"], 2)
        self.assertEqual(loss["s2c_delta"]["receipts_lost"], 2)
        self.assertAlmostEqual(loss["s2c_player"]["loss_ratio"], 1 / (3 * SPACE_FRAMES))

    def test_space_lossless_run_passes(self):
        result = self.check(*space_fixture(loss={}))
        self.assertEqual(result["verdict"], "PASS")
        self.assertEqual(sum(mode["lost"] for mode in result["loss"].values()), 0)

    def test_space_logs_fail_the_default_merger(self):
        with self.assertRaisesRegex(ValueError, "profile: expected default"):
            validate(*space_fixture(), RUN, SPACE_TOTAL, SPACE_DURATION, 1000.0,
                     allocation_manifest=allocation(*space_fixture(), SPACE_TOTAL))

    def test_space_default_logs_fail_the_space_merger(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "profile: expected space"):
            validate(server, shards, RUN, 2, 2, 1000.0,
                     allocation_manifest=allocation(server, shards), profile="space")

    def test_space_duration_bounded_by_sequence_tracking(self):
        server, shards = space_fixture()
        with self.assertRaisesRegex(ValueError, "at most 68 seconds"):
            validate(server, shards, RUN, SPACE_TOTAL, 69, 1000.0,
                     allocation_manifest=allocation(server, shards, SPACE_TOTAL), profile="space")

    def test_space_receiving_more_than_offered_fails(self):
        server, shards = space_fixture(loss={})
        shards[1] = shards[1].replace("direction=s2c combo=0 sent=0 received=120",
                                      "direction=s2c combo=0 sent=0 received=121")
        with self.assertRaisesRegex(ValueError, "delivery mismatch|disagree"):
            self.check(server, shards)

    def test_space_identity_payload_duplicate_and_order_errors_still_fail(self):
        for name in ("identity_errors", "payload_errors", "duplicate_errors", "order_errors"):
            server, shards = space_fixture()
            with self.assertRaisesRegex(ValueError, f"{name}: nonzero"):
                self.check(server.replace(f"{name}=0", f"{name}=1"), shards)

    def test_space_player_underload_fails(self):
        server, shards = space_fixture()
        server = server.replace("client=1 direction=s2c combo=0 sent=120",
                                "client=1 direction=s2c combo=0 sent=119")
        with self.assertRaisesRegex(ValueError, "per-client target rate"):
            self.check(server, shards)

    def test_space_delta_stagger_offers_are_checked(self):
        # a delta every frame to client 0 instead of every third frame
        server, shards = space_fixture(loss={})
        server = server.replace("client=0 direction=s2c combo=5 sent=40",
                                "client=0 direction=s2c combo=5 sent=120")
        shards[0] = shards[0].replace("client=0 direction=s2c combo=5 sent=0 received=40",
                                      "client=0 direction=s2c combo=5 sent=0 received=120")
        with self.assertRaisesRegex(ValueError, "per-client target rate|disagree"):
            self.check(server, shards)

    def test_space_expected_offers_count_player_and_delta(self):
        server, shards = space_fixture()
        with self.assertRaisesRegex(ValueError, "wrong expected offer count"):
            self.check(server.replace("expected_offers=480", "expected_offers=360"), shards)
        server, shards = space_fixture()
        with self.assertRaisesRegex(ValueError, "wrong expected offer count"):
            self.check(server, [shards[0].replace("expected_offers=120", "expected_offers=2"), *shards[1:]])

    def test_space_mode_counts_must_sum_to_direction_counts(self):
        server, shards = space_fixture()
        with self.assertRaisesRegex(ValueError, "player and delta counts disagree"):
            self.check(server.replace("s2c_delta_offered=120", "s2c_delta_offered=119")
                       .replace("s2c_delta_accepted=120", "s2c_delta_accepted=119"), shards)

    def test_space_client_upload_cap_is_250_kbps(self):
        # 62500 bytes in two seconds is exactly 250 kbps: the strict cap fails it
        with self.assertRaisesRegex(ValueError, "exceeds per-client cap"):
            self.check(*space_fixture(up_bytes=62500))
        self.assertEqual(self.check(*space_fixture(up_bytes=62499))["verdict"], "PASS")

    def test_space_client_download_cap_is_350_kbps(self):
        # the server's send is the client's download: 87500 bytes is 350 kbps
        with self.assertRaisesRegex(ValueError, "exceeds per-client cap"):
            self.check(*space_fixture(down_bytes=87500, limit=2000.0), limit=2000.0)
        self.assertEqual(self.check(*space_fixture(down_bytes=87499, limit=2000.0), limit=2000.0)["verdict"], "PASS")

    def test_space_aggregate_cap_still_comes_from_the_operator(self):
        # three clients downloading 240 kbps each pass their own cap and
        # still exceed a 700 kbps operator aggregate
        with self.assertRaisesRegex(ValueError, "aggregate_up_kbps: exceeds aggregate cap"):
            self.check(*space_fixture(limit=700.0), limit=700.0)

    def test_space_shard_cannot_report_server_receipts(self):
        server, shards = space_fixture()
        with self.assertRaisesRegex(ValueError, "server delta receipts"):
            self.check(server, [shards[0].replace("s2c_delta_receipts_lost=0", "s2c_delta_receipts_lost=1"),
                                *shards[1:]])

    def test_space_cli_reports_loss_and_passes(self):
        code, data, stderr = invoke_cli(*space_fixture(), profile="space", total=SPACE_TOTAL,
                                        duration=SPACE_DURATION)
        self.assertEqual(code, 0, stderr)
        self.assertEqual(data["overall_verdict"], "PASS")
        self.assertEqual(data["loss"]["c2s_unreliable"]["lost"], 3)
        self.assertEqual(stderr, "")

    def test_space_cli_without_profile_refuses_space_logs(self):
        code, data, stderr = invoke_cli(*space_fixture(), total=SPACE_TOTAL, duration=SPACE_DURATION)
        self.assertEqual(code, 1)
        self.assertEqual(data["non_timing_verdict"], "FAIL")
        self.assertIn("profile", stderr)


if __name__ == "__main__":
    unittest.main()
