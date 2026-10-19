#!/usr/bin/env python3
"""Check that every driver row has transport matrix cells in CI.

Reads sdk/rocketnet_drivers.inc and .github/workflows/ci.yml. Each
ROCKETNET_DRIVER row names its platforms and its server and client
preferences (zero: not available for that endpoint). For every OS CI builds
(Linux, macOS, Windows) the row builds for, a server-capable driver needs a
cell in that OS's TRANSPORT_MATRIX_<OS> block with it as the server
transport, and a client-capable driver needs a cell with it as the client
transport. No driver is exempt.

Each matrix block must also sit in a step that runs the three family cases
and driver_env_pin_is_honoured, and every expected-fail mark must name a
case it runs and a card. An xdp cell needs root, a namespace and a veth
pair, so its step must run the xdp family cases inside a virtme-ng guest
(vng). Exits 0 with one OK line, or 1 naming each gap.
Standard library only.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

OSES = ("LINUX", "MACOS", "WINDOWS")

PLATFORMS = {
    "ROCKETNET_DRIVER_ALL_PLATFORMS": {"LINUX", "MACOS", "WINDOWS"},
    "ROCKETNET_DRIVER_LINUX": {"LINUX"},
    "ROCKETNET_DRIVER_IO_URING_PLATFORMS": {"LINUX"},
    "ROCKETNET_DRIVER_MAC": {"MACOS"},
    "ROCKETNET_DRIVER_WINDOWS": {"WINDOWS"},
}


CASES = (
    "test_transport_udp_loopback",
    "test_transport_ipv6_loopback",
    "test_transport_dual_stack_ipv4_client",
    "driver_env_pin_is_honoured",
)

XDP_CASES = (
    "xdp_max_size_both_ways_ipv4",
    "xdp_max_size_both_ways_ipv6",
)

ROW = re.compile(r'^ROCKETNET_DRIVER\(\s*(\w+)\s*,\s*"([^"]+)"\s*,\s*\w+\s*,\s*\w+\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)', re.M)


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


def driver_rows(text):
    rows = []
    for symbol, name, platforms, server, client in ROW.findall(text):
        rows.append((name, platforms, server != "0", client != "0"))
    return rows


def matrix_blocks(text):
    """Return {OS: [(cells, step_text)]} for each TRANSPORT_MATRIX_<OS>: | block."""
    lines = text.split("\n")
    blocks = {}
    for i, line in enumerate(lines):
        m = re.match(r"^(\s*)TRANSPORT_MATRIX_(\w+):\s*\|\s*$", line)
        if not m:
            continue
        indent = len(m.group(1))
        cells = []
        j = i + 1
        while j < len(lines) and (lines[j].strip() == "" or len(lines[j]) - len(lines[j].lstrip()) > indent):
            if lines[j].strip():
                cells.append(lines[j].split())
            j += 1
        # the step this block belongs to: from its "- name:" to the next one
        start = i
        while start > 0 and not re.match(r"^\s*- name:", lines[start]):
            start -= 1
        end = i + 1
        while end < len(lines) and not re.match(r"^\s*- name:", lines[end]) and not re.match(r"^  \S", lines[end]):
            end += 1
        blocks.setdefault(m.group(2), []).append((cells, "\n".join(lines[start:end])))
    return blocks


def main():
    rows = driver_rows(read("sdk/rocketnet_drivers.inc"))
    blocks = matrix_blocks(read(".github/workflows/ci.yml"))
    problems = []

    if not rows:
        problems.append("no ROCKETNET_DRIVER rows read from sdk/rocketnet_drivers.inc")

    for os_name in sorted(blocks):
        if os_name not in OSES:
            problems.append(f"TRANSPORT_MATRIX_{os_name} names no OS CI builds")
        for cells, step in blocks[os_name]:
            for case in CASES:
                if case not in step:
                    problems.append(f"TRANSPORT_MATRIX_{os_name}: its step does not run {case}")
            if "--run" not in step:
                problems.append(f"TRANSPORT_MATRIX_{os_name}: its step does not run cases by --run")
            if any(cell and cell[0] == "xdp" for cell in cells):
                for case in XDP_CASES:
                    if case not in step:
                        problems.append(f"TRANSPORT_MATRIX_{os_name}: it has an xdp cell but its step does not run {case}")
                if "vng --run" not in step:
                    problems.append(f"TRANSPORT_MATRIX_{os_name}: it has an xdp cell but its step does not run it inside a virtme-ng guest (vng --run)")
            for cell in cells:
                if len(cell) < 2:
                    problems.append(f"TRANSPORT_MATRIX_{os_name}: cell '{' '.join(cell)}' names no client transport")
                    continue
                for mark in cell[2:]:
                    case, _, card = mark.partition("=")
                    if case not in CASES or not card:
                        problems.append(f"TRANSPORT_MATRIX_{os_name}: cell {cell[0]}/{cell[1]} expected-fail mark '{mark}' is not <case>=<card> for a case the matrix runs")

    for name, platforms, serves, connects in rows:
        if platforms not in PLATFORMS:
            problems.append(f"driver {name}: platform set {platforms} is unknown to this check")
            continue
        for os_name in sorted(PLATFORMS[platforms]):
            cells = [cell for cells, _ in blocks.get(os_name, []) for cell in cells if len(cell) >= 2]
            if serves and not any(cell[0] == name for cell in cells):
                problems.append(f"driver {name}: no TRANSPORT_MATRIX_{os_name} cell with server transport {name}")
            if connects and not any(cell[1] == name for cell in cells):
                problems.append(f"driver {name}: no TRANSPORT_MATRIX_{os_name} cell with client transport {name}")

    for line in problems:
        print(f"TRANSPORT-MATRIX-MISSING {line}")
    if problems:
        return 1
    print(f"TRANSPORT-MATRIX OK drivers={len(rows)} cells={sum(len(c) for b in blocks.values() for c, _ in b)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
