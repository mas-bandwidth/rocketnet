#!/usr/bin/env python3
"""Read a space bot-swarm run against the release and say whether it held.

tools/space_release_soak.sh builds the integrating application
(robotgamedevelopers/space, private) against the rocketnet release and runs
its `scripts/bot_swarm.sh N D`: the backend and the dedicated server come up
through dist/happy_path, N headless bots connect, and the swarm is held for D
seconds once every bot is connected. This reads what that run left behind:

  --server-log    space's logs/server, the dedicated server's own log
  --swarm-output  everything scripts/bot_swarm.sh printed

The dedicated server (game/Apps/Server/Server.cpp) pegs its loop to real time
and logs `server dropped %d frames on frame #%lld` whenever a frame overruns,
`large hitch detected!` when five or more drop at once, `client %d connected`
for each connect, and `shutting down` when it is stopped at the end of the
hold. Every line carries a `[YYYY-MM-DD HH:MM:SS]` stamp.

The run fails on any of these, and every one that holds is named:

  dropped-frames            any dropped-frame line, from the first frame on.
                            There is no warm-up window: a real-time loop that
                            drops a frame at start is a finding, not noise.
  large-hitch               any large hitch line
  server-exit-before-bound  the server log has no `shutting down` line (it
                            crashed or was killed), carries a segmentation
                            fault, or shut down less than D seconds after the
                            last client connected
  bots-fail                 the swarm printed a `BOTS FAIL` line
  no-bots-summary           the swarm printed no `BOTS connected=` line at
                            all (the stack never came up, or the build is not
                            the swarm's)
  bots-count                the summary names another bot count than --bots
  no-client-connected       the server log has no `client %d connected` line

Exit 0 when none holds, 1 otherwise. The verdict is one line carrying the
total frames dropped, the number of dropped-frame lines, the hitch count, the
clients the server saw connect and the swarm's BOTS line; the dropped-frame
and hitch lines themselves follow on stderr, every one of them.

Stdlib only; it reads two files and opens nothing else.
"""

import argparse
import datetime
from pathlib import Path
import re
import sys

STAMP = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\]")
DROPPED = re.compile(r"server dropped (\d+) frames on frame #(-?\d+)")
HITCH = re.compile(r"large hitch detected!")
CONNECTED = re.compile(r"\bclient (\d+) connected\b")
SHUTDOWN = re.compile(r"\bshutting down\b")
SEGFAULT = re.compile(r"Segmentation")
BOTS_SUMMARY = re.compile(r"^BOTS connected=(\d+)\b.*$")
BOTS_FAIL = re.compile(r"^BOTS FAIL\b.*$")


def stamp_of(line):
    """The line's timestamp, or None when it carries none."""
    match = STAMP.match(line)
    if match is None:
        return None
    return datetime.datetime.strptime(match.group(1), "%Y-%m-%d %H:%M:%S")


class Verdict:
    """What the two logs say, and the one line that says it."""

    def __init__(self):
        self.reasons = []
        self.dropped_frames = 0
        self.dropped_frame_lines = []
        self.hitch_lines = []
        self.clients_connected = 0
        self.held_seconds = None
        self.bots_line = "-"
        self.details = []

    @property
    def ok(self):
        return not self.reasons

    def line(self):
        held = "-" if self.held_seconds is None else str(self.held_seconds)
        text = (
            f"verdict: {'pass' if self.ok else 'fail'} "
            f"dropped_frames={self.dropped_frames} "
            f"dropped_frame_lines={len(self.dropped_frame_lines)} "
            f"hitches={len(self.hitch_lines)} "
            f"clients_connected={self.clients_connected} "
            f"held_seconds={held} "
            f'bots="{self.bots_line}"'
        )
        if not self.ok:
            text += f" reasons={','.join(self.reasons)}"
        return text


def check(server_log, swarm_output, seconds, bots=None):
    """Read the server log and the swarm output; return the Verdict."""
    verdict = Verdict()

    last_connect = None
    shutdown = None
    segfaults = 0
    for line in server_log.splitlines():
        match = DROPPED.search(line)
        if match is not None:
            verdict.dropped_frames += int(match.group(1))
            verdict.dropped_frame_lines.append(line)
        if HITCH.search(line):
            verdict.hitch_lines.append(line)
        if CONNECTED.search(line):
            verdict.clients_connected += 1
            stamp = stamp_of(line)
            if stamp is not None:
                last_connect = stamp
        if SHUTDOWN.search(line) and shutdown is None:
            shutdown = stamp_of(line)
            if shutdown is None:
                shutdown = datetime.datetime.min
        if SEGFAULT.search(line):
            segfaults += 1

    summary = None
    fail_line = None
    for line in swarm_output.splitlines():
        if summary is None and BOTS_SUMMARY.match(line):
            summary = line.strip()
        if fail_line is None and BOTS_FAIL.match(line):
            fail_line = line.strip()
    if summary is not None:
        verdict.bots_line = summary
    if fail_line is not None:
        verdict.bots_line = fail_line if summary is None else f"{summary}; {fail_line}"

    if verdict.dropped_frame_lines:
        verdict.reasons.append("dropped-frames")
    if verdict.hitch_lines:
        verdict.reasons.append("large-hitch")

    # The bound: the server must still be running D seconds after the last
    # connect, which is when the hold began. Stamps are whole seconds, so a
    # true span of D or more shows as D - 1 or more; that is the stamp's
    # resolution, not a tolerance.
    if shutdown is None:
        verdict.details.append("the server log has no `shutting down` line: the server crashed or was killed")
        verdict.reasons.append("server-exit-before-bound")
    elif segfaults:
        verdict.details.append(f"the server log carries {segfaults} segmentation line(s)")
        verdict.reasons.append("server-exit-before-bound")
    elif last_connect is not None and shutdown != datetime.datetime.min:
        held = int((shutdown - last_connect).total_seconds())
        verdict.held_seconds = held
        if held < seconds - 1:
            verdict.details.append(
                f"the server shut down {held}s after the last client connected; the bound is {seconds}s"
            )
            verdict.reasons.append("server-exit-before-bound")

    if fail_line is not None:
        verdict.reasons.append("bots-fail")
    elif summary is None:
        verdict.details.append("the swarm printed no BOTS summary line")
        verdict.reasons.append("no-bots-summary")
    elif bots is not None:
        count = int(BOTS_SUMMARY.match(summary).group(1))
        if count != bots:
            verdict.details.append(f"the swarm connected {count} bots, and the run asked for {bots}")
            verdict.reasons.append("bots-count")

    if verdict.clients_connected == 0:
        verdict.reasons.append("no-client-connected")

    for line in verdict.dropped_frame_lines:
        verdict.details.append(f"dropped: {line}")
    for line in verdict.hitch_lines:
        verdict.details.append(f"hitch: {line}")
    return verdict


def read_text(path):
    """A log's text; a missing log reads as empty, which the checks then name."""
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--server-log", type=Path, required=True, help="space's logs/server")
    parser.add_argument("--swarm-output", type=Path, required=True, help="what scripts/bot_swarm.sh printed")
    parser.add_argument("--seconds", type=int, required=True, help="the hold the swarm was asked for, D")
    parser.add_argument("--bots", type=int, help="the bot count the swarm was asked for, N")
    args = parser.parse_args(argv)

    server_log = read_text(args.server_log)
    if not server_log:
        print(f"the server log {args.server_log} is missing or empty", file=sys.stderr)
    verdict = check(server_log, read_text(args.swarm_output), args.seconds, args.bots)
    print(verdict.line())
    sys.stdout.flush()
    for detail in verdict.details:
        print(detail, file=sys.stderr)
    return 0 if verdict.ok else 1


if __name__ == "__main__":
    sys.exit(main())
