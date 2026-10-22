#!/usr/bin/env python3
"""Fail-closed merger for one server and disjoint remote client-shard receipts."""

import argparse
import hashlib
import json
import math
import re
import sys
from pathlib import Path


PAIR = re.compile(r"\b([A-Za-z_][A-Za-z_0-9]*)=([^\s]+)")
RUN_ID = re.compile(r"[0-9a-fA-F]{16}\Z")


def strict_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"allocation manifest duplicates {key}")
        result[key] = value
    return result


def records(text, prefix):
    found = []
    for line in text.splitlines():
        if not line.startswith(prefix + ": "):
            continue
        pairs = PAIR.findall(line)
        fields = dict(pairs)
        if not pairs or len(fields) != len(pairs):
            raise ValueError(f"{prefix}: absent or duplicate key/value fields")
        found.append(fields)
    return found


def one(text, prefix):
    found = records(text, prefix)
    if len(found) != 1:
        raise ValueError(f"{prefix}: expected exactly one record, found {len(found)}")
    return found[0]


def integer(fields, key, low=0):
    value = fields.get(key)
    if value is None or not re.fullmatch(r"-?\d+", value):
        raise ValueError(f"{key}: missing or malformed integer")
    number = int(value)
    if number < low:
        raise ValueError(f"{key}: below {low}")
    return number


def finite(fields, key, low=0.0):
    try:
        number = float(fields[key])
    except (KeyError, ValueError) as error:
        raise ValueError(f"{key}: missing or malformed number") from error
    if not math.isfinite(number) or number < low:
        raise ValueError(f"{key}: negative or nonfinite")
    return number


def expected(fields, **values):
    for key, value in values.items():
        if fields.get(key) != str(value):
            raise ValueError(f"{key}: expected {value}, got {fields.get(key)}")


def validate_resources(fields):
    """Every thread and handle the role took is returned.

    census_v1 (load_census.h): after one warm cycle, no thread or handle may
    survive from post_init to pre_term or from pre_init to post_term, by
    identity. Operating-system thread pool workers are reported in
    os_pool_threads and never counted. Without a census the counts must
    return to both phase baselines (phase_counts_v1). The counts are
    reported either way.
    """
    for name in ("fds", "threads"):
        for phase in ("pre_init", "post_init", "pre_term", "post_term"):
            integer(fields, f"{name}_{phase}")
    schema = fields.get("resource_baseline_schema")
    if schema == "census_v1":
        expected(fields, warm_cycle=1)
        for name in ("survivor_threads_operational", "survivor_handles_operational",
                     "survivor_threads_total", "survivor_handles_total"):
            if integer(fields, name) != 0:
                raise ValueError(f"{name}: resource survived its phase")
        integer(fields, "os_pool_threads")
        return
    if schema != "phase_counts_v1":
        raise ValueError(f"resource_baseline_schema: unknown {schema}")
    for name in ("fds", "threads"):
        pre_init = integer(fields, f"{name}_pre_init")
        post_init = integer(fields, f"{name}_post_init")
        pre_term = integer(fields, f"{name}_pre_term")
        post_term = integer(fields, f"{name}_post_term")
        if post_init != pre_term or pre_init != post_term:
            raise ValueError(f"{name}: resource phase mismatch")


def validate_receipt(fields, role, run_id, total, duration, limit):
    expected(fields, schema="split_v1", role=role, run_id=run_id, total=total,
             duration_target_s=duration, profile="default", payload_bytes=32,
             rate_target_hz=1, wire_scope="pre_run_to_traffic_end_conservative",
             workload_scope="traffic_plus_drain", startup_complete=1,
             traffic_complete=1, drain_complete=1, barrier_complete=1,
             load_complete=1, wire_valid=1, resource_baseline="PASS",
             delivery_verdict="PASS", load_verdict="PASS", bandwidth_verdict="PASS",
             timing_budget_us=100)
    if role == "server":
        if fields.get("timing_verdict") not in ("PASS", "FAIL"):
            raise ValueError("server timing verdict missing or invalid")
        expected(fields, verdict=fields["timing_verdict"])
    else:
        expected(fields, timing_verdict="NA", verdict="PASS")
    if fields.get("transport") in (None, "none"):
        raise ValueError("missing granted transport")
    for name in ("identity_errors", "payload_errors", "order_errors", "duplicate_errors",
                 "unexpected_disconnects", "offer_refusals", "protocol_errors"):
        if integer(fields, name) != 0:
            raise ValueError(f"{name}: nonzero")
    if integer(fields, "late_application_deliveries"):
        raise ValueError("application packet arrived after fixed drain")
    if integer(fields, "connected") != integer(fields, "count", 1):
        raise ValueError("population not fully connected")
    if finite(fields, "connect_elapsed_s") > 10.0:
        raise ValueError("connect deadline exceeded")
    if finite(fields, "start_elapsed_s") > (20.0 if role == "server" else 30.0):
        raise ValueError("startup deadline exceeded")
    if finite(fields, "traffic_actual_s") < duration or finite(fields, "drain_actual_s") < 3.0:
        raise ValueError("short traffic or drain window")
    if finite(fields, "barrier_actual_s") > (33.1 if role == "server" else 30.1):
        raise ValueError("barrier exceeded bounded teardown")
    if integer(fields, "expected_offers") != duration * integer(fields, "count", 1):
        raise ValueError("wrong expected offer count")
    if integer(fields, "missed_due_slots"):
        raise ValueError("missed due slots")
    for name in ("startup_utc_s", "traffic_start_utc_s", "traffic_end_utc_s"):
        integer(fields, name, 1)
    if integer(fields, "traffic_end_utc_s") < integer(fields, "traffic_start_utc_s"):
        raise ValueError("local UTC timestamps reversed")
    validate_resources(fields)
    if integer(fields, "term_status_code") != 0:
        raise ValueError("SDK termination failed")
    offered = ("c2s_offered", "s2c_offered")
    accepted = ("c2s_accepted", "s2c_accepted")
    for offer, accept in zip(offered, accepted):
        if integer(fields, offer) != integer(fields, accept):
            raise ValueError(f"{offer}: application offer refused")
    for name in ("c2s_received", "s2c_received", "wire_up", "wire_down"):
        integer(fields, name)
    application_offered = "s2c_offered" if role == "server" else "c2s_offered"
    if integer(fields, application_offered) != integer(fields, "expected_offers"):
        raise ValueError("target offer rate was not sustained")
    if finite(fields, "aggregate_limit_kbps", 0.000001) != limit:
        raise ValueError("aggregate cap differs from requested limit")
    if finite(fields, "wire_rate_denominator_s", 0.000001) != duration:
        raise ValueError("wire rate denominator must be target duration")
    expected(fields, timing_budget_basis="server_update_send_p99_upper")
    for name in ("max_peer_up_kbps", "max_peer_down_kbps"):
        if finite(fields, name) >= 100.0:
            raise ValueError(f"{name}: exceeds per-client cap")
    for name in ("aggregate_up_kbps", "aggregate_down_kbps"):
        if finite(fields, name) >= limit:
            raise ValueError(f"{name}: exceeds aggregate cap")


def wire_map(text, role, run_id, first, count, receipt):
    found = {}
    for fields in records(text, "split_wire"):
        expected(fields, role=role, run_id=run_id)
        client = integer(fields, "client")
        if client in found:
            raise ValueError("duplicate per-client wire record")
        found[client] = (integer(fields, "sent_bytes"), integer(fields, "received_bytes"))
    if found.keys() != set(range(first, first + count)):
        raise ValueError("per-client wire coverage mismatch")
    if any(up == 0 or down == 0 for up, down in found.values()):
        raise ValueError("zero per-client wire traffic")
    window = finite(receipt, "wire_rate_denominator_s", 0.000001)
    for label, index, field in (("up", 0, "wire_up"), ("down", 1, "wire_down")):
        total_bytes = sum(pair[index] for pair in found.values())
        if total_bytes != integer(receipt, field):
            raise ValueError(f"{label}: per-client wire sum mismatch")
        aggregate = total_bytes * 8 / (window * 1000)
        maximum = max(pair[index] * 8 / (window * 1000) for pair in found.values())
        if maximum >= 100.0 or aggregate >= finite(receipt, "aggregate_limit_kbps", 0.000001):
            raise ValueError(f"{label}: raw wire bytes exceed strict bandwidth cap")
        if not math.isclose(aggregate, finite(receipt, f"aggregate_{label}_kbps"), abs_tol=0.000002):
            raise ValueError(f"{label}: reported aggregate rate mismatch")
        if not math.isclose(maximum, finite(receipt, f"max_peer_{label}_kbps"), abs_tol=0.000002):
            raise ValueError(f"{label}: reported max peer rate mismatch")
    return found


def validate_timing(text, receipt, run_id):
    timing = one(text, "split_server_timing")
    expected(timing, run_id=run_id, scope="traffic_plus_drain")
    if integer(timing, "server_update_count", 1) == 0 or integer(timing, "server_send_count", 1) == 0:
        raise ValueError("server timing has no samples")
    if integer(timing, "clock_pair_count") != 10000:
        raise ValueError("server timing lacks clock calibration")
    p50 = integer(timing, "server_update_p50_upper_us", 1)
    p99 = integer(timing, "server_update_p99_upper_us", 1)
    max_us = finite(timing, "server_update_max_us")
    send_p99 = integer(timing, "server_send_p99_upper_us", 1)
    # The histogram reports integer-microsecond upper bounds, while max_us
    # prints to three decimals. Allow the half-nanosecond print rounding.
    if p50 > p99 or p99 > math.ceil(max_us + 0.0005):
        raise ValueError("server timing quantiles contradict")
    timing_pass = p99 <= 100 and send_p99 <= 100
    if receipt.get("timing_verdict") != ("PASS" if timing_pass else "FAIL"):
        raise ValueError("server timing receipt contradicts measured p99")
    return timing_pass


def validate_allocation(manifest, server_text, shard_texts, run_id, total, shards):
    if not isinstance(manifest, dict) or manifest.get("schema") != "split_allocation_v1" or manifest.get("run_id") != run_id:
        raise ValueError("missing or mismatched allocation manifest")
    server = manifest.get("server")
    entries = manifest.get("shards")
    if not isinstance(server, dict) or not isinstance(entries, list) or len(entries) != len(shard_texts):
        raise ValueError("allocation role coverage mismatch")
    def check_role(entry, text, role):
        if entry.get("role") != role or entry.get("log_sha256") != hashlib.sha256(text.encode()).hexdigest():
            raise ValueError("allocation does not bind exact role log")
        for key in ("host_id", "allocation_id", "runner_id", "allocation_ref"):
            if not isinstance(entry.get(key), str) or not entry[key].strip():
                raise ValueError(f"allocation lacks {key}")
    check_role(server, server_text, "server")
    if server.get("allocation_type") != "dedicated_server":
        raise ValueError("server lacks dedicated allocation")
    if server.get("total") != total:
        raise ValueError("server allocation population mismatch")
    seen = set()
    for entry, text, shard in zip(entries, shard_texts, shards):
        if not isinstance(entry, dict):
            raise ValueError("malformed shard allocation")
        check_role(entry, text, "client-shard")
        if entry.get("allocation_type") != "client_host" or entry.get("host_id") == server["host_id"] or entry.get("allocation_id") == server["allocation_id"]:
            raise ValueError("client shard is not allocated away from server")
        key = (entry.get("first"), entry.get("count"))
        if key != (integer(shard, "first"), integer(shard, "count")) or key in seen:
            raise ValueError("shard allocation range mismatch")
        seen.add(key)


def stream_map(text, role, run_id):
    result = {}
    for fields in records(text, "split_stream"):
        expected(fields, role=role, run_id=run_id)
        client = integer(fields, "client")
        direction = fields.get("direction")
        combo = integer(fields, "combo")
        if direction == "c2s" and combo < 8:
            key = (client, direction, combo)
        elif direction == "s2c" and combo == 3:
            key = (client, direction, combo)
        else:
            raise ValueError("invalid stream direction or combo")
        if key in result:
            raise ValueError(f"duplicate stream {key}")
        result[key] = (integer(fields, "sent"), integer(fields, "received"))
    return result


def validate_streams(streams, first, count):
    expected_keys = {(client, "c2s", combo)
                     for client in range(first, first + count) for combo in range(8)}
    expected_keys |= {(client, "s2c", 3) for client in range(first, first + count)}
    if streams.keys() != expected_keys:
        missing = expected_keys - streams.keys()
        extra = streams.keys() - expected_keys
        raise ValueError(f"stream coverage mismatch missing={len(missing)} extra={len(extra)}")


def expected_combo_offers(client, combo, duration):
    offset = (combo - client % 8) % 8
    return duration // 8 + int(offset < duration % 8)


def validate(server_text, shard_texts, run_id, total, duration, limit,
             server_transport=None, client_transport=None, allocation_manifest=None):
    if not RUN_ID.fullmatch(run_id):
        raise ValueError("run ID must be exactly 16 hexadecimal characters")
    run_id = run_id.lower()
    ready = one(server_text, "split_ready")
    expected(ready, schema="split_v1", role="server", run_id=run_id, total=total,
             duration_s=duration, profile="default", payload_bytes=32, rate_target_hz=1)
    if not ready.get("address") or not re.fullmatch(r"[0-9a-f]{64}", ready.get("key", "")):
        raise ValueError("server readiness lacks address or public key")
    server = one(server_text, "split_receipt")
    validate_receipt(server, "server", run_id, total, duration, limit)
    wire_map(server_text, "server", run_id, 0, total, server)
    if server_transport is not None and server.get("transport") != server_transport:
        raise ValueError("server transport differs from requested transport")
    if ready.get("transport") != server.get("transport"):
        raise ValueError("server readiness transport differs from final receipt")
    if integer(server, "first") != 0 or integer(server, "count") != total:
        raise ValueError("server population receipt mismatch")
    if integer(server, "c2s_offered") or integer(server, "c2s_accepted") or integer(server, "s2c_received"):
        raise ValueError("server receipt has client-role counters")
    server_streams = stream_map(server_text, "server", run_id)
    validate_streams(server_streams, 0, total)
    shards, client_streams = [], {}
    covered = set()
    for text in shard_texts:
        shard_ready = one(text, "split_shard_ready")
        shard = one(text, "split_receipt")
        validate_receipt(shard, "client-shard", run_id, total, duration, limit)
        if client_transport is not None and shard.get("transport") != client_transport:
            raise ValueError("client transport differs from requested transport")
        if integer(shard, "s2c_offered") or integer(shard, "s2c_accepted") or integer(shard, "c2s_received"):
            raise ValueError("shard receipt has server-role counters")
        first, count = integer(shard, "first"), integer(shard, "count", 1)
        expected(shard_ready, schema="split_v1", role="client-shard", run_id=run_id,
                 total=total, first=first, count=count, duration_s=duration,
                 profile="default", payload_bytes=32, rate_target_hz=1)
        if first + count > total:
            raise ValueError("shard range exceeds population")
        current = set(range(first, first + count))
        if current & covered:
            raise ValueError("overlapping or duplicated shard range")
        covered |= current
        streams = stream_map(text, "client-shard", run_id)
        validate_streams(streams, first, count)
        own_c2s = sum(pair[0] for key, pair in streams.items() if key[1] == "c2s")
        own_s2c = sum(pair[1] for key, pair in streams.items() if key[1] == "s2c")
        if (own_c2s != integer(shard, "c2s_offered") or
                own_c2s != integer(shard, "c2s_accepted") or
                own_s2c != integer(shard, "s2c_received")):
            raise ValueError("shard summary and local streams disagree")
        wire_map(text, "client-shard", run_id, first, count, shard)
        client_streams.update(streams)
        shards.append(shard)
    if covered != set(range(total)):
        raise ValueError("missing client shard range")
    validate_allocation(allocation_manifest, server_text, shard_texts, run_id, total, shards)
    for key, server_pair in server_streams.items():
        client_pair = client_streams[key]
        if key[1] == "c2s":
            target = expected_combo_offers(key[0], key[2], duration)
            if server_pair[1] != target or client_pair[0] != target:
                raise ValueError(f"c2s per-client target rate or rotation failed {key}")
            if server_pair[0] != 0 or client_pair[1] != 0 or client_pair[0] != server_pair[1]:
                raise ValueError(f"c2s delivery mismatch {key}")
        else:
            if server_pair[0] != duration or client_pair[1] != duration:
                raise ValueError(f"s2c per-client target rate failed {key}")
            if server_pair[1] != 0 or client_pair[0] != 0 or server_pair[0] != client_pair[1]:
                raise ValueError(f"s2c delivery mismatch {key}")
    server_c2s = sum(pair[1] for key, pair in server_streams.items() if key[1] == "c2s")
    server_s2c = sum(pair[0] for key, pair in server_streams.items() if key[1] == "s2c")
    client_c2s = sum(pair[0] for key, pair in client_streams.items() if key[1] == "c2s")
    client_s2c = sum(pair[1] for key, pair in client_streams.items() if key[1] == "s2c")
    if (server_c2s != integer(server, "c2s_received") or
            server_s2c != integer(server, "s2c_accepted") or
            server_s2c != integer(server, "s2c_offered") or
            client_c2s != sum(integer(x, "c2s_accepted") for x in shards) or
            client_c2s != sum(integer(x, "c2s_offered") for x in shards) or
            client_s2c != sum(integer(x, "s2c_received") for x in shards) or
            server_c2s != client_c2s or server_s2c != client_s2c):
        raise ValueError("summary and stream counters disagree")
    if client_c2s == 0 or server_s2c == 0:
        raise ValueError("no application traffic")
    # Shards measure independent monotonic intervals. Divide their summed
    # traffic bytes by the shortest interval for a conservative aggregate bound.
    minimum_window = min(finite(shard, "wire_rate_denominator_s", 0.000001) for shard in shards)
    aggregate_up = sum(integer(shard, "wire_up") for shard in shards) * 8 / (minimum_window * 1000)
    aggregate_down = sum(integer(shard, "wire_down") for shard in shards) * 8 / (minimum_window * 1000)
    if aggregate_up >= limit or aggregate_down >= limit:
        raise ValueError("combined client shards exceed aggregate cap")
    # A timing failure, or malformed timing evidence, must not bypass any of
    # the population, delivery, bandwidth, resource or host checks above.
    timing_pass = validate_timing(server_text, server, run_id)
    verdict = "PASS" if timing_pass else "FAIL"
    return {"schema": "split_merge_v2", "verdict": verdict,
            "overall_verdict": verdict, "non_timing_verdict": "PASS",
            "timing_verdict": "PASS" if timing_pass else "FAIL",
            "reason": "none" if timing_pass else "server_api_p99_budget",
            "run_id": run_id, "total": total,
            "client_c2s": client_c2s, "client_s2c": client_s2c,
            "combined_client_up_kbps_upper": aggregate_up,
            "combined_client_down_kbps_upper": aggregate_down}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--shard", required=True, action="append", type=Path)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--total", required=True, type=int)
    parser.add_argument("--duration", required=True, type=int)
    parser.add_argument("--aggregate-limit-kbps", required=True, type=float)
    parser.add_argument("--server-transport", required=True)
    parser.add_argument("--client-transport", required=True)
    parser.add_argument("--allocation-manifest", required=True, type=Path)
    args = parser.parse_args()
    try:
        if not (1 <= args.total <= 1000 and 1 <= args.duration <= 3600 and
                math.isfinite(args.aggregate_limit_kbps) and args.aggregate_limit_kbps > 0):
            raise ValueError("invalid expected population, duration, or aggregate cap")
        result = validate(args.server.read_bytes().decode("utf-8"),
                          [path.read_bytes().decode("utf-8") for path in args.shard],
                          args.run_id, args.total, args.duration, args.aggregate_limit_kbps,
                          args.server_transport, args.client_transport,
                          json.loads(args.allocation_manifest.read_bytes().decode("utf-8"),
                                     object_pairs_hook=strict_object))
    except (OSError, ValueError, OverflowError) as error:
        print("split_merge_json: " + json.dumps({"schema": "split_merge_v2", "verdict": "FAIL",
              "overall_verdict": "FAIL", "non_timing_verdict": "FAIL",
              "timing_verdict": "UNKNOWN", "reason": "invalid_evidence"}, sort_keys=True))
        print(f"split_merge: verdict=FAIL reason={error}", file=sys.stderr)
        return 1
    print("split_merge_json: " + json.dumps(result, sort_keys=True))
    print("split_merge: " + " ".join(f"{key}={value}" for key, value in result.items()),
          file=sys.stdout if result["verdict"] == "PASS" else sys.stderr)
    return 0 if result["verdict"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
