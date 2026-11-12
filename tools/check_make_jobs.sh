#!/usr/bin/env bash
# check_make_jobs.sh: every child make in the tree carries a bounded -j.
# A bare `-j` has no job limit and starts every compile at once, which can
# exhaust memory (exit 137) on the benches. The Makefile bounds its child make
# at JOBS (the processors capped at 8), overridden by `make JOBS=<n>` or by the
# caller's own -j. This walks the build files and fails on any child make that
# still carries a bare -j, and fails when it finds none of the build files.
#
#   bash tools/check_make_jobs.sh [ROOT]   (ROOT defaults to the current directory)
set -u

if [ $# -gt 0 ]; then
    cd "$1" || exit 1
fi

files="Makefile benchmarks/Makefile"

bad=0
checked=0
for f in $files; do
    [ -f "$f" ] || continue
    checked=$((checked + 1))
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

if [ "$checked" -eq 0 ]; then
    echo "FAIL: 0 files checked; none of $files is in $(pwd)"
    exit 1
fi
if [ "$bad" -eq 0 ]; then
    echo "ok: every child make has a bounded -j ($checked files)"
fi
exit "$bad"
