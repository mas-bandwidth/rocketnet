#!/usr/bin/env python3
"""Install, relocate, and build/run independent C and C++ consumers of both libraries."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from check_xdp_archive import embedded_bytes


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def cmake_bool(value, default=True):
    if value is None:
        return default
    return str(value).upper() not in ("OFF", "0", "FALSE", "NO", "N", "IGNORE", "NOTFOUND", "")


# A consumer of the installed package: it requests the major.minor line its
# game names and reads what the imported targets hand its own compile.
SETTINGS_PROBE = """cmake_minimum_required(VERSION 3.20)
project(rocketnet_settings_probe LANGUAGES C)
find_package(RocketNet {request} CONFIG REQUIRED COMPONENTS {components})
{absent_checks}
foreach(linkage {linkages})
    get_target_property(options RocketNet::${{linkage}} INTERFACE_COMPILE_OPTIONS)
    get_target_property(definitions RocketNet::${{linkage}} INTERFACE_COMPILE_DEFINITIONS)
    message(STATUS "probe ${{linkage}} options=[${{options}}] definitions=[${{definitions}}]")
    add_executable(probe_${{linkage}} probe.c)
    target_link_libraries(probe_${{linkage}} PRIVATE RocketNet::${{linkage}})
endforeach()
"""
PROBE_SOURCE = "#include \"rocketnet.h\"\nint main( void ) { return &rocketnet_time == 0; }\n"
# Only what compiling rocketnet.h needs: ROCKETNET_SHARED selects the shared
# library's import declaration, and ROCKETNET_DEV and ROCKETNET_MAX_CLIENTS are
# the values the library was built with, so the header the consumer compiles
# matches it (sdk/rocketnet.h).
CONSUMER_DEFINITIONS = {"ROCKETNET_SHARED", "ROCKETNET_DEV", "ROCKETNET_MAX_CLIENTS"}
FLOATING_POINT_OPTIONS = ("-ffp-contract", "/fp:")


def prepare_consumer_source(root, shared_enabled, dev_enabled):
    """Return the consumer CMake project directory, adapting linkage and
    embedded-variant assertions when shared or development variants are omitted."""
    template_dir = Path(__file__).parent / "package_consumer"
    if shared_enabled and dev_enabled:
        return template_dir
    staged = root / "consumer_source"
    staged.mkdir(parents=True, exist_ok=True)
    for name in ("main.c", "main.cpp"):
        shutil.copy2(template_dir / name, staged / name)
    cmake_text = (template_dir / "CMakeLists.txt").read_text()
    extra_checks = []
    if not shared_enabled:
        extra_checks.append(
            '    if(TARGET rocketnet_shared OR TARGET RocketNet::rocketnet_shared OR TARGET rocketnet_shared_dev)\n'
            '        message(FATAL_ERROR "Embedded SDK created shared target with ROCKETNET_BUILD_SHARED=OFF")\n'
            '    endif()'
        )
    if not dev_enabled:
        extra_checks.append(
            '    if(TARGET rocketnet_dev OR TARGET rocketnet_shared_dev)\n'
            '        message(FATAL_ERROR "Embedded SDK created dev target with ROCKETNET_BUILD_DEV_LIBRARIES=OFF")\n'
            '    endif()'
        )
    else:
        extra_checks.append(
            '    if(NOT TARGET rocketnet_dev)\n'
            '        message(FATAL_ERROR "Embedded SDK omitted rocketnet_dev with ROCKETNET_BUILD_DEV_LIBRARIES=ON")\n'
            '    endif()'
        )
    if extra_checks:
        cmake_text = cmake_text.replace(
            '    get_target_property(static_type rocketnet TYPE)',
            "\n".join(extra_checks) + '\n    get_target_property(static_type rocketnet TYPE)'
        )
    if not shared_enabled:
        cmake_text = cmake_text.replace(
            "foreach(linkage rocketnet rocketnet_shared)",
            'if(TARGET RocketNet::rocketnet_shared)\n'
            '    message(FATAL_ERROR "Installed package exported RocketNet::rocketnet_shared with ROCKETNET_BUILD_SHARED=OFF")\n'
            'endif()\n'
            'foreach(linkage rocketnet)'
        )
    (staged / "CMakeLists.txt").write_text(cmake_text)
    return staged


def check_exported_settings(installed, root, command, config, native_build_options, shared_enabled,
                            expected_dev, expected_max_clients):
    """Fail when the package refuses its own major.minor line or hands the
    library's private build settings to a consumer's compile."""
    version_file = next(installed.rglob("RocketNetConfigVersion.cmake")).read_text()
    version = re.search(r'set\(PACKAGE_VERSION "([0-9.]+)"\)', version_file).group(1)
    line = ".".join(version.split(".")[:2])
    targets_file = next(installed.rglob("RocketNetTargets.cmake")).read_text()
    failures = []
    if "RocketNet::rocketnet" not in targets_file:
        failures.append("RocketNetTargets.cmake does not export RocketNet::rocketnet")
    if shared_enabled and "RocketNet::rocketnet_shared" not in targets_file:
        failures.append("RocketNetTargets.cmake does not export RocketNet::rocketnet_shared")
    if not shared_enabled and "RocketNet::rocketnet_shared" in targets_file:
        failures.append("RocketNetTargets.cmake exports RocketNet::rocketnet_shared when ROCKETNET_BUILD_SHARED=OFF")
    for dev_target in ("rocketnet_dev", "rocketnet_shared_dev"):
        if re.search(rf"\b{dev_target}\b", targets_file):
            failures.append(f"RocketNetTargets.cmake exports build-tree target {dev_target}")
    linkages = "rocketnet rocketnet_shared" if shared_enabled else "rocketnet"
    components = "rocketnet rocketnet_shared" if shared_enabled else "rocketnet"
    absent_checks = (
        ""
        if shared_enabled
        else 'if(TARGET RocketNet::rocketnet_shared)\n'
             '    message(FATAL_ERROR "RocketNet::rocketnet_shared exists when ROCKETNET_BUILD_SHARED=OFF")\n'
             'endif()'
    )
    expected_definitions = {
        "rocketnet": {
            f"ROCKETNET_DEV={expected_dev}",
            f"ROCKETNET_MAX_CLIENTS={expected_max_clients}",
        },
        "rocketnet_shared": {
            "ROCKETNET_SHARED",
            f"ROCKETNET_DEV={expected_dev}",
            f"ROCKETNET_MAX_CLIENTS={expected_max_clients}",
        },
    }
    for index, request in enumerate((version, line)):
        # Keep the nested CMake try-compile path below Windows/MSBuild's path
        # limit. The ordinary relocated consumer retains spaces in its path and
        # covers relocation; these probes exercise only package settings.
        probe = root / f"p{index}"
        probe.mkdir()
        (probe / "CMakeLists.txt").write_text(
            SETTINGS_PROBE.format(
                request=request,
                components=components,
                absent_checks=absent_checks,
                linkages=linkages,
            )
        )
        (probe / "probe.c").write_text(PROBE_SOURCE)
        build = root / f"b{index}"
        probe_command = list(command)
        probe_command[probe_command.index("-S") + 1] = probe
        probe_command[probe_command.index("-B") + 1] = build
        probe_command.append("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")
        result = subprocess.run([str(arg) for arg in probe_command], capture_output=True, text=True)
        if result.returncode != 0:
            print(result.stdout, end="")
            print(result.stderr, end="")
            failures.append(f"settings probe for RocketNet {request} failed to configure "
                            f"(exit {result.returncode}, installed {version})")
            continue
        if request != version:
            continue
        for linkage, options, definitions in re.findall(
                r"probe (\w+) options=\[(.*?)\] definitions=\[(.*?)\]", result.stdout):
            for option in options.split(";"):
                if option.startswith(FLOATING_POINT_OPTIONS):
                    failures.append(f"RocketNet::{linkage} hands the consumer {option}")
            actual_definitions = {
                definition for definition in definitions.split(";")
                if definition and not definition.endswith("-NOTFOUND")
            }
            for definition in sorted(actual_definitions):
                if definition.split("=", 1)[0] not in CONSUMER_DEFINITIONS:
                    failures.append(f"RocketNet::{linkage} hands the consumer -D{definition}")
            wanted_definitions = expected_definitions[linkage]
            if actual_definitions != wanted_definitions:
                failures.append(
                    f"RocketNet::{linkage} compile definitions are {sorted(actual_definitions)}, "
                    f"expected {sorted(wanted_definitions)}"
                )
        run("cmake", "--build", build, "--config", config, *native_build_options)
        commands = build / "compile_commands.json"
        if commands.exists():
            for entry in json.loads(commands.read_text()):
                text = entry.get("command") or " ".join(entry.get("arguments", []))
                for option in FLOATING_POINT_OPTIONS:
                    if option in text:
                        failures.append(f"consumer compile line carries {option}: {text}")
    if not shared_enabled:
        neg_probe = root / "p_neg"
        neg_probe.mkdir()
        (neg_probe / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(rocketnet_neg_probe LANGUAGES C)\n"
            f"find_package(RocketNet {version} CONFIG REQUIRED COMPONENTS rocketnet_shared)\n"
        )
        neg_build = root / "b_neg"
        neg_command = list(command)
        neg_command[neg_command.index("-S") + 1] = neg_probe
        neg_command[neg_command.index("-B") + 1] = neg_build
        neg_result = subprocess.run([str(arg) for arg in neg_command], capture_output=True, text=True)
        if neg_result.returncode == 0:
            failures.append("find_package(RocketNet COMPONENTS rocketnet_shared) unexpectedly succeeded with ROCKETNET_BUILD_SHARED=OFF")
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
    shared_enabled = cmake_bool(cache.get("ROCKETNET_BUILD_SHARED"), True)
    dev_enabled = cmake_bool(cache.get("ROCKETNET_BUILD_DEV_LIBRARIES"), True)
    expected_dev = "1" if cmake_bool(cache.get("ROCKETNET_DEV"), True) else "0"
    expected_max_clients = cache.get("ROCKETNET_MAX_CLIENTS", "1000")
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
        consumer_source = prepare_consumer_source(root, shared_enabled, dev_enabled)
        consumer = root / "consumer build"
        command = ["cmake", "-S", consumer_source, "-B", consumer,
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
        check_exported_settings(installed, root, command, args.config, native_build_options, shared_enabled,
                                expected_dev, expected_max_clients)
        if args.embedded:
            embedded = root / "embedded build"
            # Reuse generator/toolchain selection, replacing only producer-specific arguments.
            command[command.index("-B") + 1] = embedded
            command += [
                f"-DROCKETNET_SOURCE_PATH={source}",
                "-DBUILD_SHARED_LIBS=ON",
                f"-DROCKETNET_BUILD_SHARED={'ON' if shared_enabled else 'OFF'}",
                f"-DROCKETNET_BUILD_DEV_LIBRARIES={'ON' if dev_enabled else 'OFF'}",
            ]
            run(*command)
            run("cmake", "--build", embedded, "--config", args.config, "--parallel", "4", *native_build_options)
            run("ctest", "--test-dir", embedded, "-C", args.config, "--output-on-failure", env=env)
    linkage_summary = "static/shared" if shared_enabled else "static"
    print(f"Relocated SDK: C/C++ {linkage_summary} consumers passed")


if __name__ == "__main__":
    main()
