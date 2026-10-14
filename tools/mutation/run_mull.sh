#!/usr/bin/env bash
# Mutation gate for the parser, replay, ack, fragment and message core.
#
# What it is
# ----------
# A scheduled mutation run (RN-CORRECTNESS 2026-10-14, R4): the one
# hand-written mutant of tools/check_fuzz_oracle.py is generalised into a
# real LLVM-IR mutation pass over the five wire-facing files, driven by
# ./bin/test. Mull builds the library with its mutation plugin, runs the unit
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
# Mull is the tool the card names. If the bench carries another LLVM-IR
# mutation tool, point MULL_RUNNER at its entry point; the report must still
# carry one "file:line:col: warning: (Killed|Survived):" line per mutant.

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

# Locate Mull: mull-runner is the modern entry point, mull-cxx the legacy
# one. MULL_RUNNER overrides either, so a bench with a different build of the
# plugin can still drive this gate.
MULL_RUNNER="${MULL_RUNNER:-}"
if [ -z "$MULL_RUNNER" ]; then
    MULL_RUNNER="$(command -v mull-runner 2>/dev/null || true)"
fi
if [ -z "$MULL_RUNNER" ]; then
    MULL_RUNNER="$(command -v mull-cxx 2>/dev/null || true)"
fi

if [ -z "$MULL_RUNNER" ]; then
    say "Mull is absent: neither mull-runner nor mull-cxx is on PATH (nor is MULL_RUNNER set)." >&2
    say "The gate is not-done on this bench: no LLVM-IR mutation tool to run, so no floor is written and no green claim is made." >&2
    exit 3
fi

say "using $MULL_RUNNER"

# One instrumented build for the whole run: compile_commands.json is what
# Mull reads to know how each mutant is compiled. The tree's own flags (the
# strict -ffp-contract=off family floor) come from CMakeLists.txt.
if [ ! -f "$BUILD/compile_commands.json" ]; then
    say "configuring instrumented build at $BUILD"
    cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
fi
say "building"
cmake --build "$BUILD" --target test --parallel >/dev/null

TEST_BIN="$BUILD/bin/test"
[ -x "$TEST_BIN" ] || fail "build produced no $TEST_BIN" 2

# Restrict Mull to the five named files: each -include-path is a path prefix,
# and a full file path matches exactly that file and nothing else.
INCLUDE_ARGS=()
for f in "${FILES[@]}"; do
    INCLUDE_ARGS+=( -include-path "$ROOT/$f" )
done

report_file="$BUILD/mull-report.txt"
say "running mutants against $TEST_BIN"
set +e
"$MULL_RUNNER" \
    "$TEST_BIN" \
    -compdb-path "$BUILD/compile_commands.json" \
    -reporters IDE \
    -ide-reporter-show-killed \
    -mutators "$(IFS=,; printf '%s' "${MUTATORS[*]}")" \
    "${INCLUDE_ARGS[@]}" \
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
