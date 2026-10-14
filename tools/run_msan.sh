#!/usr/bin/env bash
# run_msan.sh: the one command for the MemorySanitizer leg. Builds the
# portable unit suite with clang -fsanitize=memory and origin tracking, runs
# ./bin/test under it, and reports "MSan: clean" when no uninitialised read
# appears.
#
# MSan needs every linked object instrumented. The default build uses the
# pinned two-file sodium vendor (sodium/sodium.c + sodium.h), which is part of
# this tree, so it is compiled by the same CMake target with the same flags;
# no system sodium is linked. The unit suite (./bin/test) opens no socket and
# starts no driver.
#
# Exit status: 0 when the run is clean or every finding is listed in
# tools/msan_known.txt; 1 when a finding is not listed there.
set -euo pipefail

cd "$(dirname "$0")/.."

build_dir=build-msan
flags="-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer"

# The unit suite defines its own global operator new/delete (test_channels.cpp)
# to arm the allocation-failure trap; the MSan C++ runtime ships replacements
# too. Allow the duplicate so the instrumented in-tree operator wins; malloc is
# still intercepted and poisoned by the MSan runtime, so shadow tracking holds.
link_flags="-Wl,--allow-multiple-definition"

# Give the MSan build its own output directory; the release build's bin/ must
# not be clobbered, or the gate could run a stale, non-instrumented binary.
abs_build="$PWD/$build_dir"
cmake -S . -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS="$flags" \
    -DCMAKE_CXX_FLAGS="$flags $link_flags" \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$abs_build/bin" \
    -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$abs_build/lib" \
    -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY="$abs_build/lib"

cmake --build "$build_dir" --target test --parallel

test_bin="$abs_build/bin/test"
log="$TMPDIR/msan.log"
known_file=tools/msan_known.txt

set +e
"$test_bin" >"$log" 2>&1
status=$?
set -e

if ! grep -q "WARNING: MemorySanitizer:" "$log"; then
    if [ "$status" -ne 0 ]; then
        echo "MSan: test run failed without a MemorySanitizer report"
        cat "$log"
        exit 1
    fi
    echo "MSan: clean"
    exit 0
fi

echo "MSan: findings (each with its origin trace):"
cat "$log"

root="$(pwd)/"
grep -E "^    #0 " "$log" \
    | sed -E 's/^[[:space:]]*#0[[:space:]]+0x[0-9a-f]+[[:space:]]+in[[:space:]]+([^ ]+)[[:space:]]+(.*)$/\1 \2/' \
    | sed "s|${root}||g" \
    | sed -E 's/ \(BuildId: [0-9a-f]+\)//' \
    | sed -E 's|\((build-msan/)?bin/test|(bin/test|' \
    | sort -u > "$TMPDIR/msan_findings.txt"

: > "$TMPDIR/msan_unmatched.txt"
while IFS= read -r finding; do
    [ -n "$finding" ] || continue
    grep -qF -- "$finding" "$known_file" || echo "$finding" >> "$TMPDIR/msan_unmatched.txt"
done < "$TMPDIR/msan_findings.txt"

if [ -s "$TMPDIR/msan_unmatched.txt" ]; then
    echo "MSan: findings not in $known_file:"
    cat "$TMPDIR/msan_unmatched.txt"
    echo "report each in $known_file"
    exit 1
fi

echo "MSan: findings listed in $known_file (all known)"
exit 0
