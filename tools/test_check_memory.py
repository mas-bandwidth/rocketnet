#!/usr/bin/env python3
"""Process-level tests for tools/check_memory.py's decisions, with a fake
transport_memory written into a temporary directory, so nothing is built and
no server starts: a probe whose observed allocator peak is over its plan makes
the tool exit nonzero, a probe whose rows keep every rule makes it exit 0, and
a probe that prints zero rows (or an empty, unrunnable file) makes it exit
nonzero. The fake answers every transport the tool drives on any host."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().with_name("check_memory.py")

FAKE_PROBE = r'''
import json
import os
import sys

args = sys.argv[1:]
clients, transport, queues = int(args[0]), args[2], int(args[3])
flag = args[4] if len(args) > 4 else None
mode = os.environ.get("FAKE_MEMORY", "honest")
if mode == "silent":
    sys.exit(0)


def out(*rows):
    for row in rows:
        print(json.dumps(row))


if transport == "auto" or clients > 1000:
    sys.stderr.write("memory plan unavailable\n")
    sys.exit(1)
if flag == "--session-plan-only" and ("ROCKETNET_SERVER_MAX_CLIENTS" in os.environ or
                                      "ROCKETNET_DEV_SERVER_LATENCY_MS" in os.environ):
    sys.stderr.write("environment overrides are refused\n")
    sys.exit(1)

stack = 65536
planned = 10000 + 1000 * clients
if flag == "--plan-only":
    if transport == "xdp":
        umem = 1 << 20
        out({"umem_bytes": umem, "worker_threads": queues, "sockets": queues,
             "thread_stack_bytes": stack * queues, "os_bytes": umem + stack * queues})
    elif transport == "io_uring":
        out({"mapped_ring_bytes": 4096, "os_bytes": 4096 + stack})
    elif transport == "rio":
        out({"worker_threads": 1, "allocator_bytes": planned, "planned_bytes": planned,
             "os_bytes": stack, "thread_stack_bytes": stack})
    sys.exit(0)
if flag == "--session-plan-only":
    rio = 4096 if transport == "rio" else 0
    out({"scope": "one_public_server_one_pinned_transport",
         "create_callback_request_bytes": 200, "create_payload_bytes": 100,
         "concurrent_sdk_generations": 1, "active_transport_sdk_callback_bytes": 64,
         "active_xdp_umem_bytes": 0, "active_rio_registered_region_bytes": rio,
         "known_active_or_stopping_requests_bytes": 200 + 64 + rio,
         "native_retained_on_failed_cancel_bytes": None,
         "config_copy_frozen_for_future_start": False, "one_block_sufficient": False})
    sys.exit(0)

os_rows = {"thread_stack_bytes": stack, "umem_bytes": 0, "mapped_ring_bytes": 0,
           "os_bytes": stack, "worker_threads": 1 if transport == "gso" else 2}
if flag is None:
    peak = planned + (1 if mode == "over" else 0)
    out(dict(os_rows, planned_bytes=planned, allocator_bytes=planned, sockets=1 + clients if transport == "msgx" else 1,
             os_and_application_bytes=None),
        {"peak_allocator_bytes": peak, "live_allocator_bytes": peak // 2})
elif flag in ("--lifecycle", "--light-lifecycle"):
    rows = []
    for lifecycle in ("server", "client"):
        row = {"lifecycle": lifecycle, "required_bytes": planned,
               "first_cycle_peak_bytes": planned - 10, "second_cycle_peak_bytes": planned - 10}
        if flag == "--light-lifecycle":
            row.update(profile="light", arena_capacity_bytes=planned, final_live_bytes=0,
                       refused_allocations=0, invalid_frees=0, canaries_ok=True)
        if lifecycle == "server":
            row.update(os_rows)
        rows.append(row)
    out({"plan": True}, *rows)
'''


class CheckMemoryProcessTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.probe = self.dir / "transport_memory"
        self.probe.write_text("#!" + sys.executable + " -S\n" + FAKE_PROBE)
        self.probe.chmod(0o755)

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, probe, mode):
        env = {key: value for key, value in os.environ.items()
               if key not in ("ROCKETNET_SERVER_MAX_CLIENTS", "ROCKETNET_DEV_SERVER_LATENCY_MS")}
        env["FAKE_MEMORY"] = mode
        return subprocess.run([sys.executable, str(TOOL), str(probe)],
                              capture_output=True, text=True, env=env, cwd=self.dir)

    def test_rows_inside_the_plan_pass(self):
        result = self.run_tool(self.probe, "honest")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("test_transport_memory_plans_match_allocations", result.stdout)

    def test_every_platform_branch_passes_and_fails(self):
        # the tool picks its transports from sys.platform; drive each branch
        # (linux: gso, xdp and io_uring; darwin: msgx; win32: iocp and rio)
        runner = ("import runpy, sys; sys.platform = sys.argv.pop(1); "
                  "sys.path.insert(0, sys.argv[1]); sys.argv = sys.argv[2:]; "
                  "runpy.run_path(sys.argv[0], run_name='__main__')")
        host = "linux" if sys.platform.startswith("linux") else sys.platform
        runs = []
        for platform in [p for p in ("linux", "darwin", "win32") if p != host]:
            for mode, code in (("honest", 0), ("over", 1)):
                env = dict(os.environ, FAKE_MEMORY=mode)
                env.pop("ROCKETNET_SERVER_MAX_CLIENTS", None)
                env.pop("ROCKETNET_DEV_SERVER_LATENCY_MS", None)
                # the runs are independent: start them all, then wait
                process = subprocess.Popen([sys.executable, "-c", runner, platform, str(TOOL.parent),
                                            str(TOOL), str(self.probe)],
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                           text=True, env=env, cwd=self.dir)
                runs.append((platform, mode, code, process))
        for platform, mode, code, process in runs:
            _, stderr = process.communicate()
            self.assertEqual(process.returncode, code, (platform, mode, stderr[-800:]))

    def test_peak_over_the_plan_fails(self):
        result = self.run_tool(self.probe, "over")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("AssertionError", result.stderr)
        self.assertIn("peak_allocator_bytes", result.stderr)

    def test_zero_rows_fail(self):
        result = self.run_tool(self.probe, "silent")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("printed 0 JSON lines", result.stderr)

    def test_empty_probe_file_fails(self):
        empty = self.dir / "empty"
        empty.write_text("")
        result = self.run_tool(empty, "honest")
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
