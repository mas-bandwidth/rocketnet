#!/usr/bin/env python3
"""Tests for tools/run_backend_examples.py's key and token verdicts, with no
build, no sockets and no network.

`go` is a fake on PATH that "builds" a fake backend script; the build
directory holds fake example_backend_server, example_backend_client and
backend_check_token programs. A backend whose generated private key does not
carry its public key, and a token backend_check_token refuses, are refused
by the tool as a process (it stops before the loopback end-to-end run, which
needs sockets and so is not run here); a tree with none of the binaries is
refused. The passing side is checked in process: generate_keys and
check_token accept a good fake backend."""

import base64
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
TOOL = TOOLS / "run_backend_examples.py"
sys.path.insert(0, str(TOOLS))
import run_backend_examples  # noqa: E402

PUBLIC = bytes(range(32))
GOOD_PRIVATE = bytes(range(100, 132)) + PUBLIC
BAD_PRIVATE = bytes(range(100, 132)) + bytes(range(1, 33))

FAKE_GO = """import shutil, sys, os
args = sys.argv[1:]
assert args[0] == "build" and args[1] == "-o", args
shutil.copyfile(os.environ["FAKE_BACKEND"], args[2])
os.chmod(args[2], 0o755)
"""

FAKE_BACKEND = """import sys
if sys.argv[1] == "--keygen":
    print("ROCKETNET_SERVER_BACKEND_PUBLIC_KEY={public}")
    print("ROCKETNET_BACKEND_PRIVATE_KEY={private}")
elif sys.argv[1] == "--mint":
    print("token-for-" + sys.argv[2])
else:
    sys.exit("fake backend: no server in this test")
"""

FAKE_CHECK = """import sys
token = sys.stdin.read().strip()
print("PASS" if token == "token-for-" + sys.argv[1] and len(sys.argv) == 4 else "FAIL: token refused")
"""


def b64(data):
    return base64.b64encode(data).decode()


class RunBackendExamplesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.root = base / "root"
        (self.root / "examples/backend/go").mkdir(parents=True)
        self.build = base / "build"
        self.build.mkdir()
        self.fakes = base / "fakebin"
        self.fakes.mkdir()
        self.backend = base / "backend.py"
        self.script(self.fakes / "go", FAKE_GO)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("ROCKETNET_")}
        self.env.update(PATH=f"{self.fakes}{os.pathsep}{os.environ.get('PATH', '')}", FAKE_BACKEND=str(self.backend))

    def tearDown(self):
        self.tmp.cleanup()

    def script(self, path, body):
        path.write_text(f"#!{sys.executable}\n{body}")
        path.chmod(0o755)

    def plant(self, private, check=FAKE_CHECK):
        self.script(self.backend, FAKE_BACKEND.format(public=b64(PUBLIC), private=b64(private)))
        for name in ("example_backend_server", "example_backend_client"):
            self.script(self.build / name, "import sys\nsys.exit('not run in this test')\n")
        self.script(self.build / "backend_check_token", check)

    def run_tool(self):
        return subprocess.run([sys.executable, str(TOOL), "--root", str(self.root), "--build", str(self.build),
                               "--backend", "go"], capture_output=True, text=True, timeout=30, env=self.env)

    def test_good_keys_and_token_are_accepted(self):
        self.plant(GOOD_PRIVATE)
        command = [str(self.backend)]
        keys = run_backend_examples.generate_keys("go", command)
        self.assertEqual(keys, {"ROCKETNET_SERVER_BACKEND_PUBLIC_KEY": b64(PUBLIC),
                                "ROCKETNET_BACKEND_PRIVATE_KEY": b64(GOOD_PRIVATE)})
        self.assertTrue(run_backend_examples.check_token("go", command, self.build / "backend_check_token", keys))

    def test_a_private_key_not_carrying_its_public_key_is_refused(self):
        self.plant(BAD_PRIVATE)
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("FAIL backend_go_keygen: invalid key output", result.stderr)
        self.assertNotIn("PASS backend_go_keygen", result.stdout)

    def test_a_refused_token_is_refused(self):
        self.plant(GOOD_PRIVATE, check="print('FAIL: signature')\n")
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS backend_go_keygen", result.stdout)
        self.assertIn("FAIL backend_go_token: backend_check_token exited 0", result.stderr)

    def test_no_binaries_is_refused(self):
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("binary not found", result.stderr)


if __name__ == "__main__":
    unittest.main()
