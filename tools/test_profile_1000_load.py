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
SUMMARY = ("load: connected=1000 connect_within_10s=PASS "
           "no_unexpected_disconnect=PASS reliable_delivery=PASS "
           "unreliable_integrity=PASS counters_agree=PASS "
           "server_update_p99_us=51319 "
           "frame_server_update_count=6000 "
           "frame_server_update_p50_upper_us=48000 "
           "frame_server_update_p99_upper_us=52000 "
           "frame_server_update_max_us=53000 "
           "frame_server_update_overbudget_count=6000 "
           "fds_start=4 fds_end=4 threads_start=1 threads_end=1 "
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
    def run_fake(self, wrong_source=False, missing_binary=False):
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
                seen.append(argv)
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
                return 0, SUMMARY, "", False

            args = ["profile_1000_load.py", "--mode", "timings", "--source", str(source),
                    "--output", str(output), "--expected-head", HEAD,
                    "--server-transport", "portable", "--seconds", "60"]
            with mock.patch.object(sys, "argv", args), mock.patch.object(profile, "command", fake_command), \
                 mock.patch.object(profile, "run_capture", fake_run), \
                 mock.patch.object(profile, "preflight", lambda _: {"failures": []}), \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                code = profile.main()
            receipt = json.loads((output / "receipt.json").read_text())
            return code, receipt, seen

    def test_fresh_release_build_is_recorded_and_used(self):
        code, receipt, seen = self.run_fake()
        self.assertEqual(code, 0)
        self.assertEqual(receipt["verdict"], "COLLECTED_NOT_QUALIFIED")
        self.assertEqual(seen[1][-2:], ["--parallel", "4"])
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


if __name__ == "__main__":
    unittest.main()
