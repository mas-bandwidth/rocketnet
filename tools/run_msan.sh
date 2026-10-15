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
# driver, so the run is the portable configuration: no XDP dataplane, no
# io_uring. The instrumented binary lives under build-msan/bin so a later
# `make && ./bin/test` is not a stale sanitizer build.
#
# Exit status: 0 when the run is clean or every finding is listed in
# tools/msan_known.txt; 1 when a finding is not listed there. A finding is the
# top frame of a MemorySanitizer warning. Its origin trace is printed with it.
set -euo pipefail

cd "$(dirname "$0")/.."

build_dir=build-msan
flags="-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer"
link_flags="-Wl,--allow-multiple-definition"

# The unit suite defines its own global operator new/delete (test_channels.cpp)
# to arm the allocation-failure trap; the MSan C++ runtime ships replacements
# too. Allow the duplicate so the instrumented in-tree operator wins; malloc is
# still intercepted and poisoned by the MSan runtime, so shadow tracking holds.
abs_build="$PWD/$build_dir"
cmake -S . -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS="$flags" \
    -DCMAKE_CXX_FLAGS="$flags $link_flags" \
    -DCMAKE_EXE_LINKER_FLAGS="$flags $link_flags" \
    -DCMAKE_SHARED_LINKER_FLAGS="$flags" \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$abs_build/bin" \
    -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$abs_build/lib" \
    -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY="$abs_build/lib"

cmake --build "$build_dir" --target test --parallel

test_bin="$abs_build/bin/test"
msan_tmp=$(mktemp -d "${TMPDIR:-${RUNNER_TEMP:-/tmp}}/rocketnet-msan.XXXXXX")
trap 'rm -rf "$msan_tmp"' EXIT
log="$msan_tmp/msan.log"
reports="$msan_tmp/msan-reports"
known_file=tools/msan_known.txt
mkdir -p "$reports"

# Per-process logs keep each origin trace with its warning. The suite runner
# spawns tests in parallel, and a shared stderr interleaves those traces.
export MSAN_OPTIONS="halt_on_error=0:print_stats=0:log_path=${reports}/msan"

set +e
"$test_bin" >"$log" 2>&1
status=$?
set -e

python3 - "$log" "$reports" "$known_file" "$status" "$PWD" << 'PY'
import glob, os, re, sys

log_path, reports, known_file, status, root = sys.argv[1:]
status = int(status)
root = root.rstrip("/") + "/"

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
        print("MSan: test run failed without a MemorySanitizer report")
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

if unmatched:
    print("MSan: findings not in %s:" % known_file)
    for top in unmatched:
        print(top)
    print("report each in %s" % known_file)
    sys.exit(1)

print("MSan: findings listed in %s (all known)" % known_file)
sys.exit(0)
PY
