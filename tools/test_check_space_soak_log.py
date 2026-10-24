#!/usr/bin/env python3
"""Tests for the space soak log check in tools/check_space_soak_log.py.

Every case is a canned server log and a canned swarm output, written in the
shape space's dedicated server and scripts/bot_swarm.sh print them, into a
temporary directory the test owns. No case opens a socket, reads a clock,
starts a thread or runs a process: the check is called through its own main
with the two files as arguments.

The cases are:

  test_clean_log_passes                 eight clients connect, the hold runs
                                        its full bound, nothing drops: pass
  test_dropped_frames_line_fails        one dropped-frame line, on the very
                                        first frames: fail, no warm-up window
  test_large_hitch_fails                a large hitch line: fail
  test_server_exit_before_bound_fails   the server shuts down before the
                                        bound, and, separately, the server log
                                        stops with no shutdown at all: fail
  test_bots_fail_line_fails             the swarm printed BOTS FAIL: fail
  test_no_client_connected_fails        the server saw no client connect: fail
"""

import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_space_soak_log

SECONDS = 90
BOTS = 8

HEAD = [
    "[2026-10-24 00:43:21] [launcher] running in dedicated server mode",
    "[2026-10-24 00:43:21] [server] rocketnet version is dev",
    "[2026-10-24 00:43:21] [server] starting server with 100 players",
]

CONNECTS = [f"[2026-10-24 00:43:3{index}] [server] client {index} connected" for index in range(BOTS)]

HOLD = [
    "[2026-10-24 00:44:15] [server] PARITY frames=60 net_avg_us=81.0 wall_max_us=16666.9",
    "[2026-10-24 00:44:45] [server] PARITY frames=61 net_avg_us=86.4 wall_max_us=16667.0",
]

# The last connect is 00:43:37; a 90 second hold ends at 00:45:07.
SHUTDOWN = [
    "[2026-10-24 00:45:08] ",
    "[2026-10-24 00:45:08] [server] shutting down",
    "[2026-10-24 00:45:08] [server] destroy all objects and allocations before terminating rocketnet",
]

SWARM_OK = "\n".join([
    f"BOTS connected={BOTS} update_config={BOTS} disconnects=0 duration={SECONDS}",
]) + "\n"


def server_log(*parts):
    lines = []
    for part in parts:
        lines.extend(part)
    return "\n".join(lines) + "\n"


class CheckSpaceSoakLogTest(unittest.TestCase):

    def run_check(self, server_text, swarm_text):
        """Write the two logs, run the check's main; return (code, stdout, stderr)."""
        with tempfile.TemporaryDirectory() as directory:
            server_path = Path(directory) / "server"
            swarm_path = Path(directory) / "swarm"
            server_path.write_text(server_text, encoding="utf-8")
            swarm_path.write_text(swarm_text, encoding="utf-8")
            out = io.StringIO()
            err = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = check_space_soak_log.main([
                    "--server-log", str(server_path),
                    "--swarm-output", str(swarm_path),
                    "--seconds", str(SECONDS),
                    "--bots", str(BOTS),
                ])
            return code, out.getvalue(), err.getvalue()

    def assert_one_verdict_line(self, out):
        lines = out.splitlines()
        self.assertEqual(len(lines), 1, out)
        self.assertTrue(lines[0].startswith("verdict: "), out)
        return lines[0]

    def test_clean_log_passes(self):
        code, out, _ = self.run_check(server_log(HEAD, CONNECTS, HOLD, SHUTDOWN), SWARM_OK)
        line = self.assert_one_verdict_line(out)
        self.assertEqual(code, 0, out)
        self.assertTrue(line.startswith("verdict: pass "), line)
        self.assertIn("dropped_frames=0", line)
        self.assertIn("hitches=0", line)
        self.assertIn(f"clients_connected={BOTS}", line)
        self.assertIn(f'bots="BOTS connected={BOTS} update_config={BOTS} disconnects=0 duration={SECONDS}"', line)
        self.assertNotIn("reasons=", line)

    def test_dropped_frames_line_fails(self):
        # On frame #1: the first frames count exactly as much as the rest.
        dropped = ["[2026-10-24 00:43:22] [server] server dropped 1 frames on frame #1"]
        code, out, err = self.run_check(server_log(HEAD, dropped, CONNECTS, HOLD, SHUTDOWN), SWARM_OK)
        line = self.assert_one_verdict_line(out)
        self.assertEqual(code, 1, out)
        self.assertIn("verdict: fail", line)
        self.assertIn("dropped_frames=1", line)
        self.assertIn("dropped_frame_lines=1", line)
        self.assertIn("reasons=dropped-frames", line)
        self.assertIn("server dropped 1 frames on frame #1", err)

    def test_large_hitch_fails(self):
        hitch = [
            "[2026-10-24 00:44:20] [server] server dropped 5 frames on frame #3540",
            "[2026-10-24 00:44:20] [server] large hitch detected!",
        ]
        code, out, err = self.run_check(server_log(HEAD, CONNECTS, hitch, HOLD, SHUTDOWN), SWARM_OK)
        line = self.assert_one_verdict_line(out)
        self.assertEqual(code, 1, out)
        self.assertIn("hitches=1", line)
        self.assertIn("dropped_frames=5", line)
        self.assertIn("large-hitch", line.split("reasons=")[1].split(","))
        self.assertIn("large hitch detected!", err)

    def test_server_exit_before_bound_fails(self):
        early = [
            "[2026-10-24 00:44:00] [server] shutting down",
        ]
        code, out, _ = self.run_check(server_log(HEAD, CONNECTS, early), SWARM_OK)
        line = self.assert_one_verdict_line(out)
        self.assertEqual(code, 1, out)
        self.assertIn("reasons=server-exit-before-bound", line)

        # No shutdown line at all: the server died, and the log just stops.
        code, out, err = self.run_check(server_log(HEAD, CONNECTS, HOLD), SWARM_OK)
        line = self.assert_one_verdict_line(out)
        self.assertEqual(code, 1, out)
        self.assertIn("reasons=server-exit-before-bound", line)
        self.assertIn("crashed or was killed", err)

    def test_bots_fail_line_fails(self):
        swarm = (
            f"BOTS connected={BOTS} update_config={BOTS} disconnects=1 duration={SECONDS}\n"
            "BOTS FAIL check=disconnects bot=3\n"
        )
        code, out, _ = self.run_check(server_log(HEAD, CONNECTS, HOLD, SHUTDOWN), swarm)
        line = self.assert_one_verdict_line(out)
        self.assertEqual(code, 1, out)
        self.assertIn("BOTS FAIL check=disconnects", line)
        self.assertIn("reasons=bots-fail", line)

    def test_no_client_connected_fails(self):
        swarm = "BOTS FAIL check=connected\n"
        code, out, _ = self.run_check(server_log(HEAD, HOLD, SHUTDOWN), swarm)
        line = self.assert_one_verdict_line(out)
        self.assertEqual(code, 1, out)
        self.assertIn("clients_connected=0", line)
        self.assertIn("no-client-connected", line.split("reasons=")[1].split(","))


if __name__ == "__main__":
    unittest.main()
