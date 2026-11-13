#!/usr/bin/env bash
#
# The NDEBUG fuzz campaign: every tests/fuzz/ harness, and the library under it,
# built with NDEBUG, so rocketnet_assert compiles to nothing
# (sdk/rocketnet_internal.h, ROCKETNET_ASSERTS) and a bound that only an
# assert holds is fuzzed the way a release build runs it.
#
# The nightly's other legs (.github/workflows/fuzz-nightly.yml) build the
# library Debug, so its asserts stay live; ci.yml's fuzz-smoke-ndebug puts
# -DNDEBUG on the harness line only and replays the corpora. This script
# configures its own build directory with -DCMAKE_BUILD_TYPE=Release (CMake
# adds -DNDEBUG) and the nightly's sanitizer flags, builds rocketnet_dev,
# checks that every library compile line carries -DNDEBUG and that no
# library object references the assert callback, then builds each harness
# with the nightly's compile and link lines plus -DNDEBUG and fuzzes it.
#
# Usage: tools/fuzz_ndebug.sh [--seconds N] [--target NAME] [--jobs N]
#                             [--out DIR] [--build-jobs N] [--phase P]
#   --seconds N     seconds to fuzz each target (default 60)
#   --target NAME   run one target (repeatable); default all sixteen
#   --jobs N        targets fuzzed at once (default 1)
#   --out DIR       reproducers, logs and scratch corpora (default
#                   build-fuzz-ndebug/out, which .gitignore covers)
#   --build-jobs N  build parallelism (default 4)
#   --phase P       all (default), build (configure, build and check the
#                   library and the harnesses, then stop) or fuzz (fuzz
#                   what a build phase left in the build directory)
#
# Environment: CC / CXX (default clang / clang++, which must carry the
# libFuzzer runtime); ASAN_OPTIONS / UBSAN_OPTIONS (default as the nightly's
# Linux leg). The seed corpora under tests/fuzz/ are read, never written: new
# inputs go to <out>/corpus/<target>. A crash writes its reproducer to
# <out>/crashes/<target>/ and the script exits 1 naming it.
#
# Linux (the nightly's link set: bpf, xdp, io_uring) or macOS (CoreFoundation
# and SystemConfiguration). Bash 4 or later (associative arrays, wait -n): on
# macOS, whose /bin/bash is 3.2, run it with a newer bash first on PATH
# (Homebrew's).

set -euo pipefail

if [ "${BASH_VERSINFO[0]}" -lt 4 ]; then
    echo "fuzz_ndebug.sh: needs bash 4 or later (associative arrays, wait -n); this is bash $BASH_VERSION" >&2
    exit 2
fi

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

ALL_TARGETS=(
    rocketnet_fuzz
    rocketnet_fuzz_connect_token
    rocketnet_fuzz_receive_structured
    rocketnet_fuzz_message
    rocketnet_fuzz_token_private
    rocketnet_fuzz_round_trip
    rocketnet_fuzz_config
    rocketnet_fuzz_server_transport
    rocketnet_fuzz_client_transport
    rocketnet_fuzz_session
    rocketnet_fuzz_session_raw
    rocketnet_fuzz_xdp_frame
    rocketnet_fuzz_uring_recvmsg
    rocketnet_fuzz_gateway_netlink
    rocketnet_fuzz_endpoint
    rocketnet_fuzz_xdp_filter
)
SECONDS_PER_TARGET=60
JOBS=1
BUILD_JOBS=4
PHASE=all
OUT="$ROOT/build-fuzz-ndebug/out"
TARGETS=()

usage() {
    sed -n '/^# Usage:/,/^# Environment:/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --seconds) SECONDS_PER_TARGET="$2"; shift 2 ;;
        --target) TARGETS+=("$2"); shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --build-jobs) BUILD_JOBS="$2"; shift 2 ;;
        --phase) PHASE="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "fuzz_ndebug: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

case "$PHASE" in
    all|build|fuzz) ;;
    *) echo "fuzz_ndebug: --phase '$PHASE' is not all, build or fuzz" >&2; exit 2 ;;
esac

for n in "$SECONDS_PER_TARGET" "$JOBS" "$BUILD_JOBS"; do
    if ! [[ "$n" =~ ^[1-9][0-9]*$ ]]; then
        echo "fuzz_ndebug: '$n' is not a positive integer" >&2
        exit 2
    fi
done

if [ ${#TARGETS[@]} -eq 0 ]; then
    TARGETS=("${ALL_TARGETS[@]}")
fi
for t in "${TARGETS[@]}"; do
    known=0
    for a in "${ALL_TARGETS[@]}"; do
        if [ "$t" = "$a" ]; then known=1; fi
    done
    if [ "$known" -eq 0 ]; then
        echo "fuzz_ndebug: unknown target '$t' (one of: ${ALL_TARGETS[*]})" >&2
        exit 2
    fi
done

CC=${CC:-clang}
CXX=${CXX:-clang++}
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}

BUILD="$ROOT/build-fuzz-ndebug"
BIN="$BUILD/bin"
SAN_LIB="-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -U_FORTIFY_SOURCE"
read -r -a SAN_LIB_ARGS <<< "$SAN_LIB"
SAN_BIN=("-fsanitize=fuzzer,address,undefined" -fno-sanitize-recover=all -fno-omit-frame-pointer)

if [ "$(uname -s)" = "Darwin" ]; then
    LINK=(-lpthread -framework CoreFoundation -framework SystemConfiguration)
else
    LINK=(-lbpf -lxdp -luring -lpthread)
fi

mkdir -p "$OUT/logs" "$OUT/corpus" "$OUT/crashes" "$BIN"
OUT=$(cd "$OUT" && pwd)

build_all() {
# 1. the library: Release (NDEBUG) with the nightly's instrumentation
echo "== configure $BUILD (Release, NDEBUG, fuzzer-no-link + asan + ubsan)"
cmake -S "$ROOT" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_C_FLAGS="$SAN_LIB" \
    -DCMAKE_CXX_FLAGS="$SAN_LIB" >"$OUT/logs/configure.log"
echo "== build rocketnet_dev"
cmake --build "$BUILD" --target rocketnet_dev --parallel "$BUILD_JOBS" >"$OUT/logs/build-library.log"
LIB="$BUILD/librocketnet_dev.a"

# 2. prove the library is NDEBUG: every sdk/ compile line carries -DNDEBUG,
# and no library object references the assert callback.
# (awk reads the whole file: an early-exit grep -m1 would SIGPIPE the
# pipeline under pipefail)
lib_commands=$(awk '/"command"/ && /rocketnet_dev\.dir\// && /\/sdk\//' "$BUILD/compile_commands.json")
sdk_lines=$(printf '%s\n' "$lib_commands" | grep -c . || [ $? -eq 1 ])
sdk_ndebug=$(printf '%s\n' "$lib_commands" | grep -c -- ' -DNDEBUG ' || [ $? -eq 1 ])
if [ "$sdk_lines" -eq 0 ] || [ "$sdk_lines" -ne "$sdk_ndebug" ]; then
    echo "NDEBUG-LIBRARY FAIL: $sdk_ndebug of $sdk_lines rocketnet_dev sdk/ compile lines carry -DNDEBUG" >&2
    exit 1
fi
echo "NDEBUG-LIBRARY compile: $sdk_ndebug of $sdk_lines rocketnet_dev sdk/ compile lines carry -DNDEBUG"
printf '%s\n' "$lib_commands" | awk '!shown && /\/sdk\/rocketnet\.cpp"/ { sub(/^ *"command": */, ""); print; shown = 1 }' \
    | tee "$OUT/logs/ndebug-compile-line.txt"
# nm on its own line: an unreadable library stops here instead of reading as
# "no reference"; grep exits 1 when nothing references the callback
symbols=$(nm -A "$LIB")
referencing=$(printf '%s\n' "$symbols" | grep -E ' U .*rn_assert_function') || [ $? -eq 1 ]
if [ -n "$referencing" ]; then
    echo "NDEBUG-LIBRARY FAIL: library objects still reference rn_assert_function:" >&2
    echo "$referencing" >&2
    exit 1
fi
echo "NDEBUG-LIBRARY nm: no object in $(basename "$LIB") references rn_assert_function (rocketnet_assert compiled out)"

# 3. the harnesses: the nightly's compile and link line plus -DNDEBUG
build_target() {
    local t="$1"
    # bash 3.2 under set -u calls an empty "${extra[@]}" unbound, hence the
    # ${extra[@]+...} form below
    local extra=()
    # the XDP filter as host C (tests/xdp_host/), instrumented like the
    # library; Linux only, elsewhere the target compiles to a no-op
    if [ "$t" = rocketnet_fuzz_xdp_filter ] && [ "$(uname -s)" = Linux ]; then
        "$CC" -g -O1 -std=gnu11 -ffreestanding -ffp-contract=off -D__linux__ -D__BPF__ -DNDEBUG \
            "${SAN_LIB_ARGS[@]}" \
            -Itests/xdp_host -Isdk "-DROCKETNET_XDP_FILTER_SOURCE=\"$ROOT/sdk/xdp/server_xdp.c\"" \
            -c tests/xdp_host/server_xdp_host.c -o "$BUILD/xdp_host_fuzz.o"
        extra=(-DROCKETNET_FUZZ_XDP_FILTER_HOST=1 "$BUILD/xdp_host_fuzz.o")
    fi
    "$CXX" -g -O1 -std=c++17 \
        -ffp-contract=off \
        -DROCKETNET_DEV=1 -DNDEBUG \
        "${SAN_BIN[@]}" \
        -Isodium -Isdk -Itests ${extra[@]+"${extra[@]}"} "tests/fuzz/$t.cpp" "$LIB" \
        "${LINK[@]}" \
        -o "$BIN/$t"
}

for t in "${TARGETS[@]}"; do
    echo "== build $t (NDEBUG)"
    if ! build_target "$t" >"$OUT/logs/build-$t.log" 2>&1; then
        cat "$OUT/logs/build-$t.log" >&2
        echo "BUILD FAIL $t" >&2
        exit 1
    fi
done
}

# --phase build stops here and --phase fuzz starts here, so a caller can put
# a deadline on the build alone (the fuzz campaign is never cut short)
if [ "$PHASE" != fuzz ]; then
    build_all
fi
if [ "$PHASE" = build ]; then
    echo "== built ${#TARGETS[@]} target(s) in $BIN"
    exit 0
fi
for t in "${TARGETS[@]}"; do
    if [ ! -x "$BIN/$t" ]; then
        echo "fuzz_ndebug: $BIN/$t is not built (run --phase build first)" >&2
        exit 2
    fi
done

# 4. fuzz: seeds and dictionary as the nightly's leg names them
seeds_of() {
    if [ "$1" = rocketnet_fuzz ]; then echo tests/fuzz/corpus; else echo "tests/fuzz/corpus_${1#rocketnet_fuzz_}"; fi
}

dict_of() {
    if [ -f "tests/fuzz/dict/$1.dict" ]; then echo "tests/fuzz/dict/$1.dict"; else echo tests/fuzz/rocketnet.dict; fi
}

fuzz_target() {
    local t="$1"
    mkdir -p "$OUT/corpus/$t" "$OUT/crashes/$t"
    "$BIN/$t" "$OUT/corpus/$t" "$(seeds_of "$t")" \
        -dict="$(dict_of "$t")" \
        -artifact_prefix="$OUT/crashes/$t/" \
        -max_total_time="$SECONDS_PER_TARGET" -print_final_stats=1 -rss_limit_mb=4096 -timeout=25 \
        >"$OUT/logs/fuzz-$t.log" 2>&1
}

echo "== fuzz ${#TARGETS[@]} target(s), ${SECONDS_PER_TARGET}s each, $JOBS at a time; output $OUT"
# bash 3.2 (macOS's /bin/bash) has neither associative arrays nor wait -n:
# the pool is a queue of pids, and a full pool waits for its oldest job
pids=()
for t in "${TARGETS[@]}"; do
    if [ "${#pids[@]}" -ge "$JOBS" ]; then
        wait "${pids[0]}" 2>/dev/null || true
        pids=("${pids[@]:1}")
    fi
    fuzz_target "$t" &
    pids+=("$!")
done

failed=0
if [ "${#pids[@]}" -gt 0 ]; then
    for pid in "${pids[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
fi

# the exit status of a reaped background job is gone, so a target's result
# is read from its log: libFuzzer prints "Done N runs" only on a clean finish
for t in "${TARGETS[@]}"; do
    log="$OUT/logs/fuzz-$t.log"
    if [ ! -f "$log" ]; then
        failed=1
        echo "FAIL $t: no log $log"
        continue
    fi
    done_line=$(grep -E '^Done [0-9]+ runs' "$log" | tail -n 1 || [ $? -eq 1 ])
    execs=$(grep -E '^stat::number_of_executed_units' "$log" | tail -n 1 | awk '{print $2}' || [ $? -eq 1 ])
    reproducers=$(find "$OUT/crashes/$t" -type f 2>/dev/null | sort)
    if [ -n "$done_line" ] && [ -z "$reproducers" ]; then
        echo "PASS $t: $done_line (executed units ${execs:-?})"
        # a clean finish is not enough: the run must reach its target's floor
        # of coverage and executed units (tools/fuzz_floors.tsv)
        if ! python3 "$ROOT/tools/check_fuzz_stats.py" --target "$t" --seconds "$SECONDS_PER_TARGET" "$log"; then
            failed=1
            echo "FAIL $t: under its floor; log $log"
        fi
    else
        failed=1
        echo "FAIL $t: no clean finish; log $log"
        grep -m 5 -E 'ERROR: (AddressSanitizer|LeakSanitizer|libFuzzer)|runtime error:|SUMMARY:' "$log" || [ $? -eq 1 ]
        if [ -n "$reproducers" ]; then
            while IFS= read -r r; do echo "  REPRODUCER $t $r"; done <<<"$reproducers"
        fi
    fi
done

exit "$failed"
