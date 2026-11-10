#!/usr/bin/env python3
"""Check that every transport row has transport matrix cells in CI.

Reads sdk/rocketnet_drivers.inc and .github/workflows/ci.yml. Each
ROCKETNET_TRANSPORT row names its platforms and its server and client
preferences (zero: not available for that endpoint). For every OS CI builds
(Linux, macOS, Windows) the row builds for, a server-capable transport needs a
cell in that OS's TRANSPORT_MATRIX_<OS> block with it as the server
transport, and a client-capable transport needs a cell with it as the client
transport. No transport is exempt.

Each matrix block must also sit in a step that runs the three family cases
and test_transport_env_pin_is_honoured, and every expected-fail mark must name a
case it runs and a card. An xdp cell needs root, a namespace and a veth
pair, so its step must run the xdp family cases inside a virtme-ng guest
(vng). Each matrix step's script is also run on a fake functional binary:
a cell that dies before writing its result files must fail the step by
name, and an io_uring cell the kernel gates off must be reported as an
explicit SKIP with its reason; an unrun cell is never dropped silently.
Exits 0 with one OK line, or 1 naming each gap.

Also renders the auto order per platform and endpoint from
sdk/rocketnet_drivers.inc (server development, server production and client;
lower nonzero rank first, pin-only excluded) and fails if sdk/rocketnet.h,
docs/USAGE.md or docs/TRANSPORTS.md state a different order.
Standard library only.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

OSES = ("LINUX", "MACOS", "WINDOWS")

PLATFORMS = {
    "ROCKETNET_TRANSPORT_ALL_PLATFORMS": {"LINUX", "MACOS", "WINDOWS"},
    "ROCKETNET_TRANSPORT_LINUX": {"LINUX"},
    "ROCKETNET_TRANSPORT_IO_URING_PLATFORMS": {"LINUX"},
    "ROCKETNET_TRANSPORT_MAC": {"MACOS"},
    "ROCKETNET_TRANSPORT_WINDOWS": {"WINDOWS"},
}


CASES = (
    "test_loopback_transport_udp",
    "test_loopback_transport_ipv6",
    "test_transport_dual_stack_ipv4_client",
    "test_transport_env_pin_is_honoured",
)

XDP_CASES = (
    "test_xdp_transport_max_size_both_ways_ipv4",
    "test_xdp_transport_max_size_both_ways_ipv6",
)

ROW = re.compile(r'^ROCKETNET_TRANSPORT\(\s*(\w+)\s*,\s*"([^"]+)"\s*,\s*\w+\s*,\s*\w+\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)', re.M)

ROW_FULL = re.compile(r'^ROCKETNET_TRANSPORT\(\s*(\w+)\s*,\s*"([^"]+)"\s*,\s*\w+\s*,\s*\w+\s*,\s*(\w+)\s*,\s*(\([^)]*\)|\w+)\s*,\s*(\w+)\s*\)', re.M)

OS_LABELS = {"LINUX": "Linux", "MACOS": "macOS", "WINDOWS": "Windows"}
ENDPOINTS = ("server-development", "server-production", "client")


def rank_value(expr, development):
    if expr == "ROCKETNET_TRANSPORT_PIN_ONLY":
        return None
    if expr.startswith("("):
        numbers = re.findall(r"\d+", expr)
        if len(numbers) != 2:
            return None
        return int(numbers[0] if development else numbers[1])
    return int(expr)


def auto_orders(text):
    """Return {(OS, endpoint): [names]} rendered from the registry rows."""
    parsed = []
    for _symbol, name, platforms, server, client in ROW_FULL.findall(text):
        parsed.append((name, platforms, server, client))
    orders = {}
    for os_name in OSES:
        label = OS_LABELS[os_name]
        for endpoint in ENDPOINTS:
            development = endpoint != "server-production"
            ranked = []
            for index, (name, platforms, server, client) in enumerate(parsed):
                if platforms not in PLATFORMS or os_name not in PLATFORMS[platforms]:
                    continue
                expr = client if endpoint == "client" else server
                rank = rank_value(expr, development)
                if rank is None or rank == 0:
                    continue
                ranked.append((rank, index, name))
            ranked.sort(key=lambda item: (item[0], item[1]))
            orders[(label, endpoint)] = [name for _rank, _index, name in ranked]
    return orders


def norm_order(text):
    return ",".join(part.strip().strip("`") for part in text.strip().strip("`").split(","))


def norm_doc(text):
    return re.sub(r"\s+", " ", text.replace("`", ""))


def check_doc_orders(orders, problems):
    expected = {(label, ep): ",".join(names) for (label, ep), names in orders.items()}

    def compare(doc, label, endpoint, got):
        want = expected[(label, endpoint)]
        if got is None:
            problems.append(f"{doc}: no documented auto order for {label} {endpoint} (registry: {want})")
        elif got != want:
            problems.append(f"{doc}: {label} {endpoint} auto order is '{got}', registry renders '{want}'")

    # sdk/rocketnet.h: Development/Production servers try ...; Linux/macOS/Windows clients try ...
    header = norm_doc(read("sdk/rocketnet.h").replace("///", ""))
    for kind, endpoint in (("Development", "server-development"), ("Production", "server-production")):
        m = re.search(kind + r" servers try (.+?) on Linux; (.+?) on macOS; and (.+?) on Windows", header)
        groups = m.groups() if m else (None, None, None)
        for label, got in zip(("Linux", "macOS", "Windows"), groups):
            compare("sdk/rocketnet.h", label, endpoint, norm_order(got) if got else None)
    m = re.search(r"Linux clients try (.+?); macOS clients try (.+?); and Windows clients try (.+?)\.", header)
    groups = m.groups() if m else (None, None, None)
    for label, got in zip(("Linux", "macOS", "Windows"), groups):
        compare("sdk/rocketnet.h", label, "client", norm_order(got) if got else None)

    # docs/USAGE.md: server prod sentence with dev in parentheses (dev differs only on Windows), then client sentence.
    usage = norm_doc(read("docs/USAGE.md"))
    m = re.search(r"The server's auto order is (.+?) on Linux; (.+?) on macOS; and (.+?) on Windows in production builds \((.+?) in development builds\)", usage)
    if not m:
        for label in ("Linux", "macOS", "Windows"):
            compare("docs/USAGE.md", label, "server-production", None)
            compare("docs/USAGE.md", label, "server-development", None)
    else:
        prod_linux, prod_macos, prod_windows, dev_windows = (norm_order(g) for g in m.groups())
        compare("docs/USAGE.md", "Linux", "server-production", prod_linux)
        compare("docs/USAGE.md", "macOS", "server-production", prod_macos)
        compare("docs/USAGE.md", "Windows", "server-production", prod_windows)
        compare("docs/USAGE.md", "Linux", "server-development", prod_linux)
        compare("docs/USAGE.md", "macOS", "server-development", prod_macos)
        compare("docs/USAGE.md", "Windows", "server-development", dev_windows)
    m = re.search(r"The client's auto order is [^.]*?: (.+?) on Linux; (.+?) on macOS; (.+?) on Windows\.", usage)
    groups = m.groups() if m else (None, None, None)
    for label, got in zip(("Linux", "macOS", "Windows"), groups):
        compare("docs/USAGE.md", label, "client", norm_order(got) if got else None)

    # docs/TRANSPORTS.md: the `auto` resolves table, one row per platform.
    found = {}
    for label, dev, prod, client in re.findall(r"^\| (Linux|macOS|Windows) \| (.+?) \| (.+?) \| (.+?) \|", read("docs/TRANSPORTS.md"), re.M):
        found[label] = (norm_order(dev), norm_order(prod), norm_order(client))
    for label in ("Linux", "macOS", "Windows"):
        if label not in found:
            for endpoint in ENDPOINTS:
                compare("docs/TRANSPORTS.md", label, endpoint, None)
        else:
            dev, prod, client = found[label]
            compare("docs/TRANSPORTS.md", label, "server-development", dev)
            compare("docs/TRANSPORTS.md", label, "server-production", prod)
            compare("docs/TRANSPORTS.md", label, "client", client)


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


def transport_rows(text):
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


FAKE_FUNCTIONAL = """#!/usr/bin/env bash
# the matrix step's functional binary, faked: it passes every case it is run
# with, except that a server of $FAKE_CRASH_SERVER kills its cell's subshell
# before the cell can write a result file, as a crashing cell would
if [ "$ROCKETNET_SERVER_TRANSPORT" = "${FAKE_CRASH_SERVER:-}" ]; then kill -9 "$PPID"; exit 1; fi
echo "$2"
"""


def step_script(step):
    """The run: | script of a matrix step, dedented, or None."""
    lines = step.split("\n")
    for i, line in enumerate(lines):
        m = re.match(r"^(\s*)run:\s*\|\s*$", line)
        if not m:
            continue
        body = []
        for later in lines[i + 1:]:
            if later.strip() and len(later) - len(later.lstrip()) <= len(m.group(1)):
                break
            body.append(later)
        indent = min(len(b) - len(b.lstrip()) for b in body if b.strip())
        return "\n".join(b[indent:] for b in body) + "\n"
    return None


def run_step(script, matrix, uring_disabled, crash_server=""):
    """Run a matrix step's script on a fake functional binary; (exit, output)."""
    import subprocess
    import tempfile
    with tempfile.TemporaryDirectory() as work:
        for path in ("bin/functional", "bin/Release/functional.exe"):
            os.makedirs(os.path.dirname(os.path.join(work, path)), exist_ok=True)
            with open(os.path.join(work, path), "w", encoding="utf-8") as f:
                f.write(FAKE_FUNCTIONAL)
            os.chmod(os.path.join(work, path), 0o755)
        uring = os.path.join(work, "io_uring_disabled")
        with open(uring, "w", encoding="utf-8") as f:
            f.write("2\n" if uring_disabled else "0\n")
        env = dict(os.environ, RUNNER_OS="Linux", FAKE_CRASH_SERVER=crash_server,
                   TRANSPORT_MATRIX_LINUX=matrix, TRANSPORT_MATRIX_MACOS=matrix, TRANSPORT_MATRIX_WINDOWS=matrix)
        done = subprocess.run(["bash", "-c", script.replace("/proc/sys/kernel/io_uring_disabled", uring)],
                              cwd=work, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        return done.returncode, done.stdout


def check_unrun_cells(os_name, step, problems):
    """An unrun cell is never dropped from the verdict: a cell that dies before
    writing its result files fails the step, and a gated io_uring cell is
    reported as an explicit skip with its reason."""
    script = step_script(step)
    if script is None or "/proc/sys/kernel/io_uring_disabled" not in script:
        problems.append(f"TRANSPORT_MATRIX_{os_name}: its step has no run: script reading /proc/sys/kernel/io_uring_disabled for this check to drive")
        return
    rc, out = run_step(script, "portable portable\ngso gso\n", uring_disabled=False, crash_server="gso")
    if rc == 0 or "server=gso client=gso" not in out or "PASS server=portable client=portable" not in out:
        problems.append(f"TRANSPORT_MATRIX_{os_name}: a cell that died before writing its result files was not failed by name (exit {rc}): {out.strip()!r}")
    rc, out = run_step(script, "portable portable\nio_uring portable\n", uring_disabled=True)
    if rc != 0 or not re.search(r"^SKIP server=io_uring client=portable \S+: .*io_uring", out, re.M):
        problems.append(f"TRANSPORT_MATRIX_{os_name}: an io_uring cell gated off by the kernel was not reported as an explicit SKIP with its reason (exit {rc}): {out.strip()!r}")


def main():
    rows = transport_rows(read("sdk/rocketnet_drivers.inc"))
    blocks = matrix_blocks(read(".github/workflows/ci.yml"))
    problems = []

    if not rows:
        problems.append("no ROCKETNET_TRANSPORT rows read from sdk/rocketnet_drivers.inc")

    driven = set()
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
            if step not in driven:
                driven.add(step)
                check_unrun_cells(os_name, step, problems)
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
            problems.append(f"transport {name}: platform set {platforms} is unknown to this check")
            continue
        for os_name in sorted(PLATFORMS[platforms]):
            cells = [cell for cells, _ in blocks.get(os_name, []) for cell in cells if len(cell) >= 2]
            if serves and not any(cell[0] == name for cell in cells):
                problems.append(f"transport {name}: no TRANSPORT_MATRIX_{os_name} cell with server transport {name}")
            if connects and not any(cell[1] == name for cell in cells):
                problems.append(f"transport {name}: no TRANSPORT_MATRIX_{os_name} cell with client transport {name}")

    orders = auto_orders(read("sdk/rocketnet_drivers.inc"))
    for label in ("Linux", "macOS", "Windows"):
        for endpoint in ENDPOINTS:
            print(f"AUTO-ORDER {label} {endpoint}: {','.join(orders[(label, endpoint)])}")
    check_doc_orders(orders, problems)

    for line in problems:
        print(f"TRANSPORT-MATRIX-MISSING {line}")
    if problems:
        return 1
    print(f"TRANSPORT-MATRIX OK transports={len(rows)} cells={sum(len(c) for b in blocks.values() for c, _ in b)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
