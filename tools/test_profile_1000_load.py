#!/usr/bin/env python3
"""Synthetic receipt and provenance checks; no build or network run."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("profile_1000_load", Path(__file__).with_name("profile_1000_load.py"))
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


HEAD = "a" * 40
SUMMARY = ("load: connected=1000 profile=light connect_within_10s=PASS "
           "no_unexpected_disconnect=PASS reliable_delivery=PASS "
           "unreliable_integrity=PASS counters_agree=PASS "
           "server_update_p99_us=51319 "
           "frame_server_update_count=6000 "
           "frame_server_update_p50_upper_us=48000 "
           "frame_server_update_p99_upper_us=52000 "
           "frame_server_update_max_us=53000 "
           "frame_server_update_overbudget_count=6000 "
           "fds_start=4 fds_end=4 threads_start=1 threads_end=1 "
           "offered_rate_mode=per_update target_messages_per_client_second=0 "
           "application_offers_accepted=UNMEASURED "
           "server_transport_io=portable client_transport_io=portable verdict=PASS\n")
VM_STAT = ("Mach Virtual Memory Statistics: (page size of 16384 bytes)\n"
           "Pages free:                               100000.\n"
           "Pages active:                             200000.\n"
           "Pages inactive:                           300000.\n"
           "Pages speculative:                        50000.\n"
           "Pages wired down:                         100000.\n")
ZERO_CALL = (" frame_server_disconnect_reason_count=0"
             " frame_server_disconnect_reason_p50_upper_us=0"
             " frame_server_disconnect_reason_p99_upper_us=0"
             " frame_server_disconnect_reason_max_us=0.000"
             " frame_server_disconnect_reason_overbudget_count=0")
PHASE_COUNTS = (" fds_pre_init=3 fds_post_init=4 fds_pre_term=4 fds_post_term=3"
                " threads_pre_init=2 threads_post_init=3 threads_pre_term=3 threads_post_term=2"
                " term_status_code=0 resource_baseline_schema=phase_pairs_v1 resource_baseline=PASS")
PHASE_SUMMARY = (SUMMARY.replace("fds_start=4 fds_end=4 threads_start=1 threads_end=1",
                                 "fds_start=4 fds_end=3 threads_start=3 threads_end=2")
                 .replace(" verdict=PASS", PHASE_COUNTS + " verdict=PASS"))


class MacMemoryTests(unittest.TestCase):
    def test_vm_stat_header_and_reclaimable_pages(self):
        def fake_command(argv, cwd=None):
            return "34359738368" if argv[0] == "sysctl" else VM_STAT

        with mock.patch.object(profile, "command", fake_command):
            total, available = profile.mac_memory()
        self.assertEqual(total, 34359738368)
        self.assertEqual(available, (100000 + 300000 + 50000) * 16384)

    def test_missing_or_malformed_memory_count_is_unknown(self):
        for output in (VM_STAT.replace("Pages speculative:                        50000.\n", ""),
                       VM_STAT.replace("Pages free:                               100000.",
                                       "Pages free:                               unknown."),
                       VM_STAT.replace("Pages inactive:                           300000.",
                                       "Pages inactive:                           300000.\nPages inactive: 300000.")):
            with self.subTest(output=output), self.assertRaises(ValueError):
                profile.parse_mac_vm_stat(output)


class ReceiptTests(unittest.TestCase):
    def test_population_default_and_requested_mismatch(self):
        result = profile.parse_load(SUMMARY, "portable", "portable")
        self.assertEqual(result["expected_clients"], 1000)
        self.assertEqual(result["expected_profile"], "light")
        with self.assertRaisesRegex(ValueError, "population"):
            profile.parse_load(SUMMARY, "portable", "portable", expected_clients=999)

    def test_profile_must_match_requested_profile(self):
        with self.assertRaisesRegex(ValueError, "profile"):
            profile.parse_load(SUMMARY, "portable", "portable", expected_profile="default")

    def test_rate_mode_and_echo_must_match_request(self):
        fixed = (SUMMARY.replace("profile=light", "profile=default")
                 .replace("offered_rate_mode=per_update", "offered_rate_mode=per_client_due")
                 .replace("target_messages_per_client_second=0",
                          "target_messages_per_client_second=1")
                 .replace("application_offers_accepted=UNMEASURED",
                          "application_offers_accepted=PASS"))
        result = profile.parse_load(fixed, "portable", "portable", expected_profile="default",
                                    expected_rate=1.0)
        self.assertEqual(result["expected_rate"], 1.0)
        for line in (fixed.replace("target_messages_per_client_second=1",
                                   "target_messages_per_client_second=2"),
                     fixed.replace("offered_rate_mode=per_client_due", "offered_rate_mode=per_update"),
                     fixed.replace("application_offers_accepted=PASS",
                                   "application_offers_accepted=UNMEASURED")):
            with self.subTest(line=line), self.assertRaisesRegex(ValueError, "rate"):
                profile.parse_load(line, "portable", "portable", expected_profile="default",
                                   expected_rate=1.0)

    def test_rate_echo_uses_six_significant_digits_without_tolerance_zeros(self):
        rounded = (SUMMARY.replace("offered_rate_mode=per_update", "offered_rate_mode=per_client_due")
                   .replace("target_messages_per_client_second=0",
                            "target_messages_per_client_second=1.23457")
                   .replace("application_offers_accepted=UNMEASURED",
                            "application_offers_accepted=PASS"))
        profile.parse_load(rounded, "portable", "portable", expected_rate=1.234567)
        tiny_zero = (SUMMARY.replace("offered_rate_mode=per_update", "offered_rate_mode=per_client_due")
                     .replace("target_messages_per_client_second=0",
                              "target_messages_per_client_second=0")
                     .replace("application_offers_accepted=UNMEASURED",
                              "application_offers_accepted=PASS"))
        with self.assertRaisesRegex(ValueError, "rate"):
            profile.parse_load(tiny_zero, "portable", "portable", expected_rate=1e-15)

    def test_exact_binary_plan_and_headroom_are_recorded(self):
        plan = ("load_plan: clients=64 profile=default client_public_required_bytes=100 "
                "server_public_required_bytes=1000 total_public_required_bytes=7400 "
                "public_memory_scope=create_only transport_selection=unresolved_before_start "
                "term_status_code=0 reason=none verdict=PASS\n")
        result = profile.parse_memory_plan(plan, 64, "default")
        self.assertEqual(result["total_public_required_bytes"], 7400)
        self.assertEqual(result["headroom_bytes"], 2 * 1024**3)
        self.assertIn("not a proven upper bound", result["headroom_policy"])
        for invalid in (plan.replace("clients=64", "clients=63"),
                        plan.replace("total_public_required_bytes=7400", "total_public_required_bytes=7401"),
                        plan.replace("profile=default", "profile=light"),
                        plan.replace("public_memory_scope=create_only", "public_memory_scope=all_runtime"),
                        plan.replace("verdict=PASS", "verdict=FAIL")):
            with self.subTest(plan=invalid), self.assertRaises(ValueError):
                profile.parse_memory_plan(invalid, 64, "default")

    def test_explicit_phase_pairs_allow_runtime_resources_to_close_at_term(self):
        result = profile.parse_load(PHASE_SUMMARY, "portable", "portable")
        self.assertEqual(result["resource_baseline_schema"], "phase_pairs_v1")
        self.assertEqual(result["fields"]["fds_start"], "4")
        self.assertEqual(result["fields"]["fds_end"], "3")

    def test_explicit_phase_pairs_fail_closed(self):
        for line in (PHASE_SUMMARY.replace("fds_pre_term=4", "fds_pre_term=3"),
                     PHASE_SUMMARY.replace("threads_post_term=2", "threads_post_term=3"),
                     PHASE_SUMMARY.replace("fds_pre_init=3", "fds_pre_init=-1"),
                     PHASE_SUMMARY.replace("fds_post_init=4 ", ""),
                     PHASE_SUMMARY.replace("fds_end=3", "fds_end=4"),
                     PHASE_SUMMARY.replace("term_status_code=0", "term_status_code=-1"),
                     PHASE_SUMMARY.replace("resource_baseline=PASS", "resource_baseline=FAIL"),
                     PHASE_SUMMARY.replace("resource_baseline_schema=phase_pairs_v1", "")):
            with self.subTest(line=line), self.assertRaises(ValueError):
                profile.parse_load(line, "portable", "portable")

    def test_partial_new_resource_markers_cannot_fall_back_to_legacy(self):
        for marker in ("resource_baseline=PASS", "term_status_code=-1",
                       "resource_baseline=PASS term_status_code=-1"):
            line = SUMMARY.replace(" verdict=PASS", f" {marker} verdict=PASS")
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                profile.parse_load(line, "portable", "portable")

    def test_valid_histogram_is_retained_without_budget_pass(self):
        result = profile.parse_load(SUMMARY, "portable", "portable")
        self.assertEqual(result["reported_frame_calls"]["frame_server_update"]["count"], 6000)
        self.assertEqual(result["reported_calls_at_or_above_100us"], ["frame_server_update"])
        self.assertIn("frame_client_update", result["missing_frame_calls"])
        self.assertEqual(result["every_main_call_budget"], "UNMEASURED")

    def test_zero_sample_category_is_present_but_unmeasured(self):
        result = profile.parse_load(SUMMARY.rstrip() + ZERO_CALL + "\n", "portable", "portable")
        category = result["reported_frame_calls"]["frame_server_disconnect_reason"]
        self.assertEqual(category["status"], "UNMEASURED")
        self.assertEqual(category["count"], 0)
        self.assertIn("frame_server_disconnect_reason", result["unmeasured_frame_calls"])
        self.assertNotIn("frame_server_disconnect_reason", result["missing_frame_calls"])
        self.assertEqual(result["every_main_call_budget"], "UNMEASURED")

    def test_zero_sample_category_must_be_complete_and_all_zero(self):
        for line in (ZERO_CALL.replace("frame_server_disconnect_reason_count=0", ""),
                     ZERO_CALL.replace("frame_server_disconnect_reason_p99_upper_us=0", ""),
                     ZERO_CALL.replace("frame_server_disconnect_reason_p50_upper_us=0",
                                       "frame_server_disconnect_reason_p50_upper_us=1"),
                     ZERO_CALL.replace("frame_server_disconnect_reason_p99_upper_us=0",
                                       "frame_server_disconnect_reason_p99_upper_us=1"),
                     ZERO_CALL.replace("frame_server_disconnect_reason_max_us=0.000",
                                       "frame_server_disconnect_reason_max_us=1.000"),
                     ZERO_CALL.replace("frame_server_disconnect_reason_overbudget_count=0",
                                       "frame_server_disconnect_reason_overbudget_count=1")):
            with self.subTest(line=line), self.assertRaises(ValueError):
                profile.parse_load(SUMMARY.rstrip() + line + "\n", "portable", "portable")

    def test_missing_or_invalid_resource_counts_fail_closed(self):
        for line in (SUMMARY.replace("fds_start=4 ", ""),
                     SUMMARY.replace("threads_end=1 ", ""),
                     SUMMARY.replace("fds_start=4", "fds_start=-1"),
                     SUMMARY.replace("threads_start=1", "threads_start=nope")):
            with self.subTest(line=line), self.assertRaises(ValueError):
                profile.parse_load(line, "portable", "portable")

    def test_incomplete_histogram_and_nonfinite_value_fail_closed(self):
        for line in (SUMMARY.replace("frame_server_update_count=6000 ", ""),
                     SUMMARY.replace("frame_server_update_p99_upper_us=52000 ", ""),
                     SUMMARY.replace("verdict=PASS", "fds_start=4 verdict=PASS"),
                     SUMMARY.replace("frame_server_update_p99_upper_us=52000", "frame_server_update_p99_upper_us=nan"),
                     SUMMARY.replace("frame_server_update_overbudget_count=6000", "frame_server_update_overbudget_count=6001")):
            with self.subTest(line=line), self.assertRaises(ValueError):
                profile.parse_load(line, "portable", "portable")


class ProvenanceTests(unittest.TestCase):
    def run_fake(self, wrong_source=False, missing_binary=False, clients=None, rate=None,
                 load_profile=None, include_overhead=True):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = (root / "source").resolve()
            source.mkdir()
            output = (root / "evidence").resolve()
            seen = []

            def fake_command(argv, cwd=None):
                if argv[-2:] == ["rev-parse", "HEAD"]:
                    return HEAD
                if argv[-2:] == ["status", "--porcelain"]:
                    return ""
                raise AssertionError(argv)

            def fake_run(argv, cwd, env, timeout):
                seen.append((argv, dict(env)))
                if argv[:2] == ["cmake", "-S"]:
                    build_dir = output / "build"
                    build_dir.mkdir()
                    cache_source = root / "wrong-source" if wrong_source else source
                    (build_dir / "CMakeCache.txt").write_text(
                        f"CMAKE_BUILD_TYPE:STRING=Release\nCMAKE_HOME_DIRECTORY:INTERNAL={cache_source}\n")
                    return 0, "configured", "", False
                if argv[:2] == ["cmake", "--build"]:
                    if not missing_binary:
                        binary = output / "bin" / "load"
                        binary.parent.mkdir()
                        binary.write_bytes(b"synthetic executable")
                        binary.chmod(0o755)
                    return 0, "built", "", False
                self.assertEqual(argv[:1], [str(output / "bin" / "load")])
                connected = 1000 if clients is None else clients
                if env.get("LOAD_PLAN_ONLY") == "1":
                    client_bytes = 100
                    server_bytes = 1000
                    total_bytes = server_bytes + connected * client_bytes
                    plan = (f"load_plan: clients={connected} profile={env['LOAD_PROFILE']} "
                            f"client_public_required_bytes={client_bytes} "
                            f"server_public_required_bytes={server_bytes} "
                            f"total_public_required_bytes={total_bytes} "
                            "public_memory_scope=create_only transport_selection=unresolved_before_start "
                            "term_status_code=0 reason=none verdict=PASS\n")
                    return 0, plan, "", False
                summary = SUMMARY.replace("connected=1000", f"connected={connected}")
                summary = summary.replace("profile=light", f"profile={env['LOAD_PROFILE']}")
                if rate is not None:
                    summary = (summary.replace("offered_rate_mode=per_update", "offered_rate_mode=per_client_due")
                               .replace("target_messages_per_client_second=0",
                                        f"target_messages_per_client_second={rate:.6g}")
                               .replace("application_offers_accepted=UNMEASURED",
                                        "application_offers_accepted=PASS"))
                return 0, summary, "", False

            args = ["profile_1000_load.py", "--mode", "timings", "--source", str(source),
                    "--output", str(output), "--expected-head", HEAD,
                    "--server-transport", "portable", "--seconds", "60"]
            if clients is not None:
                args += ["--clients", str(clients)]
            if rate is not None:
                args += ["--messages-per-client-second", str(rate)]
            if load_profile is not None:
                args += ["--profile", load_profile]
            if include_overhead:
                args += ["--transport-overhead-reserve-bytes", "4096",
                         "--transport-overhead-evidence",
                         f"synthetic {HEAD} {profile.platform.system()} portable "
                         f"{1000 if clients is None else clients} clients measured receipt"]
            with mock.patch.object(sys, "argv", args), mock.patch.object(profile, "command", fake_command), \
                 mock.patch.object(profile, "run_capture", fake_run), \
                 mock.patch.object(profile, "preflight", lambda *_: {"failures": []}), \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                code = profile.main()
            receipt = json.loads((output / "receipt.json").read_text())
            return code, receipt, seen

    def test_fresh_release_build_is_recorded_and_used(self):
        code, receipt, seen = self.run_fake()
        self.assertEqual(code, 0)
        self.assertEqual(receipt["verdict"], "COLLECTED_NOT_QUALIFIED")
        self.assertEqual(seen[1][0][-2:], ["--parallel", "4"])
        self.assertEqual(receipt["runs"][0]["command"][0], receipt["binary"])
        self.assertEqual(len(receipt["runs"]), 3)
        self.assertIn("binary_sha256", receipt)

    def test_wrong_cmake_source_is_rejected(self):
        code, receipt, seen = self.run_fake(wrong_source=True)
        self.assertEqual(code, 1)
        self.assertEqual(receipt["verdict"], "INCOMPLETE")
        self.assertIn("CMake build source differs", receipt["error"])
        self.assertEqual(len(seen), 2)

    def test_missing_built_binary_is_rejected(self):
        code, receipt, seen = self.run_fake(missing_binary=True)
        self.assertEqual(code, 1)
        self.assertEqual(receipt["verdict"], "INCOMPLETE")
        self.assertIn("load binary missing", receipt["error"])
        self.assertEqual(len(seen), 2)

    def test_population_and_rate_are_passed_and_recorded(self):
        code, receipt, seen = self.run_fake(clients=64, rate=2.5)
        self.assertEqual(code, 0)
        self.assertEqual(receipt["clients"], 64)
        self.assertEqual(receipt["sdk_default_clients"], 1000)
        self.assertEqual(receipt["profile"], "light")
        self.assertEqual(receipt["messages_per_client_second"], 2.5)
        self.assertEqual(receipt["rate_environment"], {"LOAD_MESSAGES_PER_CLIENT_SECOND": "2.5"})
        self.assertEqual(receipt["runs"][0]["load"]["expected_clients"], 64)
        self.assertEqual(receipt["runs"][0]["load"]["expected_rate"], 2.5)
        self.assertEqual(receipt["runs"][0]["command"][1:3], ["64", "60"])
        self.assertEqual(seen[3][1]["LOAD_MESSAGES_PER_CLIENT_SECOND"], "2.5")
        self.assertEqual(seen[3][1]["LOAD_PROFILE"], "light")
        self.assertEqual(receipt["memory_plan"]["minimum_available_memory_bytes"],
                         receipt["memory_plan"]["public_plan_with_headroom_bytes"] + 4096)

    def test_default_profile_is_passed_and_recorded(self):
        code, receipt, seen = self.run_fake(clients=4, load_profile="default")
        self.assertEqual(code, 0)
        self.assertEqual(receipt["profile"], "default")
        self.assertEqual(receipt["runs"][0]["load"]["expected_profile"], "default")
        self.assertEqual(seen[3][1]["LOAD_PROFILE"], "default")

    def test_default_rate_preserves_unset_environment(self):
        code, receipt, seen = self.run_fake()
        self.assertEqual(code, 0)
        self.assertIsNone(receipt["messages_per_client_second"])
        self.assertEqual(receipt["rate_environment"], {})
        self.assertEqual(receipt["messages_per_client_second_source"],
                         "harness default; environment unset")
        self.assertNotIn("LOAD_MESSAGES_PER_CLIENT_SECOND", seen[3][1])

    def test_uncalibrated_transport_overhead_fails_closed_before_build(self):
        code, receipt, seen = self.run_fake(include_overhead=False, load_profile="default")
        self.assertEqual(code, 1)
        self.assertIn("positive measured transport/native overhead reserve", receipt["error"])
        self.assertNotIn("memory_plan", receipt)
        self.assertNotIn("runs", receipt)
        self.assertEqual(seen, [])

    def test_legacy_light_profile_keeps_floor_and_labels_incomplete_memory_scope(self):
        code, receipt, _ = self.run_fake(include_overhead=False)
        self.assertEqual(code, 0)
        plan = receipt["memory_plan"]
        self.assertEqual(plan["capacity_status"],
                         "PUBLIC_PLAN_LOWER_BOUND_ONLY_TRANSPORT_UNMEASURED")
        self.assertIn("does not establish", plan["capacity_limitation"])


class ArgumentTests(unittest.TestCase):
    def test_clients_and_rate_require_positive_values(self):
        for value in ("0", "-1", "1.5", "nope"):
            with self.subTest(clients=value), self.assertRaises(profile.argparse.ArgumentTypeError):
                profile.positive_integer(value)
        for value in ("0", "-0.1", "nan", "inf", "-inf", "nope"):
            with self.subTest(rate=value), self.assertRaises(profile.argparse.ArgumentTypeError):
                profile.positive_finite_rate(value)
        self.assertEqual(profile.positive_integer("17"), 17)
        self.assertEqual(profile.positive_finite_rate("0.125"), 0.125)

    def test_transport_overhead_reserve_requires_provenance_and_matching_scope(self):
        head = "a" * 40
        with self.assertRaisesRegex(ValueError, "positive measured"):
            profile.validate_transport_overhead_reserve(None, None, "portable", 64, head, "Linux", "default")
        with self.assertRaisesRegex(ValueError, "evidence provenance"):
            profile.validate_transport_overhead_reserve(1024, None, "portable", 64, head, "Linux", "default")
        with self.assertRaisesRegex(ValueError, "identify"):
            profile.validate_transport_overhead_reserve(
                1024, f"{head} Linux portable 1000 client run", "portable", 64, head, "Linux", "default")
        result = profile.validate_transport_overhead_reserve(
            1024, f"{head} Linux portable 64 clients measured receipt", "portable", 64,
            head, "Linux", "default")
        self.assertEqual(result["bytes"], 1024)
        self.assertIsNone(profile.validate_transport_overhead_reserve(
            None, None, "portable", 64, head, "Linux", "light"))

    def test_preflight_uses_supplied_requirement_without_relaxing_baseline(self):
        with tempfile.TemporaryDirectory() as temp, \
             mock.patch.object(profile.platform, "system", return_value="Linux"), \
             mock.patch.object(profile, "linux_memory", return_value=(64 * 1024**3, 24 * 1024**3)), \
             mock.patch.object(profile.resource, "getrlimit", return_value=(4096, 4096)), \
             mock.patch.object(profile.shutil, "disk_usage", return_value=type("D", (), {"free": 16 * 1024**3})()), \
             mock.patch.object(profile.os, "cpu_count", return_value=16), \
             mock.patch.object(profile.os, "getloadavg", return_value=(0.0, 0.0, 0.0)):
            result = profile.preflight(Path(temp), clients=1000,
                                       required_available_bytes=32 * 1024**3)
        self.assertEqual(result["minimum_available_memory_bytes"], 32 * 1024**3)
        self.assertTrue(any("available memory below required" in item for item in result["failures"]))

    def test_preflight_never_relaxes_existing_available_memory_floor(self):
        with tempfile.TemporaryDirectory() as temp, \
             mock.patch.object(profile.platform, "system", return_value="Linux"), \
             mock.patch.object(profile, "linux_memory", return_value=(64 * 1024**3, 16 * 1024**3)), \
             mock.patch.object(profile.resource, "getrlimit", return_value=(4096, 4096)), \
             mock.patch.object(profile.shutil, "disk_usage", return_value=type("D", (), {"free": 16 * 1024**3})()), \
             mock.patch.object(profile.os, "cpu_count", return_value=16), \
             mock.patch.object(profile.os, "getloadavg", return_value=(0.0, 0.0, 0.0)):
            result = profile.preflight(Path(temp), clients=1, required_available_bytes=1)
        self.assertEqual(result["minimum_available_memory_bytes"], profile.MIN_AVAILABLE_BYTES)


if __name__ == "__main__":
    unittest.main()
