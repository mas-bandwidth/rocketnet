#!/usr/bin/env python3
"""Build an extra client/server backend and exchange packets through the public API."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--config", default="Release")
    args = parser.parse_args()
    build = args.build.resolve()
    if not (build / "CMakeCache.txt").is_file():
        sys.exit(f"Native extension: 0 cache entries: no CMakeCache.txt in {build}")
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if "=" in line and not line.startswith(("//", "#")):
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    source = Path(cache["rocketnet_SOURCE_DIR"])
    with tempfile.TemporaryDirectory(prefix="transport extension ", dir=build) as directory:
        command = ["cmake", "-S", source / "tests/driver_extension", "-B", directory,
                   f"-DROCKETNET_SOURCE_PATH={source}", f"-DCMAKE_BUILD_TYPE={args.config}",
                   f"-DCMAKE_CONFIGURATION_TYPES={args.config}", "-G", cache["CMAKE_GENERATOR"]]
        for key in ("CMAKE_TOOLCHAIN_FILE", "CMAKE_GENERATOR_PLATFORM", "CMAKE_GENERATOR_TOOLSET"):
            if cache.get(key):
                command.append(f"-D{key}={cache[key]}")
        run(*command)
        native_options = ["--", "-nodeReuse:false"] if cache["CMAKE_GENERATOR"].startswith("Visual Studio") else []
        run("cmake", "--build", directory, "--config", args.config, "--parallel", "4", *native_options)
        # ctest exits 0 when it finds 0 tests; an extension that registered
        # none has exchanged nothing, so that is a failure
        run("ctest", "--test-dir", directory, "-C", args.config, "--output-on-failure", "--no-tests=error")
    print("Native extension: new client/server transport registered without transport edits")


if __name__ == "__main__":
    main()
