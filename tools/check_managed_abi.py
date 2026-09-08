#!/usr/bin/env python3
"""Prove managed/native mismatches refuse before any struct-writing native entry."""
import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bin", type=Path)
    parser.add_argument("--dotnet", default="dotnet")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    prefix, suffix = ("", ".dll") if sys.platform == "win32" else ("lib", ".dylib" if sys.platform == "darwin" else ".so")
    command = [args.dotnet, "run", "--project", str(root / "csharp/smoke"), "-c", "Release", "--no-build", "--"]

    def check(library, *options, mode=""):
        env = os.environ.copy()
        env["ROCKETNET_LIBRARY_PATH"] = str(library.resolve())
        env["ROCKETNET_ABI_TEST_MODE"] = mode
        subprocess.run(command + list(options), env=env, check=True)

    missing = args.bin / "does-not-exist" / "rocketnet"
    check(missing, "--token-boundaries")
    check(missing, "--load-refusal")
    check(args.bin / f"{prefix}rocketnet_abi_legacy{suffix}", "--abi-refusal", "missing")
    for mode in ("revision", "size", "field count", "offset", "width"):
        check(args.bin / f"{prefix}rocketnet_abi_mismatch{suffix}", "--abi-refusal", mode, mode=mode)
    check(args.bin / f"{prefix}rocketnet{suffix}", "--abi-only")
    print("Managed ABI: matching library accepted; missing/legacy and five mismatch cases refused")


if __name__ == "__main__":
    main()
