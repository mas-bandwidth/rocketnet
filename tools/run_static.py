#!/usr/bin/env python3
"""The static-analysis gate for the sdk.

Compares the current gcc -fanalyzer findings against tools/static_baseline.txt.

gcc -fanalyzer runs over the parser translation units: every sdk/*.cpp that
includes rocketnet_read_write.h, plus the parser files named in the card
(base64, message, ack, replay, connect_token). The analyzer walks
these paths interprocedurally, which is where wire parsing defects live.

A finding is one line in the baseline: <file> <check> <hash>, where the hash is
a stable digest of the diagnostic text with the source location removed, so a
finding keeps its identity when lines move. A finding that is present now but
not in the baseline fails the job. A baselined finding that is gone from the
current run also fails the job: the baseline only shrinks, and a stale line is
deleted in the same pull request that removes the finding.

The analyzer runs with -Wno-error, and a finding the build's own -Werror=
options still raise to an error is read the same as a warning: [-Werror=X] is
normalized to [-WX], so one finding has one line whatever its level. The run
is incomplete, and the gate fails without writing a report or a baseline, when
a selected translation unit has no compile command or when the compiler exits
nonzero for any reason other than error-level analyzer findings it printed.

Run with --generate to rewrite the baseline from the current findings.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
COMPILE_DB = os.path.join(BUILD, "compile_commands.json")
BASELINE = os.path.join(ROOT, "tools", "static_baseline.txt")

NAMED_PARSER_FILES = [
    "sdk/rocketnet_base64.cpp",
    "sdk/rocketnet_message.cpp",
    "sdk/rocketnet_ack.cpp",
    "sdk/rocketnet_replay.cpp",
    "sdk/rocketnet_connect_token.cpp",
]

READ_WRITE_HEADER = "rocketnet_read_write.h"

LOCATION = re.compile(r"^(.+?):(\d+):(\d+):\s+(warning|error|fatal error):\s+(.*)$")
OPTION = re.compile(r"\s*\[(-W[A-Za-z0-9_.=+-]+)\]\s*$")
ERROR_LINE = re.compile(r"\berror:")


# finding line -> the "<file>:<line>:<col>: <message>" sites gcc printed for it,
# so a failing run names where each finding is; the baseline keeps only the line
SITES = {}


class IncompleteRun(Exception):
    """The analyzer did not cover every selected translation unit."""


def relpath(path):
    return os.path.relpath(os.path.abspath(path), ROOT)


def digest(text):
    return hashlib.sha1(text.encode("utf-8", "replace")).hexdigest()[:16]


def finding_line(analyzer, rel, check, message):
    key = "%s|%s|%s" % (rel, check, message)
    return "%s\t%s\t%s\t%s" % (analyzer, rel, check, digest(key))


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


def load_compile_db():
    with open(COMPILE_DB, "r", encoding="utf-8") as handle:
        entries = json.load(handle)
    by_file = {}
    for entry in entries:
        by_file.setdefault(os.path.abspath(entry["file"]), entry)
    return by_file


def parser_files():
    files = set()
    sdk = os.path.join(ROOT, "sdk")
    for name in sorted(os.listdir(sdk)):
        if not name.endswith(".cpp"):
            continue
        path = os.path.join(sdk, name)
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                text = handle.read()
        except OSError:
            continue
        if READ_WRITE_HEADER in text:
            files.add(os.path.join("sdk", name))
    for name in NAMED_PARSER_FILES:
        if os.path.exists(os.path.join(ROOT, name)):
            files.add(name)
    return sorted(files)


def analyzer_check(rest):
    """The finding's message and -Wanalyzer-* check, or None if it is not one."""
    option = OPTION.search(rest)
    if not option:
        return None
    check = option.group(1)
    if check.startswith("-Werror="):
        check = "-W" + check[len("-Werror="):]
    if not check.startswith("-Wanalyzer-"):
        return None
    return rest[: option.start()].strip(), check


def parse_diagnostics(text, analyzer):
    """Turn gcc -fanalyzer output into baseline finding lines."""
    return parse_output(text, analyzer)[0]


def parse_output(text, analyzer):
    """The finding lines, whether any finding was error level, and every
    error line that is not an analyzer finding."""
    findings = []
    error_findings = False
    other_errors = []
    for raw in text.splitlines():
        match = LOCATION.match(raw)
        finding = None
        if match and match.group(4) != "fatal error":
            finding = analyzer_check(match.group(5))
        if finding is None:
            if ERROR_LINE.search(raw):
                other_errors.append(raw)
            continue
        path, line, col, level, _rest = match.groups()
        message, check = finding
        if level == "error":
            error_findings = True
        found = finding_line(analyzer, relpath(path), check, message)
        SITES.setdefault(found, []).append(
            "%s:%s:%s: %s [%s]" % (relpath(path), line, col, message, check))
        findings.append(found)
    return findings, error_findings, other_errors


def run_gcc_analyzer():
    by_file = load_compile_db()
    findings = []
    for rel in parser_files():
        entry = by_file.get(os.path.join(ROOT, rel))
        if entry is None:
            raise IncompleteRun("no compile command for %s in %s" % (rel, COMPILE_DB))
        command = entry["command"]
        compiler = "g++" if rel.endswith((".cc", ".cpp", ".cxx")) else "gcc"
        command = re.sub(r"^\S+", compiler, command)
        tmp_obj = os.path.join(BUILD, "static-gcc-analyzer.o")
        command += " -fanalyzer -Wno-error -fdiagnostics-color=never -o " + tmp_obj
        result = subprocess.run(
            command, shell=True, cwd=entry["directory"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        found, error_findings, other_errors = parse_output(result.stderr, "gcc")
        if result.returncode != 0 and (other_errors or not error_findings):
            detail = "\n".join(other_errors) or result.stderr.strip() or "(no output)"
            raise IncompleteRun("%s exited %d for %s:\n%s"
                                % (compiler, result.returncode, rel, detail))
        findings.extend(found)
    return findings


def current_findings():
    return sorted(set(run_gcc_analyzer()))


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


def write_baseline(path, findings):
    counts = {}
    for line in findings:
        check = line.split("\t")[2]
        counts[check] = counts.get(check, 0) + 1
    with open(path, "w", encoding="utf-8") as handle:
        for line in sorted(findings):
            handle.write(line + "\n")
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate", action="store_true",
                        help="rewrite the baseline from the current findings")
    parser.add_argument("--baseline", default=BASELINE,
                        help="baseline file to compare against")
    parser.add_argument("--report", metavar="PATH",
                        help="write the finding set to this path")
    args = parser.parse_args()

    ensure_compile_db()
    try:
        findings = current_findings()
    except IncompleteRun as incomplete:
        print("static gate FAILED: the analyzer run is incomplete, so no "
              "report or baseline is written")
        print(str(incomplete))
        return 1

    if args.report:
        with open(args.report, "w", encoding="utf-8") as handle:
            for line in findings:
                handle.write(line + "\n")

    if args.generate:
        counts = write_baseline(args.baseline, findings)
        print("baseline %s: %d findings" % (args.baseline, len(findings)))
        for check in sorted(counts):
            print("  %-52s %d" % (check, counts[check]))
        return 0

    baseline = read_baseline(args.baseline)
    if baseline is None:
        print("no baseline at %s; run with --generate" % args.baseline,
              file=sys.stderr)
        return 1

    # GCC 15+ must find zero whatever the baseline holds; GCC 13 must match it
    # exactly (the baseline is empty now, so both generations must find zero).
    if os.path.abspath(args.baseline) == os.path.abspath(BASELINE):
        version = subprocess.check_output(["g++", "-dumpfullversion"], text=True).strip()
        if int(version.split(".", 1)[0]) >= 15:
            baseline = set()

    now = set(findings)
    new = sorted(now - baseline)
    gone = sorted(baseline - now)

    counts = {}
    for line in now:
        check = line.split("\t")[2]
        counts[check] = counts.get(check, 0) + 1

    print("static gate: %d findings, baseline %d" % (len(now), len(baseline)))
    for check in sorted(counts):
        print("  %-52s %d" % (check, counts[check]))

    if new:
        print("NEW findings not in the baseline:")
        for line in new:
            print("  + " + line.replace("\t", " "))
            for site in sorted(set(SITES.get(line, []))):
                print("      " + site)
    if gone:
        print("BASELINED findings that disappeared; delete them from "
              "tools/static_baseline.txt:")
        for line in gone:
            print("  - " + line.replace("\t", " "))

    if new or gone:
        print("static gate FAILED")
        return 1
    print("static gate passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
