#!/usr/bin/env python3
"""Install, relocate, and build/run independent C and C++ consumers of both libraries."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from check_xdp_archive import embedded_bytes


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


# A consumer of the installed package: it requests the major.minor line its
# game names and reads what the imported targets hand its own compile.
SETTINGS_PROBE = """cmake_minimum_required(VERSION 3.20)
project(rocketnet_settings_probe LANGUAGES C)
find_package(RocketNet {request} CONFIG REQUIRED)
foreach(linkage rocketnet rocketnet_shared)
    get_target_property(options RocketNet::${{linkage}} INTERFACE_COMPILE_OPTIONS)
    get_target_property(definitions RocketNet::${{linkage}} INTERFACE_COMPILE_DEFINITIONS)
    message(STATUS "probe ${{linkage}} options=[${{options}}] definitions=[${{definitions}}]")
    add_executable(probe_${{linkage}} probe.c)
    target_link_libraries(probe_${{linkage}} PRIVATE RocketNet::${{linkage}})
endforeach()
"""
PROBE_SOURCE = "#include \"rocketnet.h\"\nint main( void ) { return &rocketnet_time == 0; }\n"
# Only what compiling rocketnet.h needs: ROCKETNET_SHARED selects the shared
# library's import declaration, and ROCKETNET_DEV is the value the library was
# built with, so the header the consumer compiles matches it (sdk/rocketnet.h).
CONSUMER_DEFINITIONS = {"ROCKETNET_SHARED", "ROCKETNET_DEV"}
FLOATING_POINT_OPTIONS = ("-ffp-contract", "/fp:")


def check_exported_settings(installed, root, command, config, native_build_options):
    """Fail when the package refuses its own major.minor line or hands the
    library's private build settings to a consumer's compile."""
    version_file = next(installed.rglob("RocketNetConfigVersion.cmake")).read_text()
    version = re.search(r'set\(PACKAGE_VERSION "([0-9.]+)"\)', version_file).group(1)
    line = ".".join(version.split(".")[:2])
    failures = []
    for request in (version, line):
        probe = root / f"settings probe {request}"
        probe.mkdir()
        (probe / "CMakeLists.txt").write_text(SETTINGS_PROBE.format(request=request))
        (probe / "probe.c").write_text(PROBE_SOURCE)
        build = probe / "build"
        probe_command = list(command)
        probe_command[probe_command.index("-S") + 1] = probe
        probe_command[probe_command.index("-B") + 1] = build
        probe_command.append("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")
        result = subprocess.run([str(arg) for arg in probe_command], capture_output=True, text=True)
        if result.returncode != 0:
            failures.append(f"find_package(RocketNet {request}) refuses the installed {version}")
            continue
        if request != version:
            continue
        for linkage, options, definitions in re.findall(
                r"probe (\w+) options=\[(.*?)\] definitions=\[(.*?)\]", result.stdout):
            for option in options.split(";"):
                if option.startswith(FLOATING_POINT_OPTIONS):
                    failures.append(f"RocketNet::{linkage} hands the consumer {option}")
            for definition in definitions.split(";"):
                if definition and not definition.endswith("-NOTFOUND") and \
                        definition.split("=", 1)[0] not in CONSUMER_DEFINITIONS:
                    failures.append(f"RocketNet::{linkage} hands the consumer -D{definition}")
        run("cmake", "--build", build, "--config", config, *native_build_options)
        commands = build / "compile_commands.json"
        if commands.exists():
            for entry in json.loads(commands.read_text()):
                text = entry.get("command") or " ".join(entry.get("arguments", []))
                for option in FLOATING_POINT_OPTIONS:
                    if option in text:
                        failures.append(f"consumer compile line carries {option}: {text}")
    for failure in failures:
        print("FAIL  " + failure)
    if failures:
        raise RuntimeError(f"{len(failures)} package setting failures")
    print(f"Package {version}: find_package(RocketNet {line}) accepts it; consumers inherit no private settings")


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
    header = build / "generated/rocketnet_server_xdp.h"
    with tempfile.TemporaryDirectory(prefix="package check ", dir=build) as directory:
        root = Path(directory)
        # Linux builds embed server_xdp.o (cmake/EmbedXdp.cmake): the array
        # holds exactly the object the build compiled.
        if header.exists():
            obj = build / "generated/server_xdp.o"
            data = embedded_bytes(header)
            if data and (not obj.exists() or obj.read_bytes() != data):
                raise RuntimeError(f"{header} does not embed {obj}")
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
        check_exported_settings(installed, root, command, args.config, native_build_options)
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
