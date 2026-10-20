#!/usr/bin/env python3
"""Install, relocate, and build/run independent C and C++ consumers of both libraries."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from check_xdp_archive import check_archive, embedded_bytes, inputs_for


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--embedded", action="store_true")
    args = parser.parse_args()
    build = args.build.resolve()
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if "=" in line and not line.startswith(("//", "#")):
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    source = Path(cache["rocketnet_SOURCE_DIR"])
    # Reusable MSBuild workers can retain the temporary build directory after
    # a successful build. These consumers have no subsequent build to reuse
    # them for; let the workers exit so TemporaryDirectory can remove the tree.
    native_build_options = (["--", "-nodeReuse:false"]
                            if cache["CMAKE_GENERATOR"].startswith("Visual Studio") else [])
    archive = build / "generated/rocketnet_server_xdp.tar.gz"
    with tempfile.TemporaryDirectory(prefix="package check ", dir=build) as directory:
        root = Path(directory)
        inputs = inputs_for(source, root)
        data = archive.read_bytes() if archive.exists() else embedded_bytes(source / "sdk/rocketnet_server_xdp.h")
        check_archive(data, inputs)
        stage, installed = root / "stage", root / "relocated SDK"
        run("cmake", "--install", build, "--config", args.config, "--prefix", stage)
        stage.rename(installed)
        # No absolute source/build/original install path may leak into package metadata.
        for config in installed.rglob("*.cmake"):
            text = config.read_text().replace("\\", "/")
            for forbidden in (build, stage, Path(cache["rocketnet_SOURCE_DIR"])):
                if forbidden.as_posix() in text:
                    raise RuntimeError(f"{config.name} embeds producer path {forbidden}")
        consumer = root / "consumer build"
        command = ["cmake", "-S", Path(__file__).parent / "package_consumer", "-B", consumer,
                   f"-DCMAKE_PREFIX_PATH={installed}", f"-DCMAKE_BUILD_TYPE={args.config}",
                   # Multi-config generators otherwise generate uninstalled configs too.
                   # In particular, vcpkg maps RelWithDebInfo/MinSizeRel to Release,
                   # which does not exist in this independently installed Debug prefix.
                   f"-DCMAKE_CONFIGURATION_TYPES={args.config}",
                   "-G", cache["CMAKE_GENERATOR"]]
        for key in ("CMAKE_TOOLCHAIN_FILE", "CMAKE_OSX_ARCHITECTURES", "CMAKE_OSX_DEPLOYMENT_TARGET",
                    "CMAKE_GENERATOR_PLATFORM", "CMAKE_GENERATOR_TOOLSET"):
            if cache.get(key):
                command.append(f"-D{key}={cache[key]}")
        run(*command)
        run("cmake", "--build", consumer, "--config", args.config, "--parallel", "4", *native_build_options)
        # A universal producer build has universal consumers: every consumer executable
        # carries each slice CMAKE_OSX_ARCHITECTURES names.
        wanted = set(cache.get("CMAKE_OSX_ARCHITECTURES", "").split(";")) - {""}
        if len(wanted) > 1:
            executables = [path for path in consumer.rglob("consumer_rocketnet_*")
                           if path.is_file() and os.access(path, os.X_OK)]
            if not executables:
                raise RuntimeError("no consumer executable found to check its slices")
            for executable in executables:
                built = set(subprocess.check_output(["lipo", "-archs", str(executable)], text=True).split())
                if built != wanted:
                    raise RuntimeError(f"{executable.name} carries {sorted(built)}, not {sorted(wanted)}")
            print(f"Universal consumers: {len(executables)} executables carry {sorted(wanted)}")
        env = os.environ.copy()
        # Installed DLL plus vcpkg's copied runtime dependency on Windows.
        env["PATH"] = str(installed / "bin") + os.pathsep + env.get("PATH", "")
        run("ctest", "--test-dir", consumer, "-C", args.config, "--output-on-failure", env=env)
        if args.embedded:
            embedded = root / "embedded build"
            # Reuse generator/toolchain selection, replacing only producer-specific arguments.
            command[command.index("-B") + 1] = embedded
            command += [f"-DROCKETNET_SOURCE_PATH={source}", "-DBUILD_SHARED_LIBS=ON"]
            run(*command)
            run("cmake", "--build", embedded, "--config", args.config, "--parallel", "4", *native_build_options)
            run("ctest", "--test-dir", embedded, "-C", args.config, "--output-on-failure", env=env)
    print("Relocated SDK: C/C++ static/shared consumers passed")


if __name__ == "__main__":
    main()
