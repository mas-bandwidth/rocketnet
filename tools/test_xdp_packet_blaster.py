#!/usr/bin/env python3
"""Pure packet-blaster contract tests: no socket, thread, root, or clock."""

from __future__ import annotations

import base64
import json
import pathlib
import struct
import subprocess
import sys
import tempfile


def run(argv: list[str], expected: int) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(argv, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode != expected:
        raise AssertionError(
            f"exit {result.returncode}, expected {expected}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def dry_args(binary: str, manifest: str, token_file: str) -> list[str]:
    return [
        binary,
        "--dry-run",
        "--interface",
        "bench0",
        "--queue",
        "0",
        "--xdp-mode",
        "native",
        "--source",
        "10.40.0.2:42000",
        "--server",
        "10.40.0.1:40000",
        "--source-mac",
        "02:00:00:00:00:02",
        "--next-hop-mac",
        "02:00:00:00:00:01",
        "--packet-filter",
        "b7590100",
        "--token-file",
        token_file,
        "--manifest",
        manifest,
        "--clients",
        "1",
        "--duration",
        "2",
        "--rate-pps",
        "10",
        "--payload-bytes",
        "64",
        "--source-port",
        "42000",
        "--seed",
        "0x1234",
        "--mode",
        "throughput",
        "--direction",
        "both",
        "--bind",
        "auto",
        "--owned-bench-confirm",
        "I-OWN-THIS-BENCH",
    ]


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_xdp_packet_blaster.py BINARY MANIFEST")
    binary, manifest_path = sys.argv[1:]

    run([binary, "--self-test"], 0)

    manifest = json.loads(pathlib.Path(manifest_path).read_text(encoding="utf-8"))
    assert manifest["schema_version"] == 1
    cases = manifest["cases"]
    assert len(cases) == 7
    assert len({case["name"] for case in cases}) == len(cases)
    profiles = ("xdp", "gso", "io_uring", "msgx", "iocp", "rio")
    required = {
        "name",
        "source",
        "operation",
        "argument",
        "expected_delta",
        "expected_recovery",
        "sequence_state",
    }
    for profile in profiles:
        required.update({f"expected_stage_{profile}", f"expected_counter_{profile}"})
    operations = {
        "truncate",
        "oversize",
        "wrong_epoch",
        "wrong_filter",
        "reserved_type",
        "replay",
        "bad_tag",
    }
    generic_prefilter = {
        "truncate": ("server-ingress-size-filter", "PREFILTER_TOO_SHORT"),
        "oversize": ("server-ingress-size-filter", "PREFILTER_TOO_LARGE"),
        "wrong_epoch": ("server-prefilter", "PREFILTER_WRONG_EPOCH"),
        "wrong_filter": ("server-prefilter", "PREFILTER_WRONG_VALUE"),
        "reserved_type": ("server-type-filter", "RESERVED_TYPE"),
        "replay": ("server-replay-window", "PAYLOAD_ALREADY_RECEIVED"),
        "bad_tag": ("server-aead-open", "PAYLOAD_DID_NOT_OPEN"),
    }
    xdp_oracles = {
        "truncate": ("xdp-prefilter", "XDP_KERNEL_DROPPED_TOO_SMALL"),
        "oversize": ("xdp-prefilter", "XDP_KERNEL_DROPPED_TOO_LARGE"),
        "wrong_epoch": ("xdp-prefilter", "XDP_KERNEL_DROPPED_WRONG_EPOCH"),
        "wrong_filter": ("xdp-prefilter", "XDP_KERNEL_DROPPED_WRONG_FILTER"),
        "reserved_type": ("xdp-type-filter", "XDP_KERNEL_DROPPED_INVALID_TYPE"),
        "replay": ("server-replay-window", "PAYLOAD_ALREADY_RECEIVED"),
        "bad_tag": ("server-aead-open", "PAYLOAD_DID_NOT_OPEN"),
    }
    for case in cases:
        assert required <= case.keys()
        assert case["operation"] in operations
        assert case["expected_recovery"]
        assert case["expected_delta"] in {1, 2}
        assert case["sequence_state"] in {"accepted", "fresh"}
        assert case["source"].startswith(("sdk/rocketnet_wire.h", "tests/pcapng/regress/", "fuzz/corpus/"))
        for profile in profiles:
            expected_stage, expected_counter = xdp_oracles[case["operation"]] if profile == "xdp" else generic_prefilter[case["operation"]]
            assert case[f"expected_stage_{profile}"] == expected_stage
            assert case[f"expected_counter_{profile}"] == expected_counter
    assert {case["operation"] for case in cases} == operations

    with tempfile.TemporaryDirectory(prefix="rn-blaster-test-") as temporary:
        temporary_path = pathlib.Path(temporary)
        token_file = pathlib.Path(temporary) / "tokens.txt"
        # Structural dry-run token: the runtime later performs the full
        # production connect/crypto/time checks. Signed-field offsets are the
        # canonical rocketnet_connect_token.h layout.
        token = bytearray(768)
        token[32] = 1  # ROCKETNET_ADDRESS_FLAG_IPV4
        token[33:37] = bytes((10, 40, 0, 1))
        struct.pack_into("<H", token, 37, 40000)
        token_text = base64.b64encode(token).decode("ascii")
        assert len(token_text) == 1024
        token_file.write_text(token_text + "\n", encoding="ascii")
        args = dry_args(binary, manifest_path, str(token_file))
        first = run(args, 0)
        second = run(args, 0)
        assert first.stdout == second.stdout
        assert "attach=false transmit=false" in first.stdout
        assert "next=dry_run_stop" in first.stdout
        assert "throughput_offers_per_enabled_direction=20" in first.stdout
        assert "nominal_rocketnet_bytes=101" in first.stdout
        assert "nominal_frame_bytes=143" in first.stdout
        assert "nominal_wire_slot_bytes=167" in first.stdout
        assert "nominal_physical_wire_bytes_per_direction=3340" in first.stdout
        assert "downstream=controlled_server_workload_contract" in first.stdout
        assert "endpoint_arena_required_bytes=" in first.stdout
        assert "manifest_cases=7" in first.stdout

        for profile in profiles:
            robustness = list(args)
            robustness[robustness.index("throughput")] = "robustness"
            robustness.extend(("--server-transport-profile", profile))
            result = run(robustness, 0)
            assert f"server_transport_profile={profile} profile_source=operator-declared" in result.stdout
            assert "runtime_autodetect=false counter_observed=false" in result.stdout
            case_lines = [line for line in result.stdout.splitlines() if line.startswith("PLAN_CASE ")]
            assert len(case_lines) == 7
            for case, line in zip(cases, case_lines):
                expected_stage = case[f"expected_stage_{profile}"]
                expected_counter = case[f"expected_counter_{profile}"]
                assert f"name={case['name']} server_transport_profile={profile}" in line
                assert f"expected_stage={expected_stage} expected_counter={expected_counter}" in line
                assert f"expected_delta={case['expected_delta']}" in line

        missing_profile = list(args)
        missing_profile[missing_profile.index("throughput")] = "robustness"
        refused = run(missing_profile, 2)
        assert "requires an explicit --server-transport-profile" in refused.stderr

        for unsupported_profile in ("portable", "loopback", "unknown"):
            unsupported = list(missing_profile)
            unsupported.extend(("--server-transport-profile", unsupported_profile))
            refused = run(unsupported, 2)
            assert "unsupported server transport profile" in refused.stderr

        public = list(args)
        public[public.index("--server") + 1] = "8.8.8.8:40000"
        refused = run(public, 2)
        assert "private or ULA" in refused.stderr

        unbounded = list(args)
        unbounded[unbounded.index("--duration") + 1] = "601"
        run(unbounded, 2)

        wrong_queue = list(args)
        wrong_queue[wrong_queue.index("--queue") + 1] = "7"
        run(wrong_queue, 2)

        unconfirmed = list(args)
        unconfirmed[unconfirmed.index("I-OWN-THIS-BENCH")] = "no"
        refused = run(unconfirmed, 2)
        assert "owned bench confirmation absent" in refused.stderr

        unsafe_mac = list(args)
        unsafe_mac[unsafe_mac.index("--next-hop-mac") + 1] = "ff:ff:ff:ff:ff:ff"
        refused = run(unsafe_mac, 2)
        assert "nonzero unicast" in refused.stderr

        # No key/token material is printed even on a malformed token refusal.
        token_file.write_text("SECRET#" + "A" * 1017 + "\n", encoding="ascii")
        refused = run(args, 2)
        assert "SECRET" not in refused.stdout + refused.stderr

        token_file.write_text((token_text + "\n") * 2, encoding="ascii")
        duplicate = list(args)
        duplicate[duplicate.index("--clients") + 1] = "2"
        refused = run(duplicate, 2)
        assert "duplicate token" in refused.stderr

        wrong_server = bytearray(token)
        wrong_server[36] = 2
        token_file.write_text(base64.b64encode(wrong_server).decode("ascii") + "\n", encoding="ascii")
        refused = run(args, 2)
        assert "exact explicit server" in refused.stderr
        token_file.write_text(token_text + "\n", encoding="ascii")

        def refused_manifest(name: str, changed: dict[str, object], message: str) -> None:
            path = temporary_path / name
            path.write_text(json.dumps(changed), encoding="utf-8")
            candidate = list(args)
            candidate[candidate.index("--manifest") + 1] = str(path)
            result = run(candidate, 2)
            assert message in result.stderr

        bad_schema = json.loads(json.dumps(manifest))
        bad_schema["schema_version"] = 2
        refused_manifest("schema.json", bad_schema, "schema_version must be exactly 1")

        bad_sequence = json.loads(json.dumps(manifest))
        next(case for case in bad_sequence["cases"] if case["operation"] == "bad_tag")["sequence_state"] = "accepted"
        refused_manifest("sequence.json", bad_sequence, "noncanonical argument, sequence state, or oracle")

        bad_argument = json.loads(json.dumps(manifest))
        next(case for case in bad_argument["cases"] if case["operation"] == "wrong_filter")["argument"] = 1
        refused_manifest("argument.json", bad_argument, "noncanonical argument, sequence state, or oracle")

        bad_oracle = json.loads(json.dumps(manifest))
        next(case for case in bad_oracle["cases"] if case["operation"] == "replay")["expected_counter_xdp"] = "XDP_KERNEL_DROPPED_REPLAYED"
        refused_manifest("oracle.json", bad_oracle, "noncanonical argument, sequence state, or oracle")

        mismatched_profile = json.loads(json.dumps(manifest))
        next(case for case in mismatched_profile["cases"] if case["operation"] == "wrong_filter")["expected_counter_gso"] = "XDP_KERNEL_DROPPED_WRONG_FILTER"
        refused_manifest("profile-mismatch.json", mismatched_profile, "noncanonical argument, sequence state, or oracle")

        missing_profile_oracle = json.loads(json.dumps(manifest))
        next(case for case in missing_profile_oracle["cases"] if case["operation"] == "truncate").pop("expected_counter_rio")
        refused_manifest("profile-missing.json", missing_profile_oracle, "missing or overlong server transport profile oracle")

        bad_delta = json.loads(json.dumps(manifest))
        next(case for case in bad_delta["cases"] if case["operation"] == "replay")["expected_delta"] = 1
        refused_manifest("delta.json", bad_delta, "noncanonical argument, sequence state, or oracle")

        missing_case = json.loads(json.dumps(manifest))
        missing_case["cases"].pop()
        refused_manifest("missing.json", missing_case, "exact seven canonical operations")

        duplicate_operation = json.loads(json.dumps(manifest))
        duplicate_operation["cases"][-1]["operation"] = "truncate"
        refused_manifest("duplicate-operation.json", duplicate_operation, "noncanonical argument, sequence state, or oracle")

    print("test_xdp_packet_blaster: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
