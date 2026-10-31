#!/usr/bin/env bash
# check_make_jobs.sh: every build in the tree carries a bounded job count.
# A bare `-j` (or a bare `cmake --build --parallel`, which cmake turns into a
# bare `-j` for make and ignores CMAKE_BUILD_PARALLEL_LEVEL for) has no job
# limit and starts every compile at once, which exhausts memory (exit 137) and
# starves the other runners on a shared fleet host.
#
#   1. Makefiles: every child make carries a bounded -j. The Makefile bounds
#      its child make at JOBS: CMAKE_BUILD_PARALLEL_LEVEL when set, else the
#      processors capped at 8, overridden by `make JOBS=<n>` or the caller's -j.
#   2. Workflows and the shell tools they run: no bare `--parallel`, no bare
#      `-j`, no `-j"$(nproc)"`. Leave the flag off and the cap applies.
#   3. Every workflow that builds declares CMAKE_BUILD_PARALLEL_LEVEL in its
#      top-level env, the one cap every cmake --build and make in it reads.
set -u

bad=0

for f in Makefile benchmarks/Makefile; do
    [ -f "$f" ] || continue
    lineno=0
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno + 1))
        case "$line" in
            *'$(MAKE)'*|*'${MAKE}'*|*'make '*) ;;
            *) continue ;;
        esac
        if [[ "$line" =~ (^|[[:space:]])-j([[:space:]]|$) ]]; then
            echo "$f:$lineno: unbounded -j in a child make: $line"
            bad=1
        fi
    done < "$f"
done

unbounded_parallel='--parallel([[:space:]]*$|[[:space:]]+[^0-9"$[:space:]]|["'"'"'])'
unbounded_j='(^|[[:space:]])(make|ninja)[[:space:]]+(.*[[:space:]])?-j([[:space:]]|$|"?\$\((nproc|getconf))'
for f in .github/workflows/*.yml tools/*.sh scripts/*; do
    [ -f "$f" ] || continue
    lineno=0
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno + 1))
        [[ "$line" =~ ^[[:space:]]*# ]] && continue
        if [[ "$line" =~ cmake[[:space:]].*--build ]] && [[ "$line" =~ $unbounded_parallel ]]; then
            echo "$f:$lineno: unbounded cmake --build --parallel: $line"
            bad=1
        fi
        if [[ "$line" =~ $unbounded_j ]]; then
            echo "$f:$lineno: unbounded -j: $line"
            bad=1
        fi
    done < "$f"
done

for f in .github/workflows/*.yml; do
    if grep -Eq '(cmake[[:space:]].*--build|(run:|&&|^[[:space:]]+)[[:space:]]*make([[:space:]]|$))' "$f" &&
       ! awk '/^env:/ { e = 1; next } /^[^[:space:]#]/ { e = 0 } e && /^  CMAKE_BUILD_PARALLEL_LEVEL:/ { found = 1 } END { exit !found }' "$f"; then
        echo "$f: builds without a top-level env CMAKE_BUILD_PARALLEL_LEVEL"
        bad=1
    fi
done

if [ "$bad" -eq 0 ]; then
    echo "ok: every build has a bounded job count"
fi
exit "$bad"
