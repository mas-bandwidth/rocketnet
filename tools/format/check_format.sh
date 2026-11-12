#!/bin/sh
# check_format.sh: run clang-format 18 (--dry-run -Werror) over rocketnet
# sources and report unformatted files. With file arguments it checks only
# those files; with none it checks every tracked C/C++ source except the
# vendored trees (sodium/, tests/xdp_host/linux/, tests/xdp_host/bpf/), the
# checker's own fixtures (tools/format/testdata/, whose dirty.c is unformatted
# on purpose) and the generated files named in GENERATED below.
#
# A run that checks nothing is a failure: git failing to list (exit 2), a
# listing with no source in it, or zero files left to check (exit 1). A file
# whose first 20 lines carry a "Generated" (or "@generated",
# "auto-generated") comment and is not in GENERATED fails: an exemption is
# listed here with its reason, never claimed by a comment.
# Self-test: tools/format/test_check_format.sh
set -eu
set -f

# generated files, exempt from formatting, one per line
#   sdk/rocketnet_test_vectors.h: written by the vectors harness that produced
#     the standard's appendix; never edited by hand
GENERATED="sdk/rocketnet_test_vectors.h"

CLANG_FORMAT="${CLANG_FORMAT:-clang-format-18}"
if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
    echo "check_format.sh: clang-format-18 not found" >&2
    exit 2
fi

if [ $# -eq 0 ]; then
    if ! tracked="$(git ls-files -- '*.c' '*.cc' '*.cpp' '*.h' '*.hpp' '*.inc')"; then
        echo "check_format.sh: git ls-files failed; nothing was checked" >&2
        exit 2
    fi
    if [ -z "$tracked" ]; then
        echo "check_format.sh: git listed no C or C++ source; nothing was checked" >&2
        exit 1
    fi
    list="$(printf '%s\n' "$tracked" \
        | grep -vE '^(sodium|tests/xdp_host/linux|tests/xdp_host/bpf|tools/format/testdata)/' || true)"
    unlisted=""
    IFS='
'
    for f in $list; do
        if sed -n '1,20p' "$f" | grep -qiE '^[[:space:]]*(//|/\*|\*)[[:space:]]*(@?generated|auto-generated)'; then
            if printf '%s\n' "$GENERATED" | grep -qxF "$f"; then
                continue
            fi
            unlisted="$unlisted$f
"
        fi
        set -- "$@" "$f"
    done
    unset IFS
    if [ -n "$unlisted" ]; then
        printf '%s' "$unlisted" | sed 's/$/: says it is generated but is not in GENERATED (tools\/format\/check_format.sh)/'
        exit 1
    fi
    if [ $# -eq 0 ]; then
        echo "check_format.sh: no file left to check after the exclusions" >&2
        exit 1
    fi
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
