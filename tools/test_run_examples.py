#!/usr/bin/env python3
"""Tests for tools/run_examples.py's verdict, with no build and no sockets.

A fixture tree holds examples/<name>.c for every program the further runs
name (each one runs alone, so no pair and no UDP port is used) and a build
directory of fake example_<name> scripts that answer as the real examples
do. That tree passes; the same tree with one example that exits 0 without
PASS as its last line is refused by name; a tree with no examples is
refused. Each case runs the tool as a process with --root and --build on a
temporary directory."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parent / "run_examples.py"

# answers every run tools/run_examples.py makes, as the real examples do
FAKE_EXAMPLE = """#!/bin/sh
case "$1 $2" in
    "--wrong-protocol ") echo "expected refusal: wrong protocol id"; exit 0 ;;
    "--seconds "*) echo "FAIL: --seconds takes 1 .. 35791394"; exit 1 ;;
    "--seed "*) echo "FAIL: --seed takes a whole number"; exit 1 ;;
    "--clients 1x") echo "usage: example_game_loop [--clients N]"; exit 1 ;;
    "--port "*) echo "FAIL: the port takes 1 .. 65535"; exit 1 ;;
esac
if [ "$ROCKETNET_SERVER_MAX_CLIENTS" = "5" ]; then
    echo 'FAIL: ROCKETNET_SERVER_MAX_CLIENTS="5": server create refused'
    exit 1
fi
echo PASS
"""

NAMES = ("client_server", "client_server_server", "config_env", "soak", "game_loop")


class RunExamplesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "root"
        self.build = Path(self.tmp.name) / "build"
        (self.root / "examples").mkdir(parents=True)
        self.build.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def example(self, name, script):
        (self.root / "examples" / f"{name}.c").write_text("int main(void) { return 0; }\n")
        binary = self.build / f"example_{name}"
        binary.write_text(script)
        binary.chmod(0o755)

    def plant(self):
        for name in NAMES:
            self.example(name, FAKE_EXAMPLE)

    def run_tool(self):
        env = {k: v for k, v in os.environ.items() if not k.startswith("ROCKETNET_")}
        return subprocess.run([sys.executable, str(TOOL), "--root", str(self.root), "--build", str(self.build)],
                              capture_output=True, text=True, timeout=30, env=env)

    def test_examples_that_pass_pass(self):
        self.plant()
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS example_soak\n", result.stdout)
        self.assertIn("PASS example_client_server_server --port 70000\n", result.stdout)

    def test_an_example_without_pass_last_is_refused(self):
        self.plant()
        self.example("hello", "#!/bin/sh\necho PASS\necho done\n")
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("FAIL example_hello: did not print PASS last", result.stderr)

    def test_a_game_thread_failure_is_refused(self):
        # the default run passes; only the game-thread frame fails
        self.plant()
        self.example("game_loop", "#!/bin/sh\nif [ \"$1 $2\" = \"--game-threads 4\" ]; then echo 'FAIL: a message of the wrong size arrived'; exit 1; fi\n"
                     + FAKE_EXAMPLE.split("\n", 1)[1])
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS example_game_loop\n", result.stdout)
        self.assertIn("FAIL example_game_loop --game-threads 4 --clients 64: exited 1, expected 0", result.stderr)

    def test_a_server_game_thread_failure_is_refused(self):
        # client_server runs alone in this tree, so the server arguments reach it
        self.plant()
        self.example("client_server", "#!/bin/sh\nif [ \"$1 $2\" = \"--game-threads 4\" ]; then echo 'FAIL: a game thread'; exit 1; fi\n"
                     + FAKE_EXAMPLE.split("\n", 1)[1])
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("FAIL example_client_server server --game-threads 4: exited 1, expected 0", result.stderr)

    def test_no_examples_is_refused(self):
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("FAIL: 0 examples", result.stderr)


if __name__ == "__main__":
    unittest.main()
