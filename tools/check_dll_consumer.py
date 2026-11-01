#!/usr/bin/env python3
"""Check that C and C++ programs can call RocketNet through rocketnet.h alone.

For each of four cases -- C and C++ against the development shared library,
then C and C++ against the development static library -- the script copies
only sdk/rocketnet.h and that one library into a fresh empty directory,
compiles tools/dll_consumer/main.c as C99 or main.cpp as C++11 with the
directory as the only RocketNet include path, links the library alone (plus
system libraries and the C++ runtime), and runs the program. The program
creates a message server on 127.0.0.1 through the portable transport, connects a
client with a development connect token, exchanges one message each way inside
five seconds of wall clock, and exits 0.

The script also lists the production shared library's exported symbols and
refuses a name outside tools/exports.txt or an expected name that is missing;
the development connect-token minting symbol is therefore absent from the
production library.

Prints one PASS line per case and one for the export list, and exits 0 only
when every check passes.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from nightly_timeout import execution_timeout

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = Path(__file__).resolve()

failures = []


def report(ok, line):
    print(("PASS  " if ok else "FAIL  ") + line)
    if not ok:
        failures.append(line)


def library_names():
    if sys.platform == "win32":
        return "rocketnet_dev.dll", "rocketnet_dev_import.lib", "rocketnet_dev.lib", "rocketnet.dll"
    if sys.platform == "darwin":
        return "librocketnet_dev.dylib", "librocketnet_dev.dylib", "librocketnet_dev.a", "librocketnet.dylib"
    return "librocketnet_dev.so", "librocketnet_dev.so", "librocketnet_dev.a", "librocketnet.so"


def search_directories(build, config):
    candidates = [
        build,
        build / config,
        build / "bin",
        build / "bin" / config,
        build / config / "bin",
        ROOT / "bin",
        ROOT / "bin" / config,
    ]
    seen = []
    for candidate in candidates:
        if candidate not in seen:
            seen.append(candidate)
    return seen


def find_library(build, config, name):
    for directory in search_directories(build, config):
        candidate = directory / name
        if candidate.exists():
            return candidate
    for directory in (build, ROOT / "bin"):
        if directory.is_dir():
            matches = sorted(directory.rglob(name))
            if matches:
                return matches[0]
    return None


def shared_family(library):
    """The shared library and its SONAME aliases, as the one library to stage.

    The link name carries the platform suffix (libfoo.so on Linux,
    libfoo.dylib on macOS) while the SONAME files insert their version before
    that suffix (libfoo.so.1, libfoo.1.dylib). Strip the suffix first, or the
    darwin aliases never match and the staged loader name cannot be opened.
    """
    directory = library.parent
    name = library.name
    if name.endswith(".dylib"):
        stem = name[: -len(".dylib")]
        def is_alias(candidate):
            return candidate.name == name or (candidate.name.startswith(stem + ".")
                                              and candidate.name.endswith(".dylib"))
    elif name.endswith(".so"):
        def is_alias(candidate):
            return candidate.name == name or candidate.name.startswith(name + ".")
    else:
        return [library]
    names = {name}
    for candidate in directory.iterdir():
        if is_alias(candidate):
            names.add(candidate.name)
    return [directory / alias for alias in sorted(names)]


def stage_shared(library, directory):
    staged = []
    for source in shared_family(library):
        target = directory / source.name
        if source.is_symlink():
            link = os.readlink(source)
            os.symlink(link, target)
        else:
            shutil.copy2(source, target)
        staged.append(target)
    return staged


_msvc_env = None


def msvc_root():
    installer = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
    return Path(subprocess.check_output([str(installer), "-latest", "-products", "*",
                                         "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                         "-property", "installationPath"], text=True).strip())


def msvc_tool(name):
    found = shutil.which(name)
    if found:
        return found
    tools = sorted(msvc_root().glob(f"VC/Tools/MSVC/*/bin/Hostx64/x64/{name}.exe"))
    if not tools:
        raise RuntimeError(f"Cannot find {name}")
    return str(tools[-1])


def msvc_environment():
    global _msvc_env
    if _msvc_env is not None:
        return _msvc_env.copy()
    if os.environ.get("INCLUDE") and os.environ.get("LIB"):
        _msvc_env = os.environ.copy()
        return _msvc_env.copy()
    vcvars = msvc_root() / "VC/Auxiliary/Build/vcvars64.bat"
    output = subprocess.check_output(f"cmd.exe /d /s /c \"\"{vcvars}\" >nul && set\"",
                                     text=True, errors="replace")
    env = os.environ.copy()
    for line in output.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            env[key.upper() if key.upper() in ("PATH", "INCLUDE", "LIB", "LIBPATH") else key] = value
    _msvc_env = env
    return _msvc_env.copy()


def cmake_cache(build):
    values = {}
    cache = build / "CMakeCache.txt"
    if cache.exists():
        for line in cache.read_text(errors="replace").splitlines():
            match = re.match(r"([A-Za-z0-9_]+):[A-Z]+=(.*)$", line)
            if match:
                values[match.group(1)] = match.group(2)
    return values


def system_libraries(build):
    if sys.platform == "darwin":
        return ["-lc++", "-framework", "CoreFoundation", "-framework", "SystemConfiguration"]
    cache = cmake_cache(build)
    libraries = []
    for dependency in ("Bpf", "Xdp", "Uring"):
        value = cache.get("ROCKETNET_%s_LIBRARY" % dependency, "")
        if value and not value.endswith("-NOTFOUND"):
            libraries.append(value)
    return libraries + ["-lstdc++", "-lm", "-lpthread"]


def universal_arch_flags(library):
    """The -arch flags that make a consumer match a universal library."""
    if sys.platform != "darwin":
        return []
    try:
        output = subprocess.check_output(["lipo", "-archs", str(library)],
                                         text=True, errors="replace")
    except (OSError, subprocess.CalledProcessError):
        return []
    flags = []
    for arch in output.split():
        flags += ["-arch", arch]
    return flags


def compile_and_run(language, linkage, source, link_library, staging_library, build, config, cc, cxx):
    with tempfile.TemporaryDirectory(prefix="rocketnet dll consumer ") as directory_name:
        directory = Path(directory_name)
        shutil.copy2(ROOT / "sdk" / "rocketnet.h", directory / "rocketnet.h")
        if linkage == "shared":
            stage_shared(staging_library, directory)
            if not (directory / link_library.name).exists():
                shutil.copy2(link_library, directory / link_library.name)
        else:
            shutil.copy2(staging_library, directory / staging_library.name)
        staged_link = directory / link_library.name
        if "rocketnet.h" not in [path.name for path in directory.iterdir()]:
            report(False, f"{language} {linkage}: rocketnet.h was not staged")
            return
        if not staged_link.exists():
            report(False, f"{language} {linkage}: {link_library.name} was not staged")
            return

        if sys.platform == "win32":
            compiler = msvc_tool("cl")
            crt = "/MDd" if config.lower() == "debug" else "/MD"
            flags = ["/nologo", "/W4", "/WX", crt, "/D_CRT_SECURE_NO_WARNINGS", "/I", str(directory)]
            if linkage == "shared":
                flags += ["/DROCKETNET_SHARED=1"]
            source_arg = ("/Tc" if language == "C" else "/Tp") + str(source)
            executable = directory / ("dll_consumer_%s_%s.exe" % (language.lower(), linkage))
            command = [compiler] + flags + [source_arg, str(staged_link),
                                            "ws2_32.lib", "iphlpapi.lib", "advapi32.lib",
                                            "/Fe:" + str(executable), "/Fo:" + str(directory) + "\\"]
            environment = msvc_environment()
            environment["PATH"] = str(directory) + os.pathsep + environment.get("PATH", "")
        else:
            compiler = cc if language == "C" else cxx
            standard = ["-std=c99"] if language == "C" else ["-std=c++11"]
            flags = standard + ["-pedantic", "-Wall", "-Wextra", "-Werror", "-I", str(directory)] + \
                    universal_arch_flags(staging_library)
            if linkage == "shared":
                flags += ["-DROCKETNET_SHARED=1"]
            executable = directory / ("dll_consumer_%s_%s" % (language.lower(), linkage))
            command = [compiler] + flags + [str(source), str(staged_link)] + \
                      system_libraries(build) + ["-Wl,-rpath," + str(directory), "-o", str(executable)]
            environment = {key: value for key, value in os.environ.items()
                           if key not in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH",
                                          "OBJC_INCLUDE_PATH", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH")}
            environment["LD_LIBRARY_PATH"] = str(directory)
            if sys.platform == "darwin":
                environment["DYLD_LIBRARY_PATH"] = str(directory)

        result = subprocess.run(command, capture_output=True, text=True, cwd=directory, env=environment)
        if result.returncode != 0:
            detail = (result.stderr.strip().splitlines() or result.stdout.strip().splitlines() or
                      ["exit %d" % result.returncode])[-1]
            report(False, f"{language} client against the {linkage} library builds: {detail}")
            return
        wanted = set(universal_arch_flags(staging_library)[1::2])
        if len(wanted) > 1:
            built = set(subprocess.check_output(["lipo", "-archs", str(executable)], text=True).split())
            if built != wanted:
                report(False, f"{language} client against the {linkage} library carries the slices "
                              f"{sorted(wanted)}: lipo -archs prints {sorted(built)}")
                return
        try:
            run = subprocess.run([str(executable)], capture_output=True, text=True,
                                 cwd=directory, env=environment, timeout=execution_timeout(30))
        except subprocess.TimeoutExpired:
            report(False, f"{language} client against the {linkage} library runs in five seconds (wall clock exceeded)")
            return
        if run.returncode != 0:
            detail = (run.stderr.strip().splitlines() or run.stdout.strip().splitlines() or
                      ["exit %d" % run.returncode])[-1]
            report(False, f"{language} client against the {linkage} library exchanges one message each way: {detail}")
            return
        report(True, f"{language} client against the {linkage} library: one message each way, exited 0")


def exported_symbols(library):
    if sys.platform == "win32":
        output = subprocess.check_output([msvc_tool("dumpbin"), "/nologo", "/exports", str(library)],
                                         text=True, errors="replace")
        return set(re.findall(r"^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)", output, re.M))
    if sys.platform == "darwin":
        output = subprocess.check_output(["nm", "-gU", str(library)], text=True, errors="replace")
        return {line.split()[-1].removeprefix("_") for line in output.splitlines() if line.split()}
    output = subprocess.check_output(["nm", "-D", "--defined-only", str(library)],
                                     text=True, errors="replace")
    return {line.split()[-1].split("@")[0] for line in output.splitlines() if line.split()}


def check_exports(build, config, production_name):
    library = find_library(build, config, production_name)
    if library is None:
        report(False, f"production shared library {production_name} was not found under {build}")
        return
    expected = {line.strip() for line in (ROOT / "tools" / "exports.txt").read_text().splitlines()
                if line.strip() and not line.startswith("#")}
    try:
        actual = exported_symbols(library)
    except (OSError, subprocess.CalledProcessError) as error:
        report(False, f"production shared library exports were readable: {error}")
        return
    missing = expected - actual
    extra = actual - expected
    if missing:
        report(False, "production shared library is missing exports: " + ", ".join(sorted(missing)))
    if extra:
        report(False, "production shared library exports names outside tools/exports.txt: " + ", ".join(sorted(extra)))
    if not missing and not extra:
        report(True, f"production shared library exports exactly the {len(expected)} names in tools/exports.txt")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", type=Path, default=ROOT / "build", help="the build directory holding the libraries")
    parser.add_argument("--config", default="Release", help="the Windows build configuration")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    build = args.build if args.build.is_absolute() else (Path.cwd() / args.build).resolve()
    dev_shared_name, dev_shared_link_name, dev_static_name, production_name = library_names()

    shared = find_library(build, args.config, dev_shared_name)
    link_shared = find_library(build, args.config, dev_shared_link_name)
    static = find_library(build, args.config, dev_static_name)
    if shared is None:
        report(False, f"development shared library {dev_shared_name} was not found under {build}")
    if link_shared is None:
        report(False, f"development shared link library {dev_shared_link_name} was not found under {build}")
    if static is None:
        report(False, f"development static library {dev_static_name} was not found under {build}")

    sources = ROOT / "tools" / "dll_consumer"
    if shared is not None and link_shared is not None:
        # The link name is the unversioned alias; the staged family carries the
        # SONAME files the loader asks for at run time.
        compile_and_run("C", "shared", sources / "main.c", link_shared, shared,
                        build, args.config, args.cc, args.cxx)
        compile_and_run("C++", "shared", sources / "main.cpp", link_shared, shared,
                        build, args.config, args.cc, args.cxx)
    if static is not None:
        compile_and_run("C", "static", sources / "main.c", static, static,
                        build, args.config, args.cc, args.cxx)
        compile_and_run("C++", "static", sources / "main.cpp", static, static,
                        build, args.config, args.cc, args.cxx)

    check_exports(build, args.config, production_name)

    if failures:
        print(f"dll consumer: {len(failures)} check(s) failed")
        return 1
    print("dll consumer: C and C++ callable against the shared and static libraries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
