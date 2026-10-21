#!/usr/bin/env python3
"""Adversarial receipt fixtures; no SDK endpoint or host benchmark."""

import unittest
import hashlib

from tools.check_split_load import validate

RUN = "0123456789abcdef"


def line(name, fields):
    return name + ": " + " ".join(f"{key}={value}" for key, value in fields.items())


def receipt(role, first, count, total=2):
    server = role == "server"
    window = 60.01
    max_rate = 10000 * 8 / (window * 1000)
    aggregate_rate = max_rate * count
    return dict(schema="split_v1", role=role, run_id=RUN, total=total,
                first=first, count=count, duration_target_s=60,
                connect_elapsed_s="1.0", start_elapsed_s="1.0",
                traffic_actual_s="60.01", drain_actual_s="3.01", barrier_actual_s="4.0",
                startup_utc_s=1000, traffic_start_utc_s=1002, traffic_end_utc_s=1062,
                connected=count, startup_complete=1, traffic_complete=1, drain_complete=1,
                barrier_complete=1, expected_offers=60 * count, missed_due_slots=0, load_complete=1,
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
                aggregate_limit_kbps="1000.0",
                wire_scope="pre_run_to_traffic_end_conservative",
                workload_scope="traffic_plus_drain_plus_barrier", transport="portable",
                profile="default", payload_bytes=32, rate_target_hz=1,
                fds_pre_init=10, fds_post_init=11, fds_pre_term=11, fds_post_term=10,
                threads_pre_init=2, threads_post_init=3,
                threads_pre_term=3, threads_post_term=2,
                term_status_code=0, resource_baseline="PASS", delivery_verdict="PASS",
                load_verdict="PASS", bandwidth_verdict="PASS",
                timing_verdict="PASS" if server else "NA", timing_budget_us=100, verdict="PASS")


def stream_lines(role, clients):
    lines = []
    for client in clients:
        for combo in range(8):
            lines.append(line("split_stream", dict(role=role, run_id=RUN, client=client,
                        direction="c2s", combo=combo,
                        sent=(8 if combo < 4 else 7) if role == "client-shard" else 0,
                        received=(8 if combo < 4 else 7) if role == "server" else 0)))
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
        with self.assertRaisesRegex(ValueError, "delivery mismatch"):
            self.check(server.replace("direction=c2s combo=0 sent=0 received=8",
                                      "direction=c2s combo=0 sent=0 received=0", 1), shards)

    def test_resource_mismatch(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "resource phase mismatch"):
            self.check(server.replace("fds_pre_term=11", "fds_pre_term=12"), shards)

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
            self.check(server.replace("aggregate_up_kbps=2.666222", "aggregate_up_kbps=2.0"), shards)

    def test_false_peer_max(self):
        server, shards = fixture()
        with self.assertRaisesRegex(ValueError, "reported max peer rate"):
            self.check(server.replace("max_peer_up_kbps=1.333111", "max_peer_up_kbps=0.0"), shards)

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


if __name__ == "__main__":
    unittest.main()
