#!/usr/bin/env python3
"""The clang-tidy and cppcheck gate for the sdk.

Runs clang-tidy (checks from the repository's .clang-tidy) and cppcheck
(--enable=warning,performance,portability) over every sdk/*.cpp that has a
compile command in build/compile_commands.json, and compares the findings with
tools/tidy_baseline.txt and tools/cppcheck_baseline.txt.

The baseline shape is tools/run_static.py's: a finding is one line,
<tool> <file> <check> <hash>, where the hash is a digest of the file, the check,
the diagnostic text with every line and column number removed, and the
finding's ordinal among identical messages in that file. A finding keeps its
identity when lines move, and a second copy of an identical finding is a new
finding.

A finding that is present now but not in its baseline fails the job. A
baselined finding that is gone from the current run also fails the job: the
baseline only shrinks, and a stale line is deleted in the same pull request
that removes the finding. A translation unit the analyzer cannot parse fails
the job too: a file that is not analyzed must never read as a clean file.

clang-tidy is found as CLANG_TIDY, clang-tidy or clang-tidy-18; cppcheck as
CPPCHECK or cppcheck. A missing tool prints a TOOL-MISSING line and fails.

Run with --generate to rewrite both baselines from the current findings.
"""

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import socket
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
COMPILE_DB = os.path.join(BUILD, "compile_commands.json")
TIDY_DB_DIR = os.path.join(BUILD, "tidy")
TIDY_BASELINE = os.path.join(ROOT, "tools", "tidy_baseline.txt")
CPPCHECK_BASELINE = os.path.join(ROOT, "tools", "cppcheck_baseline.txt")

TIDY_NAMES = ["clang-tidy", "clang-tidy-18"]
CPPCHECK_ARGS = [
    "--enable=warning,performance,portability",
    "--inline-suppr",
    "--std=c++11",
    "--language=c++",
    "--quiet",
    "--template={file}:{line}:{column}: {severity}: {message} [{id}]",
]

DIAGNOSTIC = re.compile(r"^(.+?):(\d+):(\d+):\s+(\w+):\s+(.*?)\s+\[([^\]]+)\]\s*$")
NUMBERS = [
    re.compile(r"\b\d+:\d+\b"),
    re.compile(r"\blines? \d+\b"),
]

# cppcheck severities that are findings; information lines (missing system
# includes and the like) are not
CPPCHECK_SEVERITIES = ("error", "warning", "performance", "portability")


def runner_name():
    return os.environ.get("RUNNER_NAME") or socket.gethostname()


def relpath(path):
    return os.path.relpath(os.path.abspath(path), ROOT)


def in_sdk(rel):
    return rel.startswith("sdk" + os.sep) and os.sep not in rel[4:]


def digest(text):
    return hashlib.sha1(text.encode("utf-8", "replace")).hexdigest()[:16]


def strip_numbers(message):
    for pattern in NUMBERS:
        message = pattern.sub("#", message)
    return message


def finding_lines(tool, raw):
    """raw: (rel, check, message) tuples, duplicates kept. Identical messages
    in one file are numbered by their order, so each copy is its own finding."""
    seen = {}
    lines = []
    for rel, check, message in sorted(raw):
        key = "%s|%s|%s" % (rel, check, message)
        ordinal = seen.get(key, 0)
        seen[key] = ordinal + 1
        lines.append("%s\t%s\t%s\t%s" % (tool, rel, check,
                                         digest("%s|%d" % (key, ordinal))))
    return lines


def find_tool(env, names):
    override = os.environ.get(env)
    if override:
        return shutil.which(override) or (override if os.path.isfile(override) else None)
    for name in names:
        path = shutil.which(name)
        if path:
            return path
    return None


def ensure_compile_db():
    if os.path.exists(COMPILE_DB):
        return True
    print("configuring build/ with compile_commands.json", file=sys.stderr)
    subprocess.run(
        ["cmake", "-B", BUILD, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
         "-DCMAKE_BUILD_TYPE=Release"],
        cwd=ROOT, check=True,
    )
    return os.path.exists(COMPILE_DB)


def build_generated_headers():
    """The xdp server transport includes a header the build generates
    (build/generated/rocketnet_server_xdp.h); a configure alone does not write
    it, and an analyzer that cannot open it cannot parse that file."""
    result = subprocess.run(
        ["cmake", "--build", BUILD, "--target", "rocketnet_xdp_header"],
        cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    if result.returncode != 0:
        print(result.stdout[-2000:])
        print("building the generated headers failed")
        return False
    return True


def load_compile_db():
    """The first compile command per sdk/*.cpp, as tools/run_static.py takes
    it. A file built by several targets is analyzed once."""
    with open(COMPILE_DB, "r", encoding="utf-8") as handle:
        entries = json.load(handle)
    by_file = {}
    for entry in entries:
        by_file.setdefault(os.path.abspath(os.path.join(entry["directory"], entry["file"])), entry)
    return by_file


def sdk_files(by_file):
    files = []
    sdk = os.path.join(ROOT, "sdk")
    for name in sorted(os.listdir(sdk)):
        if not name.endswith(".cpp"):
            continue
        path = os.path.join(sdk, name)
        if path in by_file:
            files.append(path)
        else:
            print("no compile command for sdk/%s on this platform; not analyzed" % name)
    return files


def entry_arguments(entry):
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def write_tidy_db(by_file, files):
    os.makedirs(TIDY_DB_DIR, exist_ok=True)
    entries = [by_file[path] for path in files]
    with open(os.path.join(TIDY_DB_DIR, "compile_commands.json"), "w", encoding="utf-8") as handle:
        json.dump(entries, handle, indent=1)
    return TIDY_DB_DIR


def run_clang_tidy_file(tidy, db_dir, path):
    result = subprocess.run(
        [tidy, "-p", db_dir, "--quiet",
         "--extra-arg=-Wno-unknown-warning-option", path],
        cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    raw = []
    errors = []
    for line in result.stdout.splitlines():
        match = DIAGNOSTIC.match(line)
        if not match:
            continue
        file_path, _line, _col, level, message, check = match.groups()
        rel = relpath(os.path.join(ROOT, file_path))
        if level == "error":
            errors.append("%s: %s [%s]" % (relpath(path), message, check))
            continue
        if level != "warning" or not in_sdk(rel):
            continue
        raw.append((rel, check, strip_numbers(message)))
    if result.returncode != 0 and not raw and not errors:
        errors.append("%s: clang-tidy exited %d: %s" % (
            relpath(path), result.returncode, result.stderr.strip()[-400:]))
    return raw, errors


# cppcheck predefines no compiler macros. These are taken from the compiler the
# compile command names (`<compiler> -dM -E -x c++ /dev/null`), so cppcheck
# sees the same platform, architecture and byte-order branches the build
# compiles; the rest of the predefined set (__cplusplus, feature macros) is
# left to cppcheck's own --std handling.
PLATFORM_MACROS = (
    "__linux__", "__linux", "__unix__", "__unix", "__APPLE__", "__MACH__",
    "_WIN32", "_WIN64", "__FreeBSD__", "__OpenBSD__", "__NetBSD__",
    "__x86_64__", "__x86_64", "__amd64__", "__amd64", "__i386__",
    "__aarch64__", "__arm__", "__s390__", "__s390x__", "__powerpc__",
    "__LP64__", "_LP64", "__SIZEOF_POINTER__", "__CHAR_BIT__",
    "__BYTE_ORDER__", "__ORDER_LITTLE_ENDIAN__", "__ORDER_BIG_ENDIAN__",
    "__GNUC__", "__GNUC_MINOR__", "__clang__",
)

_platform_cache = {}


def platform_defines(compiler):
    if compiler in _platform_cache:
        return _platform_cache[compiler]
    defines = []
    try:
        output = subprocess.run(
            [compiler, "-dM", "-E", "-x", "c++", os.devnull],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
        ).stdout
    except OSError:
        output = ""
    for line in output.splitlines():
        parts = line.split(None, 2)
        if len(parts) >= 2 and parts[0] == "#define" and parts[1] in PLATFORM_MACROS:
            value = parts[2] if len(parts) == 3 else "1"
            defines.append("-D%s=%s" % (parts[1], value))
    _platform_cache[compiler] = sorted(defines)
    return _platform_cache[compiler]


def cppcheck_arguments(entry):
    args = entry_arguments(entry)
    out = list(platform_defines(args[0]))
    index = 0
    while index < len(args):
        arg = args[index]
        for flag in ("-D", "-U", "-I"):
            if arg == flag and index + 1 < len(args):
                index += 1
                out.append(flag + args[index])
                break
            if arg.startswith(flag) and len(arg) > 2:
                out.append(arg)
                break
        index += 1
    resolved = []
    for arg in out:
        if arg.startswith("-I") and not os.path.isabs(arg[2:]):
            arg = "-I" + os.path.join(entry["directory"], arg[2:])
        resolved.append(arg)
    return resolved


def run_cppcheck_file(cppcheck, entry, path):
    result = subprocess.run(
        [cppcheck] + CPPCHECK_ARGS + cppcheck_arguments(entry) + [path],
        cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    raw = []
    errors = []
    for line in result.stderr.splitlines():
        match = DIAGNOSTIC.match(line)
        if not match:
            continue
        file_path, _line, _col, level, message, check = match.groups()
        if level not in CPPCHECK_SEVERITIES:
            continue
        if check in ("syntaxError", "internalAstError", "cppcheckError", "unknownMacro"):
            errors.append("%s: %s [%s]" % (relpath(path), message, check))
            continue
        rel = relpath(os.path.join(ROOT, file_path))
        if not in_sdk(rel):
            continue
        raw.append((rel, check, strip_numbers(message)))
    if result.returncode != 0 and not errors:
        errors.append("%s: cppcheck exited %d: %s" % (
            relpath(path), result.returncode, result.stderr.strip()[-400:]))
    return raw, errors


def run_parallel(function, items, jobs):
    """Runs function over items, jobs at a time, and merges the findings.

    A header is seen by every translation unit that includes it, so the same
    header finding comes back once per includer. The merged count of a
    finding is its largest count in any one translation unit, not the sum:
    a header finding is one finding however many files include it, and a
    second copy of it in that header is still a second finding."""
    merged = {}
    errors = []
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        for file_raw, file_errors in executor.map(function, items):
            counts = {}
            for item in file_raw:
                counts[item] = counts.get(item, 0) + 1
            for item, count in counts.items():
                merged[item] = max(merged.get(item, 0), count)
            errors.extend(file_errors)
    raw = []
    for item, count in merged.items():
        raw.extend([item] * count)
    return raw, errors


def read_baseline(path):
    if not os.path.exists(path):
        return None
    lines = set()
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line and not line.startswith("#"):
                lines.add(line)
    return lines


def count_checks(findings):
    counts = {}
    for line in findings:
        check = line.split("\t")[2]
        counts[check] = counts.get(check, 0) + 1
    return counts


def print_counts(findings):
    counts = count_checks(findings)
    for check in sorted(counts):
        print("  %-64s %d" % (check, counts[check]))


def write_baseline(path, tool, findings):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("# %s findings recorded by tools/run_tidy.py --generate.\n" % tool)
        handle.write("# <tool> <file> <check> <hash>; the baseline only shrinks.\n")
        for line in sorted(findings):
            handle.write(line + "\n")


def compare(tool, path, findings):
    baseline = read_baseline(path)
    if baseline is None:
        print("no baseline at %s; run with --generate" % relpath(path))
        return False
    now = set(findings)
    new = sorted(now - baseline)
    gone = sorted(baseline - now)
    print("%s: %d findings, baseline %d" % (tool, len(now), len(baseline)))
    print_counts(now)
    if new:
        print("NEW %s findings not in %s:" % (tool, relpath(path)))
        for line in new:
            print("  + " + line.replace("\t", " "))
    if gone:
        print("BASELINED %s findings that disappeared; delete them from %s:" % (tool, relpath(path)))
        for line in gone:
            print("  - " + line.replace("\t", " "))
    return not new and not gone


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--generate", action="store_true",
                        help="rewrite both baselines from the current findings")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1),
                        help="files analyzed at once (default: min(8, cpus))")
    parser.add_argument("--tidy-baseline", default=TIDY_BASELINE)
    parser.add_argument("--cppcheck-baseline", default=CPPCHECK_BASELINE)
    args = parser.parse_args()

    tidy = find_tool("CLANG_TIDY", TIDY_NAMES)
    cppcheck = find_tool("CPPCHECK", ["cppcheck"])
    missing = False
    if tidy is None:
        print("TOOL-MISSING clang-tidy (clang-tidy, clang-tidy-18 or CLANG_TIDY) (missing) on %s" % runner_name())
        missing = True
    if cppcheck is None:
        print("TOOL-MISSING cppcheck (cppcheck or CPPCHECK) (missing) on %s" % runner_name())
        missing = True
    if missing:
        return 1

    if not ensure_compile_db():
        print("no %s after configure" % relpath(COMPILE_DB))
        return 1
    if not build_generated_headers():
        return 1
    by_file = load_compile_db()
    files = sdk_files(by_file)
    db_dir = write_tidy_db(by_file, files)

    start = time.time()
    tidy_raw, tidy_errors = run_parallel(
        lambda path: run_clang_tidy_file(tidy, db_dir, path), files, args.jobs)
    tidy_seconds = time.time() - start

    start = time.time()
    cppcheck_raw, cppcheck_errors = run_parallel(
        lambda path: run_cppcheck_file(cppcheck, by_file[path], path), files, args.jobs)
    cppcheck_seconds = time.time() - start

    tidy_findings = finding_lines("clang-tidy", tidy_raw)
    cppcheck_findings = finding_lines("cppcheck", cppcheck_raw)
    print("analyzed %d files with %d jobs: clang-tidy %.0f s, cppcheck %.0f s" % (
        len(files), args.jobs, tidy_seconds, cppcheck_seconds))

    errors = tidy_errors + cppcheck_errors
    if errors:
        print("ANALYZER ERRORS (a file that does not parse is not analyzed):")
        for line in errors:
            print("  ! " + line)

    if args.generate:
        if errors:
            print("baselines NOT written: fix the analyzer errors first")
            return 1
        write_baseline(args.tidy_baseline, "clang-tidy", tidy_findings)
        write_baseline(args.cppcheck_baseline, "cppcheck", cppcheck_findings)
        print("baseline %s: %d findings" % (relpath(args.tidy_baseline), len(tidy_findings)))
        print_counts(tidy_findings)
        print("baseline %s: %d findings" % (relpath(args.cppcheck_baseline), len(cppcheck_findings)))
        print_counts(cppcheck_findings)
        return 0

    tidy_ok = compare("clang-tidy", args.tidy_baseline, tidy_findings)
    cppcheck_ok = compare("cppcheck", args.cppcheck_baseline, cppcheck_findings)
    if errors or not tidy_ok or not cppcheck_ok:
        print("tidy gate FAILED")
        return 1
    print("tidy gate passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
