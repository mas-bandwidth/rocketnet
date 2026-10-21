#!/usr/bin/env python3
"""Fail-closed merger for one server and disjoint remote client-shard receipts."""

import argparse
import math
import re
import sys
from pathlib import Path


PAIR = re.compile(r"\b([A-Za-z_][A-Za-z_0-9]*)=([^\s]+)")
RUN_ID = re.compile(r"[0-9a-fA-F]{16}\Z")


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


def validate_receipt(fields, role, run_id, total, duration, limit):
    expected(fields, schema="split_v1", role=role, run_id=run_id, total=total,
             duration_target_s=duration, profile="default", payload_bytes=32,
             rate_target_hz=1, wire_scope="pre_run_to_traffic_end_conservative",
             workload_scope="traffic_plus_drain", startup_complete=1,
             traffic_complete=1, drain_complete=1, wire_valid=1,
             resource_baseline="PASS", verdict="PASS")
    if fields.get("transport") in (None, "none"):
        raise ValueError("missing granted transport")
    for name in ("identity_errors", "payload_errors", "order_errors", "duplicate_errors",
                 "unexpected_disconnects", "offer_refusals", "protocol_errors"):
        if integer(fields, name) != 0:
            raise ValueError(f"{name}: nonzero")
    if integer(fields, "connected") != integer(fields, "count", 1):
        raise ValueError("population not fully connected")
    if finite(fields, "connect_elapsed_s") > 10.0:
        raise ValueError("connect deadline exceeded")
    if finite(fields, "start_elapsed_s") > (20.0 if role == "server" else 30.0):
        raise ValueError("startup deadline exceeded")
    if finite(fields, "traffic_actual_s") < duration or finite(fields, "drain_actual_s") < 3.0:
        raise ValueError("short traffic or drain window")
    for name in ("startup_utc_s", "traffic_start_utc_s", "traffic_end_utc_s"):
        integer(fields, name, 1)
    if integer(fields, "traffic_end_utc_s") < integer(fields, "traffic_start_utc_s"):
        raise ValueError("local UTC timestamps reversed")
    for name in ("fds", "threads"):
        pre_init = integer(fields, f"{name}_pre_init")
        post_init = integer(fields, f"{name}_post_init")
        pre_term = integer(fields, f"{name}_pre_term")
        post_term = integer(fields, f"{name}_post_term")
        if post_init != pre_term or pre_init != post_term:
            raise ValueError(f"{name}: resource phase mismatch")
    if integer(fields, "term_status_code") != 0:
        raise ValueError("SDK termination failed")
    offered = ("c2s_offered", "s2c_offered")
    accepted = ("c2s_accepted", "s2c_accepted")
    for offer, accept in zip(offered, accepted):
        if integer(fields, offer) != integer(fields, accept):
            raise ValueError(f"{offer}: application offer refused")
    for name in ("c2s_received", "s2c_received", "wire_up", "wire_down"):
        integer(fields, name)
    if finite(fields, "aggregate_limit_kbps", 0.000001) != limit:
        raise ValueError("aggregate cap differs from requested limit")
    for name in ("max_peer_up_kbps", "max_peer_down_kbps"):
        if finite(fields, name) >= 100.0:
            raise ValueError(f"{name}: exceeds per-client cap")
    for name in ("aggregate_up_kbps", "aggregate_down_kbps"):
        if finite(fields, name) >= limit:
            raise ValueError(f"{name}: exceeds aggregate cap")


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


def validate(server_text, shard_texts, run_id, total, duration, limit,
             server_transport=None, client_transport=None):
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
        client_streams.update(streams)
        shards.append(shard)
    if covered != set(range(total)):
        raise ValueError("missing client shard range")
    for key, server_pair in server_streams.items():
        client_pair = client_streams[key]
        if key[1] == "c2s":
            if server_pair[0] != 0 or client_pair[1] != 0 or client_pair[0] != server_pair[1]:
                raise ValueError(f"c2s delivery mismatch {key}")
        elif server_pair[1] != 0 or client_pair[0] != 0 or server_pair[0] != client_pair[1]:
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
    minimum_window = min(finite(shard, "traffic_actual_s", 0.000001) for shard in shards)
    aggregate_up = sum(integer(shard, "wire_up") for shard in shards) * 8 / (minimum_window * 1000)
    aggregate_down = sum(integer(shard, "wire_down") for shard in shards) * 8 / (minimum_window * 1000)
    if aggregate_up >= limit or aggregate_down >= limit:
        raise ValueError("combined client shards exceed aggregate cap")
    return {"verdict": "PASS", "run_id": run_id, "total": total,
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
    args = parser.parse_args()
    try:
        if not (1 <= args.total <= 1000 and 1 <= args.duration <= 3600 and
                math.isfinite(args.aggregate_limit_kbps) and args.aggregate_limit_kbps > 0):
            raise ValueError("invalid expected population, duration, or aggregate cap")
        result = validate(args.server.read_text(), [path.read_text() for path in args.shard],
                          args.run_id, args.total, args.duration, args.aggregate_limit_kbps,
                          args.server_transport, args.client_transport)
    except (OSError, ValueError) as error:
        print(f"split_merge: verdict=FAIL reason={error}", file=sys.stderr)
        return 1
    print("split_merge: " + " ".join(f"{key}={value}" for key, value in result.items()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
