#!/usr/bin/env bash
# run_msan.sh: the one command for the MemorySanitizer leg. Builds the
# portable unit suite with clang -fsanitize=memory and origin tracking, runs
# ./bin/test under it, and reports "MSan: clean" when no uninitialised read
# appears.
#
# MSan needs every linked object instrumented. The default build uses the
# pinned two-file sodium vendor (sodium/sodium.c + sodium.h), which is part of
# this tree, so it is compiled by the same CMake target with the same flags;
# no system sodium is linked. The unit suite opens no socket and starts no
# transport, so the run is the portable configuration: no XDP dataplane, no
# io_uring. The instrumented binary lives under build-msan/bin so a later
# `make && ./bin/test` is not a stale sanitizer build.
#
# Exit status: 0 when the run is clean, or when every finding is listed in
# tools/msan_known.txt and every failed test carries the MSan exit code; 1
# when a finding is not listed there, a test crashes or exits for any other
# reason, or the run stops without a test fail line. A finding is the top
# frame of a MemorySanitizer warning. Its origin trace is printed with it.
#
# RUN_MSAN_TEST_BIN names a binary to run as the suite instead of configuring
# and building build-msan/bin/test; it exists for the gate's own tests.
set -euo pipefail

cd "$(dirname "$0")/.."

build_dir=build-msan
msan_exitcode=86
flags="-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer"
link_flags="-fno-sanitize-link-c++-runtime"

# The unit suite defines its own global operator new/delete (test_channels.cpp)
# to arm the allocation-failure trap; the MSan C++ runtime ships replacements
# too. Leave the MSan C++ runtime off the link so the instrumented in-tree
# operator is the only one; it calls malloc, which the MSan runtime still
# intercepts and poisons, so shadow tracking holds. Allowing the duplicate
# instead is not enough: the driver puts the sanitizer runtime ahead of the
# suite's objects, so its operator new wins and the trap never fires
# (test_channels_allocator_trap_witness fails). The flag belongs to the link
# step, so it goes on the executable link line only: on a compile line clang
# reports it unused, and the tree's -Werror fails the build.
abs_build="$PWD/$build_dir"
if [ -n "${RUN_MSAN_TEST_BIN:-}" ]; then
    test_bin=$RUN_MSAN_TEST_BIN
else
    cmake -S . -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_C_FLAGS="$flags" \
        -DCMAKE_CXX_FLAGS="$flags" \
        -DCMAKE_EXE_LINKER_FLAGS="$flags $link_flags" \
        -DCMAKE_SHARED_LINKER_FLAGS="$flags" \
        -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$abs_build/bin" \
        -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$abs_build/lib" \
        -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY="$abs_build/lib"

    cmake --build "$build_dir" --target test --parallel

    test_bin="$abs_build/bin/test"
fi
msan_tmp=$(mktemp -d "${TMPDIR:-${RUNNER_TEMP:-/tmp}}/rocketnet-msan.XXXXXX")
trap 'rm -rf "$msan_tmp"' EXIT
log="$msan_tmp/msan.log"
reports="$msan_tmp/msan-reports"
known_file=tools/msan_known.txt
mkdir -p "$reports"

# Per-process logs keep each origin trace with its warning. The suite runner
# spawns tests in parallel, and a shared stderr interleaves those traces.
# exitcode=0 lets a test whose only problem is an MSan report pass, so the
# suite (which stops at its first failing test) runs every test; each report
# is judged below from the per-process logs against the known list.
export MSAN_OPTIONS="halt_on_error=0:print_stats=0:exitcode=0:log_path=${reports}/msan"

set +e
"$test_bin" >"$log" 2>&1
status=$?
set -e

python3 - "$log" "$reports" "$known_file" "$status" "$PWD" "$msan_exitcode" << 'PY'
import glob, os, re, sys

log_path, reports, known_file, status, root, exitcode = sys.argv[1:]
status = int(status)
exitcode = int(exitcode)
root = root.rstrip("/") + "/"

failed = []
saw_fail = False
if os.path.exists(log_path):
    for line in open(log_path, errors="replace"):
        m = re.match(r"^check failed: (test_[a-z0-9_]+ (?:signal \d+|exceeded .+|exit \d+|wait failed))\s*$", line)
        if not m:
            continue
        saw_fail = True
        detail = m.group(1)
        parts = re.match(r"^(.*) (signal \d+|exceeded .+|exit \d+|wait failed)$", detail)
        if parts is None:
            failed.append((detail, ""))
        elif parts.group(2) != "exit %d" % exitcode:
            failed.append((parts.group(1), parts.group(2)))

def shorten(line):
    line = line.replace(root, "")
    line = re.sub(r" \(BuildId: [0-9a-f]+\)", "", line)
    line = re.sub(r"\((?:[^()]*?)bin/test\+0x[0-9a-f]+\)", "(bin/test)", line)
    line = re.sub(r"/usr/(?:bin/\.\./)?lib/gcc/[^/]+/[0-9]+/(?:\.\./)+include/c\+\+/[0-9]+/", "include/c++/", line)
    line = re.sub(r"\s+$", "", line)
    return line

def frame(line):
    line = shorten(line)
    line = re.sub(r"^\s*#\d+\s+0x[0-9a-f]+\s+(?:in\s+)?", "", line)
    return line.strip()

texts = []
for path in sorted(glob.glob(os.path.join(reports, "msan.*"))):
    data = open(path, errors="replace").read()
    if "WARNING: MemorySanitizer:" in data:
        texts.append(data)
if not texts and os.path.exists(log_path):
    data = open(log_path, errors="replace").read()
    if "WARNING: MemorySanitizer:" in data:
        texts.append(data)

warnings = []
for text in texts:
    for part in re.split(r"(?=^==\d+==WARNING: MemorySanitizer:)", text, flags=re.M):
        lines = part.splitlines()
        if not lines or "WARNING: MemorySanitizer:" not in lines[0]:
            continue
        top = None
        for line in lines:
            if line.startswith("    #0 "):
                top = frame(line)
                break
        if not top:
            continue
        origin = []
        cap = False
        for line in lines:
            if "Uninitialized value" in line or line.strip().startswith("Origin"):
                cap = True
            if not cap:
                continue
            if line.startswith("SUMMARY:") or line.strip() == "Exiting":
                break
            if line.startswith("    #"):
                origin.append(frame(line))
            else:
                origin.append(line.strip())
            if len(origin) >= 8:
                break
        warnings.append((top, origin))

if not warnings:
    if status != 0:
        if failed:
            for name, detail in failed:
                print(("MSan: test failed: %s %s" % (name, detail)).rstrip())
        else:
            print("MSan: test run failed with status %d" % status)
        if os.path.exists(log_path):
            sys.stdout.write(open(log_path, errors="replace").read())
        sys.exit(1)
    print("MSan: clean")
    sys.exit(0)

known = set()
if os.path.exists(known_file):
    for line in open(known_file, errors="replace"):
        line = line.strip()
        if line and not line.startswith("#"):
            known.add(line)

seen = {}
order = []
for top, origin in warnings:
    if top not in seen:
        seen[top] = origin
        order.append(top)

print("MSan: findings (each with its origin trace):")
unmatched = []
for top in order:
    print(top)
    for line in seen[top]:
        print("  origin: " + line)
    if top not in known:
        unmatched.append(top)

status_failed = status != 0 and not saw_fail
if status_failed:
    print("MSan: test run failed with status %d" % status)

if unmatched:
    print("MSan: findings not in %s:" % known_file)
    for top in unmatched:
        print(top)
    print("report each in %s" % known_file)

for name, detail in failed:
    print(("MSan: test failed: %s %s" % (name, detail)).rstrip())

if unmatched or failed or status_failed:
    sys.exit(1)

print("MSan: findings listed in %s (all known)" % known_file)
sys.exit(0)
PY
