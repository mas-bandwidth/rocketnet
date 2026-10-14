#!/usr/bin/env bash
# Mutation gate for the parser, replay, ack, fragment and message core.
#
# What it is
# ----------
# A scheduled mutation run (RN-CORRECTNESS 2026-10-14, R4): the one
# hand-written mutant of tools/check_fuzz_oracle.py is generalised into a
# real LLVM-IR mutation pass over the five wire-facing files, driven by
# ./bin/test. Mull builds the library with its compiler plugin, runs the unit
# suite against each mutant, and this script reduces Mull's report to a
# per-file killed/survived count and writes tools/mutation/floor.tsv.
#
# The floor is the surviving-mutant count per file on BASE. A run with more
# survivors than the floor fails (exit 1); a run with fewer survivors shrinks
# the floor in the same pull request. The first run on a fresh floor only
# establishes the baseline and never fails.
#
# Contract (the DONE-WHEN)
# ------------------------
#     bash tools/mutation/run_mull.sh
# on a bench with Mull prints the per-file killed/survived counts and writes
# floor.tsv. On a bench without Mull it refuses loudly (exit 3) and writes
# nothing: an absent tool is not a green gate.
#
# Mull is the tool the card names. It ships a versioned entry point
# (mull-runner-<llvm>, e.g. mull-runner-21) plus a compiler plugin
# (mull-ir-frontend-<llvm>) that embeds the mutants when the library is
# compiled. If the bench carries another LLVM-IR mutation tool, point
# MULL_RUNNER at its entry point; the report must still carry one
# "file:line:col: warning: (Killed|Survived):" line per mutant.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MUT_DIR="$ROOT/tools/mutation"
FLOOR="$MUT_DIR/floor.tsv"
BUILD="$ROOT/build-mull"

# The five parser/replay/ack/fragment/message sources this gate mutates.
FILES=(
    sdk/rocketnet_base64.cpp
    sdk/rocketnet_replay.cpp
    sdk/rocketnet_ack.cpp
    sdk/rocketnet_fragment.cpp
    sdk/rocketnet_message.cpp
)

# The mutators that land on boundary constants: replace an operator, drop a
# branch, fold a comparison to a constant. Boundary constants are where the
# card expects the hurt (4096, 64, word counts, the >= and % edges).
MUTATORS=(
    cxx_add_to_sub
    cxx_mul_to_div
    cxx_div_to_mul
    cxx_rem_to_div
    cxx_and_to_or
    cxx_or_to_and
    cxx_lshift_to_rshift
    cxx_rshift_to_lshift
    cxx_lt_to_ge
    cxx_le_to_gt
    cxx_gt_to_ge
    cxx_ge_to_gt
    cxx_eq_to_ne
    cxx_ne_to_eq
    cxx_remove_negation
    cxx_remove_void_call
    cxx_replace_scalar_call
    cxx_assign_const
    cxx_init_const
)

say() { printf 'run_mull: %s\n' "$*"; }

fail() { say "$*" >&2; exit "${2:-1}"; }

# Locate Mull. Modern Mull ships a versioned entry point
# (mull-runner-18, mull-runner-21, ...) and keeps the older mull-cxx for
# pre-0.17 layouts. MULL_RUNNER overrides either, so a bench with a different
# build of the plugin can still drive this gate.
find_runner() {
    local d pat
    for d in ${PATH//:/ }; do
        [ -d "$d" ] || continue
        for pat in "$d"/mull-runner-* "$d"/mull-runner "$d"/mull-cxx; do
            [ -x "$pat" ] || continue
            printf '%s\n' "$pat"
            return 0
        done
    done
    return 1
}

# The compiler plugin lives in a lib/ directory beside the runner's bin/:
#   <prefix>/bin/mull-runner-21  and  <prefix>/lib/mull-ir-frontend-21
find_frontend() {
    local runner_dir lib_dir cand
    if [ -n "${MULL_FRONTEND:-}" ] && [ -f "$MULL_FRONTEND" ]; then
        printf '%s\n' "$MULL_FRONTEND"
        return 0
    fi
    runner_dir="$(dirname "$MULL_RUNNER")"
    lib_dir="$(dirname "$runner_dir")/lib"
    for cand in "$lib_dir"/mull-ir-frontend-* "$lib_dir"/mull-ir-frontend; do
        [ -f "$cand" ] || continue
        printf '%s\n' "$cand"
        return 0
    done
    return 1
}

# A clang whose LLVM matches the plugin. Derive the LLVM major from the
# frontend file name (mull-ir-frontend-21 -> clang-21), and fall back to the
# bare clang/clang++ on PATH.
find_clang() {
    local ver="$1" c cxx
    c="${MULL_CC:-}"
    cxx="${MULL_CXX:-}"
    if [ -z "$c" ] && [ -n "$ver" ]; then
        c="$(command -v "clang-$ver" 2>/dev/null || true)"
    fi
    if [ -z "$c" ]; then
        c="$(command -v clang 2>/dev/null || true)"
    fi
    if [ -z "$cxx" ] && [ -n "$ver" ]; then
        cxx="$(command -v "clang++-$ver" 2>/dev/null || true)"
    fi
    if [ -z "$cxx" ]; then
        cxx="$(command -v clang++ 2>/dev/null || true)"
    fi
    [ -n "$c" ] && [ -n "$cxx" ] || return 1
    printf '%s %s\n' "$c" "$cxx"
}

MULL_RUNNER="${MULL_RUNNER:-}"
if [ -z "$MULL_RUNNER" ]; then
    MULL_RUNNER="$(find_runner || true)"
fi

if [ -z "$MULL_RUNNER" ]; then
    say "Mull is absent: no mull-runner-*, mull-runner or mull-cxx on PATH (nor is MULL_RUNNER set)." >&2
    say "The gate is not-done on this bench: no LLVM-IR mutation tool to run, so no floor is written and no green claim is made." >&2
    exit 3
fi

FRONTEND="$(find_frontend || true)"
if [ -z "$FRONTEND" ]; then
    say "found $MULL_RUNNER but no mull-ir-frontend plugin beside it (nor is MULL_FRONTEND set)." >&2
    say "The gate is not-done on this bench: nothing embeds the mutants, so no floor is written." >&2
    exit 3
fi

# The plugin version drives the compiler choice; the two must match LLVM.
ver="$(basename "$FRONTEND")"
ver="${ver#mull-ir-frontend-}"
case "$ver" in
    *[!0-9]*) ver="" ;;   # not a plain version; fall back to bare clang
esac

read -r MULL_CC MULL_CXX <<< "$(find_clang "$ver" || true)"
if [ -z "$MULL_CC" ]; then
    say "no clang on PATH to drive the Mull plugin (clang-$ver or clang)." >&2
    say "The gate is not-done on this bench: no matching compiler, so no floor is written." >&2
    exit 3
fi

say "using $MULL_RUNNER (frontend $FRONTEND, compiler $MULL_CC)"

# The instrumented build is self-contained: everything it produces lands under
# $BUILD, so the normal bin/ tree (the make front door) is untouched. The
# plugin flag embeds the mutants; -g keeps the source map the plugin needs.
# RelWithDebInfo keeps the suite fast (-O2) while the -DNDEBUG is what the
# tree's own Release leg runs, so the tests exercise the shipped shape.
if [ ! -f "$BUILD/CMakeCache.txt" ]; then
    say "configuring instrumented build at $BUILD"
    cmake -S "$ROOT" -B "$BUILD" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_COMPILER="$MULL_CC" \
        -DCMAKE_CXX_COMPILER="$MULL_CXX" \
        -DCMAKE_C_FLAGS="-fpass-plugin=$FRONTEND -g -grecord-command-line" \
        -DCMAKE_CXX_FLAGS="-fpass-plugin=$FRONTEND -g -grecord-command-line" \
        -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$BUILD/bin" \
        -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$BUILD/bin" \
        -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY="$BUILD" >/dev/null
fi

# The mull.yml config carries the mutator set and pins the run to the five
# files. It also stops the plugin from instrumenting the test and the rest of
# the SDK, which keeps each mutant run at the suite's real speed.
CONFIG="$BUILD/mull.yml"
{
    printf 'mutators:\n'
    for m in "${MUTATORS[@]}"; do
        printf '  - %s\n' "$m"
    done
    printf 'includePaths:\n'
    for f in "${FILES[@]}"; do
        printf '  - %s\n' "$(basename "$f" | sed 's/\./\\./g')"
    done
} > "$CONFIG"

say "building"
cmake --build "$BUILD" --target test --parallel >/dev/null

TEST_BIN="$BUILD/bin/test"
[ -x "$TEST_BIN" ] || fail "build produced no $TEST_BIN" 2

report_file="$BUILD/mull-report.txt"
say "running mutants against $TEST_BIN"
set +e
MULL_CONFIG="$CONFIG" "$MULL_RUNNER" \
    --reporters IDE \
    --ide-reporter-show-killed \
    --timeout 60000 \
    "$TEST_BIN" \
    > "$report_file" 2>&1
mull_status=$?
set -e

# The IDE reporter prints one line per mutant:
#   /path/file.cpp:line:col: warning: (Survived|Killed): ...
# Reduce it to per-file counts. A file with no report line is absent from the
# run (no mutants generated); it still gets a zero row below.
declare -A killed survived
for f in "${FILES[@]}"; do
    killed["$f"]=0
    survived["$f"]=0
done

while IFS= read -r line; do
    case "$line" in
        *": warning: Survived:"*) verdict=survived ;;
        *": warning: Killed:"*)   verdict=killed ;;
        *)                        continue ;;
    esac
    path="${line%%:[0-9]*}"
    base="sdk/$(basename "$path")"
    if [[ " ${FILES[*]} " == *" $base "* ]]; then
        if [ "$verdict" = survived ]; then
            survived["$base"]=$(( survived["$base"] + 1 ))
        else
            killed["$base"]=$(( killed["$base"] + 1 ))
        fi
    fi
done < "$report_file"

# Compare against the floor. The floor holds a per-file surviving count; a
# file absent from floor.tsv has no floor yet and establishes it this run.
declare -A floor
if [ -f "$FLOOR" ]; then
    while IFS=$'\t' read -r file _ _ survived_floor; do
        [ "$file" = "file" ] && continue
        floor["$file"]="${survived_floor:-0}"
    done < "$FLOOR"
fi

exceeded=0
for f in "${FILES[@]}"; do
    if [ -n "${floor[$f]+x}" ] && [ "${survived[$f]}" -gt "${floor[$f]}" ]; then
        say "survivors grew in $f: ${survived[$f]} > floor ${floor[$f]}"
        exceeded=1
    fi
done

# Write the floor: the surviving count per file, this run's numbers. A run
# with fewer survivors shrinks the floor for the same pull request.
{
    printf 'file\tkilled\tsurvived\n'
    for f in "${FILES[@]}"; do
        printf '%s\t%s\t%s\n' "$f" "${killed[$f]}" "${survived[$f]}"
    done
} > "$FLOOR"

say "per-file killed/survived counts:"
for f in "${FILES[@]}"; do
    printf '  %-32s killed=%s survived=%s\n' "$f" "${killed[$f]}" "${survived[$f]}"
done
say "wrote $FLOOR"

if [ "$mull_status" -ne 0 ]; then
    fail "mull-runner exited $mull_status (see $report_file)" "$mull_status"
fi

if [ "$exceeded" -eq 1 ]; then
    fail "a mutant survived that the floor had killed: the suite regressed" 1
fi

say "all survivors are within the floor"
exit 0
