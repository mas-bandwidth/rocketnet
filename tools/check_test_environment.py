#!/usr/bin/env python3
"""Run the SDK suite with hostile ambient config; require names-only success."""
import os
from pathlib import Path
import re
import subprocess
import sys

binary = Path(sys.argv[1] if len(sys.argv) > 1 else "bin/test").resolve()
env = os.environ.copy()
env.pop("ROCKETNET_TEST_VERBOSE", None)
# Preserve any caller's explicit ROCKETNET_DRIVER pin. Other values exercise
# exact names, legacy aliases, an invalid key and a long value without ever
# printing their contents. The suite derives its cleanup list from the tables.
env.update({
    "ROCKETNET_LOG_LEVEL": "4",
    "ROCKETNET_MAX_CLIENTS": "77",
    "ROCKETNET_NUM_REASSEMBLY_SLOTS": "3",
    "ROCKETNET_SERVER_NUM_QUEUES": "invalid",
    "ROCKETNET_SERVER_NUM_OS_CPUS": "invalid",
    "ROCKETNET_SERVER_SECRET_KEY": "x" * 4096,
    "ROCKETNET_SERVER_PUBLIC_ADDRESS": "invalid",
    "ROCKETNET_PUBLIC_ADDRESS": "invalid",
    "ROCKETNET_PUMP": "portable",
})
try:
    result = subprocess.run([str(binary)], env=env, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=180)
except subprocess.TimeoutExpired:
    sys.exit("SDK suite exceeded its 180-second deadline")
print(result.stdout, end="")
if result.returncode:
    sys.exit(f"SDK suite failed with ambient config overrides: {result.returncode}")
lines = result.stdout.splitlines()
if not lines or any(not re.fullmatch(r"(?:(?:rocketnet_)?test_[a-z0-9_]+|Test[A-Za-z0-9_]+)(?: [^\r\n]*\[(?:SKIPPED|EXPECTED_FAIL)[^\r\n]*\])?", line) for line in lines):
    sys.exit("Passing SDK suite emitted output other than test names or named skips")
if "test_suite_environment" not in lines:
    sys.exit("SDK suite did not execute its environment restoration test")
