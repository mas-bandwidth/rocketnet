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

3. End to end, over loopback: bans, the whitelist and token sharing. The
   backend starts on a free TCP port with an empty registry (a match is
   503); example_backend_server starts on a free UDP port, registers itself
   and fetches its whitelist; a client signs in with a mock ticket, is
   matched and connects; a second client can neither use its token nor,
   signed in as the same user, take a second seat; a ban kicks the first
   within one whitelist interval; the second then connects as the banned
   user with a token minted before the ban and is kicked the same way; the
   banned user's next match is 403; with the backend gone, the server keeps
   its last good list and still kicks a client on no list. See end_to_end
   below.

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
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

from nightly_timeout import execution_timeout, no_execution_timeout

ROOT = Path(__file__).resolve().parents[1]
BACKEND = ROOT / "examples" / "backend"
TIMEOUT_SECONDS = 60
READY_SECONDS = 30
SHARE_TRY_SECONDS = 3  # long enough for a loopback handshake many times over
KICK_SLACK_SECONDS = 0.5  # beyond one whitelist interval: the fetch, a frame, the disconnect packet
PROTOCOL_ID = "0x52434b544e455431"  # EXAMPLE_PROTOCOL_ID, examples/dev_keys.h
CLIENT_ID = 1234567890123
CAROL_CLIENT_ID = 0xCA401  # minted offline: seated on no server, so on no whitelist

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


def http(method, url, body=b""):
    """One request; returns (status, body bytes), or (None, b"") when the
    backend cannot be reached."""
    request = urllib.request.Request(url, data=body if method in ("POST", "PUT") else None, method=method)
    try:
        with urllib.request.urlopen(request, timeout=2) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as error:
        return error.code, error.read()
    except OSError:
        return None, b""


class Lines:
    """Reads a process's output on a thread and keeps each line with the
    monotonic time it arrived, so the harness can time what the process
    saw."""

    def __init__(self, process):
        self.process = process
        self.lines = []
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        for line in self.process.stdout:
            with self.lock:
                self.lines.append((time.monotonic(), line.rstrip("\r\n")))

    def find(self, text):
        """The time of the first line holding text, or None."""
        with self.lock:
            for when, line in self.lines:
                if text in line:
                    return when
        return None

    def count(self, text):
        with self.lock:
            return sum(1 for _, line in self.lines if text in line)

    def text(self):
        with self.lock:
            return "\n".join(line for _, line in self.lines)

    def wait(self, timeout):
        """Waits for the process to exit and its output to be read; returns
        its exit code, or None when it did not exit in time."""
        try:
            code = self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None
        self.thread.join(timeout=10)
        return code


def start(command, workdir, env):
    return Lines(subprocess.Popen(command, cwd=workdir, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))


def whitelist_timing(text):
    """The interval and grace the server prints: "... every 2.0 seconds as
    <id>; a client not on it is kicked after at most 6.0 seconds"."""
    match = re.search(r"fetches its whitelist from \S+ every ([0-9.]+) seconds as \S+; a client not on it is kicked after at most ([0-9.]+) seconds", text)
    return (float(match.group(1)), float(match.group(2))) if match else None


def mint_offline(command, server_text, keys, client_id):
    """A token from the backend's --mint, which seats nobody: for the
    server's address and public key, as the server printed them."""
    values = dict(line.split("=", 1) for line in server_text.splitlines()
                  if line.startswith(("ROCKETNET_BACKEND_SERVER_ADDRESSES=", "ROCKETNET_BACKEND_SERVER_PUBLIC_KEY=")))
    if len(values) != 2:
        return None
    values["ROCKETNET_BACKEND_PRIVATE_KEY"] = keys["ROCKETNET_BACKEND_PRIVATE_KEY"]
    minted = subprocess.run(command + ["--mint", str(client_id)], env=backend_environment(values), capture_output=True, text=True,
                            timeout=execution_timeout(TIMEOUT_SECONDS))
    token = minted.stdout.strip()
    return token if minted.returncode == 0 and len(token) == 1024 else None


def shared_token_refused(client_binary, token, workdir):
    """True when a client with token never connects in SHARE_TRY_SECONDS."""
    tried = subprocess.run([str(client_binary), "--token", token, "--stay", "--seconds", str(SHARE_TRY_SECONDS)], cwd=workdir,
                           env=backend_environment({}), capture_output=True, text=True, timeout=execution_timeout(TIMEOUT_SECONDS))
    return tried.returncode != 0 and "-> CONNECTED" not in tried.stdout


def end_to_end(name, command, server_binary, client_binary, keys, production):
    """Bans, the whitelist and sharing, end to end over loopback:

    1. With no server registered, a match is 503.
    2. The server registers itself and fetches its whitelist.
    3. Client A signs in as alice (a mock ticket), is matched, connects,
       gets its echo, and stays connected across whitelist fetches: it is
       on the list.
    4. Client B, given A's connect token, cannot connect: the server bound
       that token to A's address.
    5. B, given alice's sign-in instead, is matched (the same client id, the
       same one entry on the whitelist) but cannot connect while A holds
       alice's one seat.
    6. alice is banned: off the whitelist at once, and the server kicks A
       within one whitelist interval plus KICK_SLACK_SECONDS (the fetch, a
       frame and the disconnect packet).
    7. A's token still cannot connect from another address.
    8. B's token, minted before the ban, connects now the seat is free, and
       B, playing as alice, is kicked under alice's ban within the same bound.
    9. alice's next match is 403.
    10. Fail closed: bob is matched, connects and is on the list. Then the
        backend goes away. bob stays (the last good list holds him), while
        carol, with a token minted offline and so on no list, is kicked
        within the grace plus KICK_SLACK_SECONDS. bob leaves on his own;
        the server exits PASS having kicked alice twice and carol once.
    """
    label = f"backend_{name}_end_to_end"
    workdir = Path(tempfile.mkdtemp(prefix=f"rocketnet_backend_{name}_"))
    processes = []
    try:
        port = free_port(socket.SOCK_STREAM)
        base = f"http://127.0.0.1:{port}"
        backend = start(command, workdir, backend_environment({"ROCKETNET_BACKEND_PORT": str(port),
                        "ROCKETNET_BACKEND_PRIVATE_KEY": keys["ROCKETNET_BACKEND_PRIVATE_KEY"]}))
        processes.append(backend)

        def ticket(user):
            status, body = http("POST", f"{base}/mock/ticket/{user}")
            return body if status == 200 else None

        alice = wait_for(lambda: ticket("alice"), backend.process, READY_SECONDS)
        if not alice:
            return fail(label, f"the backend gave no ticket\n{backend.text()}")
        status, _ = http("POST", f"{base}/match", alice)
        if status != 503:
            return fail(label, f"a match with no servers registered answered {status}, not 503\n{backend.text()}")

        env = backend_environment({"ROCKETNET_SERVER_BACKEND_PUBLIC_KEY": keys["ROCKETNET_SERVER_BACKEND_PUBLIC_KEY"]})
        env["ROCKETNET_EXAMPLE_PORT"] = str(free_port(socket.SOCK_DGRAM))
        server = start([str(server_binary), "--backend", f"127.0.0.1:{port}", "--server-id", "example", "--sessions", "4"], workdir, env)
        processes.append(server)
        whitelist_url = f"{base}/servers/example/whitelist"
        if not wait_for(lambda: http("GET", whitelist_url)[0] == 200, server.process, READY_SECONDS):
            return fail(label, f"the server did not register itself with the backend\n{server.text()}")
        timing = wait_for(lambda: whitelist_timing(server.text()), server.process, READY_SECONDS)
        if not timing:
            return fail(label, f"the server did not say its whitelist interval and grace\n{server.text()}")
        interval, grace = timing
        bound = interval + KICK_SLACK_SECONDS

        status, token = http("POST", f"{base}/match", alice)
        if status != 200 or len(token) != 1024:
            return fail(label, f"the match answered {status} with {len(token)} bytes, not a token")
        token = token.decode()
        status, listed = http("GET", whitelist_url)
        if status != 200 or len(listed) != 8:
            return fail(label, f"after one match the whitelist is {status} with {len(listed)} bytes, not one client id")
        alice_id = int.from_bytes(listed, "little")

        # A connects and stays: whitelist fetches go by and A is still on
        client_a = start([str(client_binary), "--token", token, "--stay", "--seconds", "40"], workdir, backend_environment({}))
        processes.append(client_a)
        if not wait_for(lambda: client_a.find("client received:"), client_a.process, READY_SECONDS):
            return fail(label, f"client A got no echo\n{client_a.text()}")
        time.sleep(interval * 1.5)
        if client_a.process.poll() is not None or client_a.find("SERVER_DISCONNECTED"):
            return fail(label, f"client A, on the whitelist, did not stay connected\n{client_a.text()}\n{server.text()}")

        # B with A's connect token: bound to A's address, it never connects
        if not shared_token_refused(client_binary, token, workdir):
            return fail(label, "client B connected with A's connect token while A held the seat")

        # B with alice's sign-in: the same user, so the same client id and one seat
        status, token_b = http("POST", f"{base}/match", ticket("alice") or b"")
        status_list, listed = http("GET", whitelist_url)
        if status != 200 or status_list != 200 or listed != alice_id.to_bytes(8, "little"):
            return fail(label, f"a second sign-in as alice was not matched to the same one whitelist entry ({status}, {status_list}, {len(listed)} bytes)")
        token_b = token_b.decode()
        if not shared_token_refused(client_binary, token_b, workdir):
            return fail(label, "client B, signed in as alice, connected while A held alice's seat")

        # ban alice: off the whitelist at once, then A is kicked
        status, _ = http("PUT", f"{base}/bans/mock/alice")
        banned_at = time.monotonic()
        if status != 204:
            return fail(label, f"the ban answered {status}, not 204")
        status, listed = http("GET", whitelist_url)
        if status != 200 or listed:
            return fail(label, f"after the ban the whitelist is {status} with {len(listed)} bytes, not empty")
        if client_a.wait(execution_timeout(TIMEOUT_SECONDS)) != 0 or not client_a.find("-> SERVER_DISCONNECTED"):
            return fail(label, f"client A was not disconnected by the server\n{client_a.text()}\n{server.text()}")
        ban_kick = client_a.find("-> SERVER_DISCONNECTED") - banned_at
        if ban_kick > bound:
            return fail(label, f"client A was kicked {ban_kick:.3f} seconds after its ban, over {bound:.1f}")

        # A's token stays dead at another address after A has gone
        if not shared_token_refused(client_binary, token, workdir):
            return fail(label, "client B connected with A's connect token after A was kicked")

        # B's token, minted before the ban: B gets alice's seat, and alice's ban
        client_b = start([str(client_binary), "--token", token_b, "--stay", "--seconds", "20"], workdir, backend_environment({}))
        processes.append(client_b)
        if client_b.wait(execution_timeout(TIMEOUT_SECONDS)) != 0 or not client_b.find("client received:") or not client_b.find("-> SERVER_DISCONNECTED"):
            return fail(label, f"client B, signed in as alice, was not connected then kicked\n{client_b.text()}\n{server.text()}")
        share_kick = client_b.find("-> SERVER_DISCONNECTED") - client_b.find("-> CONNECTED")
        if share_kick > bound:
            return fail(label, f"client B was kicked {share_kick:.3f} seconds after it connected, over {bound:.1f}")

        status, _ = http("POST", f"{base}/match", ticket("alice") or b"")
        if status != 403:
            return fail(label, f"a banned user's match answered {status}, not 403")

        # fail closed: bob is on the last good list, carol on none, and the backend is gone
        status, token_bob = http("POST", f"{base}/match", ticket("bob") or b"")
        if status != 200:
            return fail(label, f"bob's match answered {status}")
        client_bob = start([str(client_binary), "--token", token_bob.decode(), "--stay", "--seconds", str(grace * 2 + 4)], workdir, backend_environment({}))
        processes.append(client_bob)
        if not wait_for(lambda: client_bob.find("client received:"), client_bob.process, READY_SECONDS):
            return fail(label, f"bob got no echo\n{client_bob.text()}")
        time.sleep(interval * 1.5)
        stop(backend.process)
        token_carol = mint_offline(command, server.text(), keys, CAROL_CLIENT_ID)
        if not token_carol:
            return fail(label, "could not mint carol's token offline")
        client_carol = start([str(client_binary), "--token", token_carol, "--stay", "--seconds", str(grace * 2)], workdir, backend_environment({}))
        processes.append(client_carol)
        if client_carol.wait(execution_timeout(TIMEOUT_SECONDS)) != 0 or not client_carol.find("-> SERVER_DISCONNECTED"):
            return fail(label, f"carol, on no whitelist, was not kicked with the backend gone\n{client_carol.text()}\n{server.text()}")
        closed_kick = client_carol.find("-> SERVER_DISCONNECTED") - client_carol.find("-> CONNECTED")
        if closed_kick > grace + KICK_SLACK_SECONDS:
            return fail(label, f"carol was kicked {closed_kick:.3f} seconds after she connected, over {grace + KICK_SLACK_SECONDS:.1f}")
        if client_bob.process.poll() is not None or client_bob.find("SERVER_DISCONNECTED"):
            return fail(label, f"bob, on the last good whitelist, was kicked when the backend went away\n{client_bob.text()}\n{server.text()}")
        if client_bob.wait(execution_timeout(TIMEOUT_SECONDS)) != 1 or "FAIL: the server did not disconnect the client" not in client_bob.text():
            return fail(label, f"bob did not stay connected to the end\n{client_bob.text()}")

        code = server.wait(execution_timeout(TIMEOUT_SECONDS))
        server_out = server.text()
        if code != 0 or last_line(server_out) != "PASS":
            return fail(label, f"server exited {code}\n{server_out}")
        kicked = f"server kicked client {alice_id:016x}"
        if server.count(kicked) != 2 or server.count(f"server kicked client {CAROL_CLIENT_ID:016x}") != 1 or server.count("server kicked client") != 3:
            return fail(label, f"the server did not kick {alice_id:016x} exactly twice and {CAROL_CLIENT_ID:016x} once\n{server_out}")
        if production:
            for role, output in (("client", client_a.text()), ("server", server_out)):
                if f"backend_{role}: mode=production transport=portable" not in output.splitlines():
                    observed = [line for line in output.splitlines() if line.startswith(f"backend_{role}:")]
                    return fail(label, f"{role} did not confirm production portable mode: {observed}")
        print(f"backend_{name}: fresh_keys=true server_registered_itself=true ticket_match=true whitelist_interval={interval:.1f}s "
              f"shared_token_refused=true second_sign_in_refused_while_seated=true ban_kick={ban_kick:.3f}s shared_sign_in_kick={share_kick:.3f}s "
              f"banned_match=403 grace={grace:.1f}s backend_gone_kept_last_list=true fail_closed_kick={closed_kick:.3f}s "
              f"production_required={str(production).lower()}")
        return True
    finally:
        for lines in reversed(processes):
            stop(lines.process)
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
