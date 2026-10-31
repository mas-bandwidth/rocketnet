#!/usr/bin/env python3
"""Build and run every example.

Finds the example_<name> binaries under the given build directory (and the
repository bin/ directory), runs an example made of a <name>_server and
<name>_client pair as server then client, and every other example alone.
Each pair runs in its own fresh temporary working directory on a free UDP
port it picks and passes through ROCKETNET_EXAMPLE_PORT, so two runs at once
never collide. The client starts only once the server is ready: the server's
output carries its keys-written line ("server wrote <name>.keys"), or the key
file it names is complete in the working directory (two newline-terminated
lines). The second form is needed because the server's stdout is a pipe and
the C library buffers it, so that line arrives only when the buffer is
flushed. A server that exits before it is ready because it could not bind
("FAIL: server != NULL") has lost its port and is retried on a new one, at
most three tries; one not ready within READY_SECONDS fails.
Every example that runs alone also runs in its own fresh temporary working
directory, so files it writes (example_soak writes soak-results.tsv) never
collide between two runs.
example_soak runs with --clients 64 (SINGLE_ARGS): its default is 1000
in-process clients (the top of main in examples/soak.c), which peaks near
8 GB of resident memory and is killed by an 8 GB process-tree cap when two
runs go at once.
At 64 clients each example stays well under 1 GB, so two runs at once stay
under 4 GB together. The 1000-client proof is the soak matrix, not this
script.
The backend pair (example_backend_server and example_backend_client) is not
run here: its client takes a token from a backend in examples/backend/, so
tools/run_backend_examples.py runs it with each backend between the two.
Each process gets a 120-second timeout and must exit 0 with PASS as its last
line. Prints one PASS example_<name> line per example; exits 1 on a failure.
Some examples then run again in another mode (EXTRA_RUNS), each with an
exit status and a line its output must hold: example_client_server_client
--wrong-protocol must reach its expected refusal (the server, which no client
reaches, is stopped once the client exits); example_config_env runs from a
path holding a space and a $, which its child must run as it is; as its
ROCKETNET_SERVER_MAX_CLIENTS=5 child with a server create refused for another
reason it must exit 1 rather than pass as the bad-value child; and
example_soak, example_game_loop and example_client_server_server refuse
arguments out of range or not whole numbers before they create anything.
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXAMPLES = ROOT / "examples"
TIMEOUT_SECONDS = 120
PORT_TRIES = 3
READY_SECONDS = 30
SINGLE_ARGS = {"soak": ["--clients", "64"]}
# further runs. "run" is a pair (its client gets "args") or one program;
# "env" is added to the environment; "odd_path" runs the program through a
# link in a directory whose name holds a space and a $; "code" is the exit
# status and "line" the start of a line the output must hold
EXTRA_RUNS = [
    {"run": "client_server", "args": ["--wrong-protocol"], "code": 0, "line": "expected refusal: "},
    {"run": "config_env", "odd_path": True, "code": 0, "line": "PASS"},
    {"run": "config_env", "env": {"ROCKETNET_SERVER_MAX_CLIENTS": "5", "ROCKETNET_SERVER_NUM_CHANNELS": "0"},
     "code": 1, "line": "FAIL: ROCKETNET_SERVER_MAX_CLIENTS=\"5\": server create "},
    {"run": "soak", "args": ["--seconds", "35791395"], "code": 1, "line": "FAIL: --seconds takes 1 .. "},
    {"run": "soak", "args": ["--seconds", "-1"], "code": 1, "line": "FAIL: --seconds takes 1 .. "},
    {"run": "soak", "args": ["--seed", "12x"], "code": 1, "line": "FAIL: --seed takes a whole number"},
    {"run": "game_loop", "args": ["--clients", "1x"], "code": 1, "line": "usage: example_game_loop"},
    {"run": "client_server_server", "args": ["--port", "70000"], "code": 1, "line": "FAIL: the port takes 1 .. 65535"},
]
# pairs that need more than a server and a client, run by their own script
RUN_ELSEWHERE = {"backend"}  # tools/run_backend_examples.py


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
    if result is None:
        return False
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


def run_single(name, binary, args=None, environment=None, odd_path=False):
    workdir = tempfile.mkdtemp(prefix=f"rocketnet_example_{name}_")
    try:
        if odd_path:
            directory = Path(workdir) / "a dir $HOME"
            directory.mkdir()
            link = directory / binary.name
            link.symlink_to(binary)
            binary = link
        result = subprocess.run(
            [str(binary)] + (SINGLE_ARGS.get(name, []) if args is None else args),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=TIMEOUT_SECONDS,
            cwd=workdir,
            env={**os.environ, **(environment or {})},
        )
    except subprocess.TimeoutExpired:
        failure(name, f"timed out after {TIMEOUT_SECONDS}s")
        return None
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    return result


def check_extra(label, output, returncode, expected_code, expected_line):
    if returncode != expected_code:
        print(output, file=sys.stderr, end="")
        return failure(label, f"exited {returncode}, expected {expected_code}")
    if not any(line.startswith(expected_line) for line in output.splitlines()):
        print(output, file=sys.stderr, end="")
        return failure(label, f"printed no line starting {expected_line!r}")
    return True


def kill(process):
    if process.poll() is None:
        process.kill()
    try:
        return process.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        return "", ""


def stop(process):
    if process.poll() is None:
        process.kill()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        pass


class Output:
    """Reads a process stream line by line on its own thread, so the runner
    can look at what has arrived so far without blocking on the pipe."""

    def __init__(self, stream):
        self.lines = []
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self.read, args=(stream,), daemon=True)
        self.thread.start()

    def read(self, stream):
        for line in stream:
            with self.lock:
                self.lines.append(line)
        stream.close()

    def text(self):
        with self.lock:
            return "".join(self.lines)

    def join(self):
        self.thread.join(timeout=10)
        return self.text()


def keys_written(base, output, workdir):
    if any(line.startswith("server wrote ") for line in output.text().splitlines()):
        return True
    try:
        text = (Path(workdir) / f"{base}.keys").read_text()
    except OSError:
        return False
    lines = text.split("\n")
    return text.endswith("\n") and len(lines) == 3 and all(len(line.split()) == 2 for line in lines[:2])


def wait_server_ready(base, server, server_out, workdir):
    deadline = time.monotonic() + READY_SECONDS
    while time.monotonic() < deadline:
        if keys_written(base, server_out, workdir):
            return "ready"
        if server.poll() is not None:
            return "exited"
        time.sleep(0.01)
    return "timeout"


def free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def run_pair(base, server_binary, client_binary, client_args=None):
    for attempt in range(1, PORT_TRIES + 1):
        port = free_udp_port()
        workdir = tempfile.mkdtemp(prefix=f"rocketnet_example_{base}_")
        try:
            ok = run_pair_once(base, server_binary, client_binary, port, workdir, client_args)
        finally:
            shutil.rmtree(workdir, ignore_errors=True)
        if ok != "bind":
            return ok
        again = ", picking another port" if attempt < PORT_TRIES else ""
        print(f"example_{base}: server could not bind 127.0.0.1:{port} (try {attempt} of {PORT_TRIES}){again}")
    return failure(base, f"server could not bind a free port in {PORT_TRIES} tries")


def run_pair_once(base, server_binary, client_binary, port, workdir, client_args):
    """Runs the pair once. With client_args the client runs with them, the
    server is stopped once the client exits, and the client's output and exit
    status are returned for the caller to judge."""
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
    server_reader = Output(server.stdout)
    server_error_reader = Output(server.stderr)
    ready = wait_server_ready(base, server, server_reader, workdir)
    if ready != "ready":
        stop(server)
        server_out = server_reader.join()
        server_err = server_error_reader.join()
        if ready == "timeout":
            return failure(base, f"server wrote no keys within {READY_SECONDS}s\n{server_out}{server_err}")
        if server.returncode != 0 and "FAIL: server != NULL" in server_out:
            return "bind"
        return failure(base, f"server exited {server.returncode} before it wrote its keys\n{server_out}{server_err}")
    client = subprocess.Popen(
        [str(client_binary)] + (client_args or []),
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
        stop(server)
        return failure(base, f"client timed out after {TIMEOUT_SECONDS}s")

    if client_args is not None:
        stop(server)
        server_reader.join()
        server_error_reader.join()
        return client.returncode, client_out + client_err

    if client.returncode != 0:
        stop(server)
        return failure(base, f"client exited {client.returncode}\n{client_out}{client_err}")

    try:
        server.wait(timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        stop(server)
        return failure(base, f"server timed out after {TIMEOUT_SECONDS}s")
    server_out = server_reader.join()
    server_err = server_error_reader.join()

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
    pair_bases -= RUN_ELSEWHERE

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
            ok = check_result(base, run_single(base, binaries[members[0]]))
        if not ok:
            return 1
        print(f"PASS example_{base}")

    pairs = {base: members for base, members in groups if len(members) == 2}
    for extra in EXTRA_RUNS:
        name, args, environment = extra["run"], extra.get("args", []), extra.get("env", {})
        label = " ".join([name, *args, *(f"{k}={v}" for k, v in environment.items()),
                          *(["(from a path with a space and a $)"] if extra.get("odd_path") else [])])
        if name in pairs:
            assert not environment and not extra.get("odd_path"), "a pair's further run takes client arguments only"
            outcome = run_pair(name, binaries[pairs[name][0]], binaries[pairs[name][1]], args)
            if outcome is False:
                return 1
            returncode, output = outcome
        else:
            result = run_single(name, binaries[name], args, environment, extra.get("odd_path", False))
            if result is None:
                return 1
            returncode, output = result.returncode, result.stdout + result.stderr
        if not check_extra(label, output, returncode, extra["code"], extra["line"]):
            return 1
        print(f"PASS example_{label}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
