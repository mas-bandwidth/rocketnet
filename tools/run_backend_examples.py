#!/usr/bin/env python3
"""Run the backend examples end to end, once per backend (Go and C#).

For each backend in examples/backend/:

1. Key generation. Each backend emits its own fresh signing keys with --keygen.
   They are passed through the environment, never written to the receipt.

2. Token check. The backend mints one token (--mint) for the
   development server key, and backend_check_token runs the C library's own
   checks on it: signature against the generated backend public key, the sealed
   block opened with the development server keypair, the inner fields
   consistent, and the public client's pre-send checks.

3. End to end, over loopback. example_backend_server starts on a free UDP
   port and writes backend.env (its address and public key); the backend
   starts on a free TCP port with an empty registry; the harness registers the
   live server over HTTP; the token then comes from GET /match/<client id>;
   example_backend_client connects with it, sends a
   message and receives the echo. Server and client must both exit 0 with
   PASS as their last line. A server or backend that exits before it is
   ready because another process took its port is started again on another
   port, at most PORT_TRIES times.

This is the functional tier: it uses sockets and the wall clock. The unit
tests of the backends are `go test` in examples/backend/go and
`dotnet run --project Tests` in examples/backend/csharp. Each backend is
built before it runs, so the timing below is the run, not the compile.
Every process gets a 60-second timeout and startup gets READY_SECONDS unless
ROCKETNET_NIGHTLY_NO_TIMEOUT=1. Per-request and termination cleanup waits stay bounded.

    python3 tools/run_backend_examples.py --build build [--backend go|csharp]
"""

import argparse
import base64
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

from nightly_timeout import execution_timeout, no_execution_timeout

ROOT = Path(__file__).resolve().parents[1]
BACKEND = ROOT / "examples" / "backend"
TIMEOUT_SECONDS = 60
READY_SECONDS = 30
PROTOCOL_ID = "0x52434b544e455431"  # EXAMPLE_PROTOCOL_ID, examples/dev_keys.h
CLIENT_ID = 1234567890123

# the published development server public key (sdk/rocketnet_connect_token.cpp),
# whose secret half backend_check_token opens the sealed block with
DEV_SERVER_PUBLIC_KEY = bytes.fromhex("3d2490889d374c296fab1d5770cb71476a06f74ef2f2540cf2215f7614f4801c")


def find_binary(name, build, config):
    suffix = ".exe" if os.name == "nt" else ""
    dirs = [build, build / "bin"]
    if config:
        dirs += [build / config, build / "bin" / config]
    dirs += [ROOT / "bin"] + ([ROOT / "bin" / config] if config else [])
    for directory in dirs:
        candidate = directory / f"{name}{suffix}"
        if candidate.is_file():
            return candidate
    return None


def fail(label, detail):
    print(f"FAIL {label}: {detail}", file=sys.stderr)
    return False


def last_line(text):
    lines = [line.strip() for line in text.replace("\r\n", "\n").split("\n") if line.strip()]
    return lines[-1] if lines else ""


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


# A port free when probed can be taken before the program binds it. The
# example server then prints "FAIL: server start: ..." and the backends print
# the operating system's address-in-use error; either one exits before it is
# ready, and is started again on another port, at most PORT_TRIES times.
PORT_TRIES = 3
SERVER_BIND_FAILURE = "FAIL: server start: "
BACKEND_BIND_FAILURES = ("address already in use", "only one usage of each socket address")


def start_on_free_port(label, what, kind, launch, ready, lost_port):
    """Starts a program on a free port and waits for it to be ready.

    launch(port) starts it, ready(port) is its readiness value, and
    lost_port(output) says an early exit was a port lost to another process.
    Returns (process, port, value), or None after printing why."""
    for attempt in range(1, PORT_TRIES + 1):
        port = free_port(kind)
        process = launch(port)
        value = wait_for(lambda: ready(port), process, READY_SECONDS)
        if value:
            return process, port, value
        exited = process.poll() is not None
        out, _ = stop(process)
        out = out or ""
        if exited and process.returncode != 0 and lost_port(out):
            again = ", picking another port" if attempt < PORT_TRIES else ""
            print(f"{label}: {what} could not bind 127.0.0.1:{port} (try {attempt} of {PORT_TRIES}){again}")
            continue
        state = f"exited {process.returncode}" if exited else f"was not ready within {READY_SECONDS}s"
        fail(label, f"{what} {state} on 127.0.0.1:{port}\n{out}")
        return None
    fail(label, f"{what} could not bind a free port in {PORT_TRIES} tries")
    return None


def server_lost_port(output):
    return any(line.startswith(SERVER_BIND_FAILURE) for line in output.splitlines())


def backend_lost_port(output):
    folded = output.casefold()
    return any(text in folded for text in BACKEND_BIND_FAILURES)


def backend_environment(extra):
    # No ambient RocketNet override may change transport, keys or token policy.
    env = {k: v for k, v in os.environ.items() if not k.startswith("ROCKETNET_")}
    env.update(extra)
    return env


def build_backend(name, outdir):
    """Builds one backend and returns the command line that runs it."""
    if name == "go":
        binary = outdir / ("rocketnet-backend-go" + (".exe" if os.name == "nt" else ""))
        subprocess.run(["go", "build", "-o", str(binary), "."], cwd=BACKEND / "go", check=True, timeout=execution_timeout(TIMEOUT_SECONDS))
        return [str(binary)]
    output = outdir / "csharp"
    subprocess.run(["dotnet", "build", "Backend", "-c", "Release", "-o", str(output), "--nologo", "-v", "q"],
                   cwd=BACKEND / "csharp", check=True, timeout=execution_timeout(TIMEOUT_SECONDS), stdout=subprocess.DEVNULL)
    return ["dotnet", str(output / "rocketnet-backend.dll")]


def generate_keys(name, command):
    generated = subprocess.run(command + ["--keygen"], env=backend_environment({}),
                               capture_output=True, text=True, timeout=execution_timeout(TIMEOUT_SECONDS))
    if generated.returncode != 0:
        fail(f"backend_{name}_keygen", f"keygen exited {generated.returncode}")
        return None
    lines = generated.stdout.splitlines()
    names = ("ROCKETNET_SERVER_BACKEND_PUBLIC_KEY", "ROCKETNET_BACKEND_PRIVATE_KEY")
    try:
        values = dict(line.split("=", 1) for line in lines)
        if len(lines) != 2 or set(values) != set(names):
            raise ValueError("expected two environment assignments")
        public = base64.b64decode(values[names[0]], validate=True)
        private = base64.b64decode(values[names[1]], validate=True)
        if len(public) != 32 or len(private) != 64 or private[32:] != public:
            raise ValueError("invalid Ed25519 keypair encoding")
    except (ValueError, TypeError):
        fail(f"backend_{name}_keygen", "invalid key output (contents omitted)")
        return None
    return values


def check_token(name, command, check_binary, keys):
    label = f"backend_{name}_token"
    env = backend_environment({
        "ROCKETNET_BACKEND_SERVER_PUBLIC_KEY": base64.b64encode(DEV_SERVER_PUBLIC_KEY).decode(),
        "ROCKETNET_BACKEND_SERVER_ADDRESSES": "127.0.0.1:40000,[::1]:40000",
        "ROCKETNET_BACKEND_PRIVATE_KEY": keys["ROCKETNET_BACKEND_PRIVATE_KEY"],
    })
    minted = subprocess.run(command + ["--mint", str(CLIENT_ID)], env=env, capture_output=True, text=True, timeout=execution_timeout(TIMEOUT_SECONDS))
    if minted.returncode != 0:
        return fail(label, f"mint exited {minted.returncode}\n{minted.stdout}{minted.stderr}")
    checked = subprocess.run([str(check_binary), str(CLIENT_ID), PROTOCOL_ID, keys["ROCKETNET_SERVER_BACKEND_PUBLIC_KEY"]], input=minted.stdout,
                             capture_output=True, text=True, timeout=execution_timeout(TIMEOUT_SECONDS))
    if checked.returncode != 0 or last_line(checked.stdout) != "PASS":
        return fail(label, f"backend_check_token exited {checked.returncode}\n{checked.stdout}{checked.stderr}")
    return True


def stop(process):
    if process.poll() is None:
        process.kill()
    try:
        return process.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        return "", ""


def wait_for(condition, process, seconds):
    deadline = time.monotonic() + seconds
    while no_execution_timeout() or time.monotonic() < deadline:
        value = condition()
        if value:
            return value
        if process.poll() is not None:
            return None
        time.sleep(0.05)
    return None


def read_env_file(path):
    try:
        text = path.read_text()
    except OSError:
        return None
    if not text.endswith("\n"):
        return None
    values = dict(line.split("=", 1) for line in text.splitlines() if "=" in line)
    keys = ("ROCKETNET_BACKEND_SERVER_ADDRESSES", "ROCKETNET_BACKEND_SERVER_PUBLIC_KEY")
    return values if all(values.get(key) for key in keys) else None


def fetch_token(url):
    try:
        with urllib.request.urlopen(url, timeout=2) as response:
            return response.read().decode().strip() if response.status == 200 else None
    except OSError:
        return None


def http_status(url):
    try:
        with urllib.request.urlopen(url, timeout=2) as response:
            return response.status
    except urllib.error.HTTPError as error:
        return error.code
    except OSError:
        return None


def register_server(url, values):
    body = json.dumps({
        "addresses": values["ROCKETNET_BACKEND_SERVER_ADDRESSES"].split(","),
        "public_key": values["ROCKETNET_BACKEND_SERVER_PUBLIC_KEY"],
    }).encode()
    request = urllib.request.Request(url, data=body, method="PUT", headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=2) as response:
            return response.status == 204
    except OSError:
        return False


def end_to_end(name, command, server_binary, client_binary, keys, production):
    label = f"backend_{name}_end_to_end"
    workdir = Path(tempfile.mkdtemp(prefix=f"rocketnet_backend_{name}_"))
    server = backend = None
    try:
        def launch_server(port):
            env = backend_environment({"ROCKETNET_SERVER_BACKEND_PUBLIC_KEY": keys["ROCKETNET_SERVER_BACKEND_PUBLIC_KEY"],
                                       "ROCKETNET_EXAMPLE_PORT": str(port)})
            return subprocess.Popen([str(server_binary)], cwd=workdir, env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

        started = start_on_free_port(label, "server (no backend.env)", socket.SOCK_DGRAM, launch_server,
                                     lambda port: read_env_file(workdir / "backend.env"), server_lost_port)
        if not started:
            return False
        server, _, values = started

        def launch_backend(port):
            env = backend_environment({"ROCKETNET_BACKEND_PORT": str(port),
                                       "ROCKETNET_BACKEND_PRIVATE_KEY": keys["ROCKETNET_BACKEND_PRIVATE_KEY"]})
            return subprocess.Popen(command, cwd=workdir, env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

        # ready once it reports an empty server registry
        started = start_on_free_port(
            label, "backend (no empty server registry)", socket.SOCK_STREAM, launch_backend,
            lambda port: http_status(f"http://127.0.0.1:{port}/match/{CLIENT_ID}") == 503, backend_lost_port)
        if not started:
            return False
        backend, port, _ = started
        match_url = f"http://127.0.0.1:{port}/match/{CLIENT_ID}"
        registered = wait_for(lambda: register_server(f"http://127.0.0.1:{port}/servers/example", values), backend, READY_SECONDS)
        if not registered:
            out, _ = stop(backend)
            return fail(label, f"backend did not register the live server\n{out}")
        token = wait_for(lambda: fetch_token(match_url), backend, READY_SECONDS)
        if not token:
            out, _ = stop(backend)
            return fail(label, f"backend served no token\n{out}")

        client = subprocess.run([str(client_binary), "--token", token], cwd=workdir, env=backend_environment({}), capture_output=True, text=True, timeout=execution_timeout(TIMEOUT_SECONDS))
        if client.returncode != 0 or last_line(client.stdout) != "PASS":
            return fail(label, f"client exited {client.returncode}\n{client.stdout}{client.stderr}")
        try:
            server_out, _ = server.communicate(timeout=execution_timeout(TIMEOUT_SECONDS))
        except subprocess.TimeoutExpired:
            return fail(label, "server did not exit after its client disconnected")
        if server.returncode != 0 or last_line(server_out) != "PASS":
            return fail(label, f"server exited {server.returncode}\n{server_out}")
        if production:
            for role, output in (("client", client.stdout), ("server", server_out)):
                if f"backend_{role}: mode=production transport=portable" not in output.splitlines():
                    observed = [line for line in output.splitlines() if line.startswith(f"backend_{role}:")]
                    return fail(label, f"{role} did not confirm production portable mode: {observed}")
        print(f"backend_{name}: fresh_keys=true server_registered_http=true token_from_http=true "
              f"production_required={str(production).lower()} portable_echo=true")
        return True
    finally:
        for process in (backend, server):
            if process:
                stop(process)
        shutil.rmtree(workdir, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default="build", help="build directory (default: build)")
    parser.add_argument("--config", default=None, help="Windows configuration, such as Release")
    parser.add_argument("--backend", action="append", choices=["go", "csharp"], help="run only this backend (repeatable)")
    parser.add_argument("--production", action="store_true", help="require client and server to confirm ROCKETNET_DEV=0 and portable transport")
    parser.add_argument("--root", default=None, help="tree holding examples/backend/ and bin/ (default: this repository)")
    args = parser.parse_args()

    global ROOT, BACKEND
    if args.root is not None:
        ROOT = Path(args.root).resolve()
        BACKEND = ROOT / "examples" / "backend"

    build = Path(args.build)
    if not build.is_absolute():
        build = (Path.cwd() / build).resolve()
    binaries = {}
    for name in ("example_backend_server", "example_backend_client", "backend_check_token"):
        binaries[name] = find_binary(name, build, args.config)
        if binaries[name] is None:
            print(f"FAIL: {name} binary not found under {args.build}", file=sys.stderr)
            return 1

    outdir = Path(tempfile.mkdtemp(prefix="rocketnet_backend_build_"))
    try:
        for name in args.backend or ["go", "csharp"]:
            command = build_backend(name, outdir)
            keys = generate_keys(name, command)
            if keys is None:
                return 1
            print(f"PASS backend_{name}_keygen")
            if not check_token(name, command, binaries["backend_check_token"], keys):
                return 1
            print(f"PASS backend_{name}_token")
            if not end_to_end(name, command, binaries["example_backend_server"], binaries["example_backend_client"], keys, args.production):
                return 1
            print(f"PASS backend_{name}_end_to_end")
    finally:
        shutil.rmtree(outdir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
