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
#                             [--out DIR] [--build-jobs N]
#   --seconds N     seconds to fuzz each target (default 60)
#   --target NAME   run one target (repeatable); default all eleven
#   --jobs N        targets fuzzed at once (default 1)
#   --out DIR       reproducers, logs and scratch corpora (default
#                   build-fuzz-ndebug/out, which .gitignore covers)
#   --build-jobs N  build parallelism (default 4)
#
# Environment: CC / CXX (default clang / clang++, which must carry the
# libFuzzer runtime); ASAN_OPTIONS / UBSAN_OPTIONS (default as the nightly's
# Linux leg). The seed corpora under tests/fuzz/ are read, never written: new
# inputs go to <out>/corpus/<target>. A crash writes its reproducer to
# <out>/crashes/<target>/ and the script exits 1 naming it.
#
# Linux (the nightly's link set: bpf, xdp, io_uring) or macOS (CoreFoundation
# and SystemConfiguration); rocketnet_fuzz_differential checks out the pinned
# netcode reference into the build directory, as the nightly does.

set -euo pipefail

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
    rocketnet_fuzz_differential
)

# the pinned netcode reference the nightly's differential leg builds against
NETCODE_REPO=https://github.com/mas-bandwidth/netcode.git
NETCODE_SHA=1dd10f892f4742498a99a0bb5d9b1f8f6e94f26b

SECONDS_PER_TARGET=60
JOBS=1
BUILD_JOBS=4
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
        -h|--help) usage; exit 0 ;;
        *) echo "fuzz_ndebug: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

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
SAN_BIN=("-fsanitize=fuzzer,address,undefined" -fno-sanitize-recover=all -fno-omit-frame-pointer)

if [ "$(uname -s)" = "Darwin" ]; then
    LINK=(-lpthread -framework CoreFoundation -framework SystemConfiguration)
else
    LINK=(-lbpf -lxdp -luring -lpthread)
fi

mkdir -p "$OUT/logs" "$OUT/corpus" "$OUT/crashes" "$BIN"
OUT=$(cd "$OUT" && pwd)

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
sdk_lines=$(printf '%s\n' "$lib_commands" | grep -c . || true)
sdk_ndebug=$(printf '%s\n' "$lib_commands" | grep -c -- ' -DNDEBUG ' || true)
if [ "$sdk_lines" -eq 0 ] || [ "$sdk_lines" -ne "$sdk_ndebug" ]; then
    echo "NDEBUG-LIBRARY FAIL: $sdk_ndebug of $sdk_lines rocketnet_dev sdk/ compile lines carry -DNDEBUG" >&2
    exit 1
fi
echo "NDEBUG-LIBRARY compile: $sdk_ndebug of $sdk_lines rocketnet_dev sdk/ compile lines carry -DNDEBUG"
printf '%s\n' "$lib_commands" | awk '!shown && /\/sdk\/rocketnet\.cpp"/ { sub(/^ *"command": */, ""); print; shown = 1 }' \
    | tee "$OUT/logs/ndebug-compile-line.txt"
referencing=$(nm -A "$LIB" 2>/dev/null | grep -E ' U .*rn_assert_function' || true)
if [ -n "$referencing" ]; then
    echo "NDEBUG-LIBRARY FAIL: library objects still reference rn_assert_function:" >&2
    echo "$referencing" >&2
    exit 1
fi
echo "NDEBUG-LIBRARY nm: no object in $(basename "$LIB") references rn_assert_function (rocketnet_assert compiled out)"

# 3. the harnesses: the nightly's compile and link line plus -DNDEBUG
build_target() {
    local t="$1"
    local extra=()
    if [ "$t" = rocketnet_fuzz_differential ]; then
        local ref="$BUILD/netcode-ref"
        if [ ! -d "$ref/.git" ]; then
            git clone --quiet "$NETCODE_REPO" "$ref"
        fi
        git -C "$ref" checkout --quiet "$NETCODE_SHA"
        "$CC" -O2 -DNDEBUG -ffp-contract=off -c "$ref/netcode.c" -o "$BUILD/netcode_ref.o" -I "$ref" -I sodium
        extra=(-Itools/differential "-I$ref" -DROCKETNET_DIFFERENTIAL_NETCODE_DIR "$BUILD/netcode_ref.o")
    fi
    "$CXX" -g -O1 -std=c++17 \
        -ffp-contract=off \
        -DROCKETNET_DEV=1 -DNDEBUG \
        "${SAN_BIN[@]}" \
        -Isodium -Isdk "${extra[@]}" "tests/fuzz/$t.cpp" "$LIB" \
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
declare -A PID_TARGET=()
running=0
for t in "${TARGETS[@]}"; do
    if [ "$running" -ge "$JOBS" ]; then
        wait -n || true
        running=$((running - 1))
    fi
    fuzz_target "$t" &
    PID_TARGET[$!]="$t"
    running=$((running + 1))
done

failed=0
for pid in "${!PID_TARGET[@]}"; do
    wait "$pid" 2>/dev/null || true
done

# the exit status of a reaped background job is gone, so a target's result
# is read from its log: libFuzzer prints "Done N runs" only on a clean finish
for t in "${TARGETS[@]}"; do
    log="$OUT/logs/fuzz-$t.log"
    done_line=$(grep -E '^Done [0-9]+ runs' "$log" | tail -n 1 || true)
    execs=$(grep -E '^stat::number_of_executed_units' "$log" | tail -n 1 | awk '{print $2}' || true)
    reproducers=$(find "$OUT/crashes/$t" -type f 2>/dev/null | sort)
    if [ -n "$done_line" ] && [ -z "$reproducers" ]; then
        echo "PASS $t: $done_line (executed units ${execs:-?})"
    else
        failed=1
        echo "FAIL $t: no clean finish; log $log"
        grep -E 'ERROR: (AddressSanitizer|LeakSanitizer|libFuzzer)|runtime error:|SUMMARY:' "$log" | head -n 5 || true
        if [ -n "$reproducers" ]; then
            while IFS= read -r r; do echo "  REPRODUCER $t $r"; done <<<"$reproducers"
        fi
    fi
done

exit "$failed"
