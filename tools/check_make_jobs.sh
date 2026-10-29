#!/usr/bin/env bash
# check_make_jobs.sh: every child make in the tree carries a bounded -j.
# A bare `-j` has no job limit and starts every compile at once, which can
# exhaust memory (exit 137) on the benches. The Makefile bounds its child make
# at JOBS (the processors capped at 8), overridden by `make JOBS=<n>` or by the
# caller's own -j. This walks the build files and fails on any child make that
# still carries a bare -j.
set -u

files="Makefile benchmarks/Makefile"

bad=0
for f in $files; do
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

if [ "$bad" -eq 0 ]; then
    echo "ok: every child make has a bounded -j"
fi
exit "$bad"
