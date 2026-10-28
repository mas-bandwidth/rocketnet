#!/usr/bin/env bash
#
# Source coverage of the fuzz harnesses, per target and per area: yojimbo's
# tools/fuzz_coverage.sh for rocketnet. Builds the fuzz_replay_* targets with
# source-based coverage, runs each over its committed corpus plus a batch of
# pseudo-random inputs (the replay driver's standalone mode), and prints, for
# every area of the untrusted-input surface, the lines and functions each
# target reaches and the union of all of them. Used the same way before and
# after a change, it is the comparable number for that change.
#
# Usage: tools/fuzz_coverage.sh [target ...]
#   targets are fuzz harness names (rocketnet_fuzz, rocketnet_fuzz_session,
#   ...); the default is every fuzz/rocketnet_fuzz*.cpp with a fuzz_replay_
#   target in CMakeLists.txt.
#
# Environment:
#   FUZZ_COV_ITERS   random inputs per target after its corpus (default 2000)
#   FUZZ_COV_BUILD   the coverage build directory (default build-fuzz-cov)
#   FUZZ_COV_OUT     where profiles and reports go (default a fresh temp dir)
#   FUZZ_COV_JOBS    build parallelism (default 8)
#   FUZZ_COV_SETUP   1 counts the pair targets' one-time create, connect and
#                    save as covered (default 0: the pair harnesses zero
#                    their counters once setup is done, so their numbers are
#                    what inputs reach; ROCKETNET_FUZZ_COVERAGE_INPUTS_ONLY)
#   FUZZ_COV_SUMMARY  a file to append the per-area table to as a Markdown
#                    table (header row, separator row, then one row per
#                    area), for a CI job summary; $GITHUB_STEP_SUMMARY in a
#                    workflow. Empty/unset by default (no summary written).
#   SLOT             a command prefix for the heavy steps, e.g. a bench's
#                    slot-lock script (default empty)
#   LLVM_PROFDATA / LLVM_COV   the tools (default xcrun's on macOS, else
#                    llvm-profdata / llvm-cov on PATH)
#
# Profiles are written with %c (continuous mode) and %p, and merged without
# -sparse, so a function run once still counts. The coverage objects are the
# development library's own (the recipe pass 0 used); the pair harnesses
# compile the server transport and the loopback driver into themselves, and
# their counts merge into the same functions by name and hash.
#
# A local dev tool, not a CI gate. The fuzz_replay_* binaries land in bin/ at
# the repository root, as every CMake build here does.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ITERS="${FUZZ_COV_ITERS:-2000}"
BUILD="${FUZZ_COV_BUILD:-build-fuzz-cov}"
OUT="${FUZZ_COV_OUT:-$(mktemp -d)}"
JOBS="${FUZZ_COV_JOBS:-8}"
SLOT="${SLOT:-}"
if [ "${FUZZ_COV_SETUP:-0}" = "1" ]; then
    export ROCKETNET_FUZZ_COVERAGE_INPUTS_ONLY=0
else
    export ROCKETNET_FUZZ_COVERAGE_INPUTS_ONLY=1
fi

if [ -z "${LLVM_PROFDATA:-}" ]; then
    if command -v xcrun >/dev/null 2>&1; then LLVM_PROFDATA="xcrun llvm-profdata"; else LLVM_PROFDATA="llvm-profdata"; fi
fi
if [ -z "${LLVM_COV:-}" ]; then
    if command -v xcrun >/dev/null 2>&1; then LLVM_COV="xcrun llvm-cov"; else LLVM_COV="llvm-cov"; fi
fi

# the areas of the untrusted-input surface, in exposure order: pre-auth first
AREAS=(
  "server_transport_and_request:sdk/rocketnet_server_transport.cpp sdk/rocketnet_server_protocol.cpp"
  "client_transport_and_challenge:sdk/rocketnet_client_transport.cpp sdk/rocketnet_client_protocol.cpp"
  "protocol_core:sdk/rocketnet_protocol.cpp sdk/rocketnet_replay.cpp"
  "connect_token:sdk/rocketnet_connect_token.cpp"
  "channels_and_messages:$(ls sdk/rocketnet_message*.cpp sdk/rocketnet_channel*.cpp sdk/rocketnet_window_*.cpp | tr '\n' ' ')"
  "fragments:sdk/rocketnet_fragment.cpp"
  "acks_and_receipts:sdk/rocketnet_ack.cpp sdk/rocketnet_channel_receipts.cpp"
  "config_and_env:sdk/rocketnet_config.cpp sdk/rocketnet_base64.cpp sdk/rocketnet_address.cpp"
)

seeds_for()
{
    case "$1" in
        rocketnet_fuzz)                    echo fuzz/corpus ;;
        rocketnet_fuzz_message)            echo fuzz/corpus_message ;;
        rocketnet_fuzz_connect_token)      echo fuzz/corpus_connect_token ;;
        rocketnet_fuzz_receive_structured) echo fuzz/corpus_receive_structured ;;
        *)                                 echo "fuzz/corpus_${1#rocketnet_fuzz_}" ;;
    esac
}

if [ "$#" -gt 0 ]; then
    TARGETS=("$@")
else
    TARGETS=()
    for src in fuzz/rocketnet_fuzz*.cpp; do
        t="$(basename "$src" .cpp)"
        grep -q "$t" CMakeLists.txt && TARGETS+=("$t")
    done
fi

# ROCKETNET_FUZZ_COVERAGE_BUILD lets the pair harnesses call the profile
# runtime (they zero the counters after their one-time setup); no other build
# references it
COVFLAGS="-fprofile-instr-generate -fcoverage-mapping -DROCKETNET_FUZZ_COVERAGE_BUILD=1"
# continuous mode (%c) on Linux needs the counters relocated at run time;
# without it the runtime cannot map them and writes an empty profile
if [ "$(uname -s)" = "Linux" ]; then
    COVFLAGS="$COVFLAGS -mllvm -runtime-counter-relocation"
fi
# a build directory configured with other flags would build without the
# define or the relocation flag and report wrong numbers; refuse it
if [ -f "$BUILD/CMakeCache.txt" ] && ! grep -q '^CMAKE_CXX_FLAGS:.*-DROCKETNET_FUZZ_COVERAGE_BUILD=1' "$BUILD/CMakeCache.txt"; then
    echo "fuzz_coverage: $BUILD was configured without the current coverage flags; delete it and rerun" >&2
    exit 1
fi
if [ ! -f "$BUILD/CMakeCache.txt" ]; then
    # macOS's bash 3.2 treats an empty "${a[@]}" as unbound under set -u,
    # so the generator is expanded only when ninja set it
    GENERATOR=()
    command -v ninja >/dev/null 2>&1 && GENERATOR=(-G Ninja)
    echo "configuring $BUILD with coverage instrumentation"
    $SLOT cmake -S . -B "$BUILD" ${GENERATOR[@]+"${GENERATOR[@]}"} -DCMAKE_BUILD_TYPE=Debug \
        -DROCKETNET_BUILD_TESTS=ON -DROCKETNET_BUILD_EXAMPLES=OFF -DROCKETNET_BUILD_TOOLS=ON \
        -DROCKETNET_XDP=OFF -DROCKETNET_HARDEN=OFF \
        -DCMAKE_C_FLAGS="$COVFLAGS" -DCMAKE_CXX_FLAGS="$COVFLAGS" >"$OUT/configure.log" 2>&1
fi

echo "building ${TARGETS[*]}"
build_targets=()
for t in "${TARGETS[@]}"; do build_targets+=(--target "fuzz_replay_$t"); done
$SLOT cmake --build "$BUILD" "${build_targets[@]}" --parallel "$JOBS" >"$OUT/build.log" 2>&1

mkdir -p "$OUT/raw"
for t in "${TARGETS[@]}"; do
    bin="bin/fuzz_replay_$t"
    seeds="$(seeds_for "$t")"
    if [ -d "$seeds" ] && [ -n "$(ls -A "$seeds")" ]; then
        echo "running $t over $seeds"
        LLVM_PROFILE_FILE="$OUT/raw/$t-corpus-%p%c.profraw" $SLOT "$bin" "$seeds"/* >/dev/null 2>&1
    fi
    if [ "$ITERS" -gt 0 ]; then
        echo "running $t over $ITERS random inputs"
        LLVM_PROFILE_FILE="$OUT/raw/$t-random-%p%c.profraw" FUZZ_ITERS="$ITERS" $SLOT "$bin" >/dev/null 2>&1
    fi
    $LLVM_PROFDATA merge "$OUT"/raw/"$t"-*.profraw -o "$OUT/$t.profdata"
done
$LLVM_PROFDATA merge "$OUT"/*.profdata -o "$OUT/all.profdata.tmp"
mv "$OUT/all.profdata.tmp" "$OUT/all.profdata"

OBJECTS=()
while IFS= read -r o; do OBJECTS+=(-object "$o"); done < <(find "$BUILD/CMakeFiles/rocketnet_dev.dir/sdk" -name '*.o' | sort)
PRIMARY="bin/fuzz_replay_${TARGETS[0]}"
for t in "${TARGETS[@]:1}"; do OBJECTS+=(-object "bin/fuzz_replay_$t"); done

# "lines covered/total (pct) functions executed/total" from llvm-cov's TOTAL row
total_row()
{
    # TOTAL regions missed cover functions missed executed lines missed cover ...
    awk '/^TOTAL/ { printf "%d/%d %s fn %d/%d", $8 - $9, $8, $10, $5 - $6, $5 }'
}

report()
{
    local profile="$1"; shift
    $LLVM_COV report "$PRIMARY" "${OBJECTS[@]}" -instr-profile="$profile" "$@" 2>/dev/null | total_row
}

echo
echo "=== fuzz coverage per area (lines covered/total, line cover, functions executed/total) ==="
cols=()
printf "%-32s" "area"
for t in "${TARGETS[@]}"; do cols+=("${t#rocketnet_}"); printf " | %s" "${t#rocketnet_}"; done
cols+=("all")
printf " | all\n"
rows=()
for entry in "${AREAS[@]}"; do
    area="${entry%%:*}"
    read -r -a files <<< "${entry#*:}"
    row="$area"
    printf "%-32s" "$area"
    for t in "${TARGETS[@]}"; do v="$(report "$OUT/$t.profdata" "${files[@]}")"; row="$row | $v"; printf " | %s" "$v"; done
    v="$(report "$OUT/all.profdata" "${files[@]}")"; row="$row | $v"; printf " | %s\n" "$v"
    rows+=("$row")
done
if [ -n "${FUZZ_COV_SUMMARY:-}" ]; then
    {
        echo
        echo "### fuzz coverage per area (lines covered/total, line cover, functions executed/total)"
        printf "| area"
        for c in "${cols[@]}"; do printf " | %s" "$c"; done
        printf " |\n"
        printf "|---"
        for c in "${cols[@]}"; do printf " | ---"; done
        printf " |\n"
        for row in "${rows[@]}"; do printf "| %s |\n" "$row"; done
    } >> "$FUZZ_COV_SUMMARY"
fi

echo
echo "=== per target, every sdk file ==="
for t in "${TARGETS[@]}" all; do
    printf "%-36s %s\n" "$t" "$(report "$OUT/$t.profdata" "$ROOT/sdk")"
done

$LLVM_COV report "$PRIMARY" "${OBJECTS[@]}" -instr-profile="$OUT/all.profdata" "$ROOT/sdk" > "$OUT/all_vs_sdk.report.txt" 2>/dev/null || true
echo
echo "(profiles and the full report in $OUT)"
