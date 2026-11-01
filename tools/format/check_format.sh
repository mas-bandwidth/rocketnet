#!/bin/sh
# check_format.sh: run clang-format 18 (--dry-run -Werror) over rocketnet
# sources and report unformatted files. With file arguments it checks only
# those files; with none it checks every tracked C/C++ source except the
# vendored trees (sodium/, tests/xdp_host/linux/, tests/xdp_host/bpf/), the
# checker's own fixtures (tools/format/testdata/, whose dirty.c is unformatted
# on purpose) and generated files. A generated file is one whose first 20 lines hold a comment
# line starting with "Generated" (or "@generated", "auto-generated"); today
# that is sdk/rocketnet_test_vectors.h.
set -eu
set -f

CLANG_FORMAT="${CLANG_FORMAT:-clang-format-18}"
if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
    echo "check_format.sh: clang-format-18 not found" >&2
    exit 2
fi

if [ $# -eq 0 ]; then
    list="$(git ls-files -- '*.c' '*.cc' '*.cpp' '*.h' '*.hpp' '*.inc' \
        | grep -vE '^(sodium|tests/xdp_host/linux|tests/xdp_host/bpf|tools/format/testdata)/' || true)"
    IFS='
'
    for f in $list; do
        if sed -n '1,20p' "$f" | grep -qiE '^[[:space:]]*(//|/\*|\*)[[:space:]]*(@?generated|auto-generated)'; then
            continue
        fi
        set -- "$@" "$f"
    done
    unset IFS
fi

# one clang-format per processor at once; the unformatted names come back
# sorted, so the report does not depend on which finished first
total=$#
bad=0
if [ "$total" -gt 0 ]; then
    jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
    unformatted="$(printf '%s\n' "$@" \
        | xargs -P "$jobs" -I {} sh -c '"$0" --dry-run -Werror --style=file "$1" >/dev/null 2>&1 || echo "$1"' "$CLANG_FORMAT" {} \
        | LC_ALL=C sort)"
    if [ -n "$unformatted" ]; then
        printf '%s\n' "$unformatted" | sed 's/$/: not formatted/'
        bad=$(printf '%s\n' "$unformatted" | wc -l | tr -d ' ')
    fi
fi

if [ "$bad" -eq 0 ]; then
    echo "ok: $total files formatted"
    exit 0
fi
echo "$bad of $total files not formatted"
exit 1
