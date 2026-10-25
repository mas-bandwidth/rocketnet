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
# The C++ standard library is the MSan-instrumented libc++ for the active
# clang, /opt/msan-libcxx-<clang++ -dumpversion>, built by
# tools/build_msan_libcxx.sh (the fleet's provisioning puts it on
# every Linux runner host). The system libstdc++ is not instrumented: its
# compiled tree rebalance and string growth write memory the shadow never
# sees, and every later read of it is reported. A host without the library
# for its clang fails with a TOOL-MISSING line naming the script.
# RUN_MSAN_LIBCXX names another prefix holding that build.
#
# RUN_MSAN_TEST_BIN names a binary to run as the suite instead of configuring
# and building build-msan/bin/test, and RUN_MSAN_KNOWN_FILE a known list to
# judge it by instead of tools/msan_known.txt; both exist for the gate's own
# tests.
set -euo pipefail

cd "$(dirname "$0")/.."

build_dir=build-msan
msan_exitcode=86
# -O1, as MemorySanitizer's own documentation configures it: at -O0 every
# libc++ element operation is an out-of-line call that pays for its origin
# bookkeeping, and the suite's byte vectors made one case run 534 s on a Linux x64 bench
# (the whole gate is 103 s at -O1). MSan reports an uninitialised value where
# it is used, a use the optimizer keeps; the build stays Debug, asserts on.
flags="-O1 -fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer"

# The unit suite defines its own global operator new/delete (test_channels.cpp)
# to arm the allocation-failure trap; the MSan C++ runtime ships replacements
# too, and the driver links it ahead of the suite's objects, so with the
# duplicate allowed the runtime's operator new wins and the trap never fires
# (test_channels_allocator_trap_witness fails). -fno-sanitize-link-c++-runtime
# would leave it out, but clang 18 (the fleet runner's clang) links the C++
# runtime anyway under that flag. So the driver links no sanitizer runtime and
# the executable link names the MSan runtime itself, exactly as the driver
# would (whole archive, its exported-symbol list, its system libraries), minus
# the C++ part. The in-tree operator is then the only one; it calls malloc,
# which the MSan runtime still intercepts and poisons, so shadow tracking
# holds. These are link-step flags, so they go on the executable link line
# only: on a compile line clang reports them unused, and the tree's -Werror
# fails the build.
abs_build="$PWD/$build_dir"
if [ -n "${RUN_MSAN_TEST_BIN:-}" ]; then
    test_bin=$RUN_MSAN_TEST_BIN
else
    msan_runtime=$(clang++ -print-file-name="libclang_rt.msan-$(uname -m).a")
    if [ ! -f "$msan_runtime" ] || [ ! -f "$msan_runtime.syms" ]; then
        echo "MSan: runtime libclang_rt.msan-$(uname -m).a not found by clang++ -print-file-name"
        exit 1
    fi
    link_flags="-fno-sanitize-link-runtime -Wl,--whole-archive $msan_runtime -Wl,--no-whole-archive -Wl,--dynamic-list=$msan_runtime.syms -lm -lpthread -lrt -ldl -lresolv"

    clang_version=$(clang++ -dumpversion)
    libcxx=${RUN_MSAN_LIBCXX:-/opt/msan-libcxx-$clang_version}
    if [ ! -f "$libcxx/lib/libc++.so" ] || [ ! -f "$libcxx/include/c++/v1/__config_site" ]; then
        echo "TOOL-MISSING msan-libcxx $clang_version (no MSan-instrumented libc++ in $libcxx; bash tools/build_msan_libcxx.sh) on ${RUNNER_NAME:-$(hostname)}"
        exit 1
    fi
    # libc++'s own headers on the compile line, and its shared library in
    # place of libstdc++ on the link lines of the suite and of the SDK's
    # shared libraries (the suite loads the development one), found at run
    # time by rpath. The suite's own operator new still wins: the executable
    # defines it, as it did over libstdc++. -stdlib goes on the link lines
    # only, where clang uses it (on a compile line under -nostdinc++ it is
    # unused, and -Werror refuses an unused argument).
    cxx_flags="$flags -nostdinc++ -isystem $libcxx/include/c++/v1"
    libcxx_link="-stdlib=libc++ -L$libcxx/lib -Wl,-rpath,$libcxx/lib"
    link_flags="$link_flags $libcxx_link"

    cmake -S . -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_C_FLAGS="$flags" \
        -DCMAKE_CXX_FLAGS="$cxx_flags" \
        -DCMAKE_EXE_LINKER_FLAGS="$flags $link_flags" \
        -DCMAKE_SHARED_LINKER_FLAGS="$flags $libcxx_link" \
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
known_file=${RUN_MSAN_KNOWN_FILE:-tools/msan_known.txt}
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
