#!/usr/bin/env python3
"""Build and run every example.

Finds the example_<name> binaries under the given build directory (and the
repository bin/ directory), runs an example made of a <name>_server and
<name>_client pair as server then client, and every other example alone.
Each pair runs in its own fresh temporary working directory on a free UDP
port it picks and passes through ROCKETNET_EXAMPLE_PORT, so two runs at once
never collide; a server that fails to bind is retried on a new port, at most
three tries.
Every example that runs alone also runs in its own fresh temporary working
directory, so files it writes (example_soak writes soak-results.tsv) never
collide between two runs.
example_soak runs with --clients 64 (SINGLE_ARGS): its default is 1000
in-process clients (examples/soak.c:121), which peaks near 8 GB of resident
memory and is killed by an 8 GB process-tree cap when two runs go at once.
At 64 clients each example stays well under 1 GB, so two runs at once stay
under 4 GB together. The 1000-client proof is the soak matrix, not this
script.
Each process gets a 120-second timeout and must exit 0 with PASS as its last
line. Prints one PASS example_<name> line per example; exits 1 on a failure.
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXAMPLES = ROOT / "examples"
TIMEOUT_SECONDS = 120
PORT_TRIES = 3
SINGLE_ARGS = {"soak": ["--clients", "64"]}


def binary_dirs(build, config):
    dirs = [build, build / "bin"]
    if config:
        dirs += [build / config, build / "bin" / config]
    dirs += [ROOT / "bin"]
    if config:
        dirs.append(ROOT / "bin" / config)
    return dirs


def find_binary(name, dirs):
    suffix = ".exe" if os.name == "nt" else ""
    for directory in dirs:
        candidate = directory / f"example_{name}{suffix}"
        if candidate.is_file():
            return candidate
    return None


def last_line(text):
    lines = [line.strip() for line in text.replace("\r\n", "\n").split("\n") if line.strip()]
    return lines[-1] if lines else ""


def failure(label, detail):
    print(f"FAIL example_{label}: {detail}", file=sys.stderr)
    return False


def check_result(label, result):
    if result.returncode != 0:
        if result.stdout:
            print(result.stdout, file=sys.stderr, end="")
        if result.stderr:
            print(result.stderr, file=sys.stderr, end="")
        return failure(label, f"exited {result.returncode}")
    if last_line(result.stdout) != "PASS":
        if result.stdout:
            print(result.stdout, file=sys.stderr, end="")
        if result.stderr:
            print(result.stderr, file=sys.stderr, end="")
        return failure(label, "did not print PASS last")
    return True


def run_single(name, binary):
    workdir = tempfile.mkdtemp(prefix=f"rocketnet_example_{name}_")
    try:
        result = subprocess.run(
            [str(binary)] + SINGLE_ARGS.get(name, []),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=TIMEOUT_SECONDS,
            cwd=workdir,
        )
    except subprocess.TimeoutExpired:
        return failure(name, f"timed out after {TIMEOUT_SECONDS}s")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    return check_result(name, result)


def kill(process):
    if process.poll() is None:
        process.kill()
    try:
        return process.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        return "", ""


def free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def run_pair(base, server_binary, client_binary):
    for attempt in range(1, PORT_TRIES + 1):
        port = free_udp_port()
        workdir = tempfile.mkdtemp(prefix=f"rocketnet_example_{base}_")
        try:
            ok = run_pair_once(base, server_binary, client_binary, port, workdir)
        finally:
            shutil.rmtree(workdir, ignore_errors=True)
        if ok != "bind":
            return ok
        again = ", picking another port" if attempt < PORT_TRIES else ""
        print(f"example_{base}: server could not bind 127.0.0.1:{port} (try {attempt} of {PORT_TRIES}){again}")
    return failure(base, f"server could not bind a free port in {PORT_TRIES} tries")


def run_pair_once(base, server_binary, client_binary, port, workdir):
    env = dict(os.environ)
    env["ROCKETNET_EXAMPLE_PORT"] = str(port)
    server = subprocess.Popen(
        [str(server_binary)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        cwd=workdir,
        env=env,
    )
    time.sleep(0.5)
    if server.poll() is not None and server.returncode != 0:
        server_out, server_err = server.communicate()
        if "FAIL: server != NULL" in server_out:
            return "bind"
        return failure(base, f"server exited {server.returncode}\n{server_out}{server_err}")
    client = subprocess.Popen(
        [str(client_binary)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        cwd=workdir,
        env=env,
    )

    try:
        client_out, client_err = client.communicate(timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        kill(client)
        kill(server)
        return failure(base, f"client timed out after {TIMEOUT_SECONDS}s")

    if client.returncode != 0:
        kill(server)
        return failure(base, f"client exited {client.returncode}\n{client_out}{client_err}")

    try:
        server_out, server_err = server.communicate(timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        kill(server)
        return failure(base, f"server timed out after {TIMEOUT_SECONDS}s")

    if server.returncode != 0:
        return failure(base, f"server exited {server.returncode}\n{server_out}{server_err}")
    if last_line(server_out) != "PASS":
        return failure(base, f"server did not print PASS last\n{server_out}{server_err}")
    if last_line(client_out) != "PASS":
        return failure(base, f"client did not print PASS last\n{client_out}{client_err}")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build", help="build directory (default: build)")
    parser.add_argument("--config", default=None, help="Windows configuration, such as Release")
    args = parser.parse_args()

    build = Path(args.build)
    if not build.is_absolute():
        build = (Path.cwd() / build).resolve()
    dirs = binary_dirs(build, args.config)

    names = {path.stem for path in EXAMPLES.glob("*.c")}
    if not names:
        print("FAIL: no examples/*.c", file=sys.stderr)
        return 1

    pair_bases = {
        name[: -len("_server")]
        for name in names
        if name.endswith("_server") and name[: -len("_server")] + "_client" in names
    }
    used = {base + suffix for base in pair_bases for suffix in ("_server", "_client")}

    groups = [(base, [base + "_server", base + "_client"]) for base in sorted(pair_bases)]
    groups += [(name, [name]) for name in sorted(names) if name not in used]

    binaries = {}
    for _, members in groups:
        for member in members:
            binary = find_binary(member, dirs)
            if binary is None:
                print(f"FAIL: example_{member} binary not found under {args.build}", file=sys.stderr)
                return 1
            binaries[member] = binary

    for base, members in groups:
        if len(members) == 2:
            ok = run_pair(base, binaries[members[0]], binaries[members[1]])
        else:
            ok = run_single(base, binaries[members[0]])
        if not ok:
            return 1
        print(f"PASS example_{base}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
