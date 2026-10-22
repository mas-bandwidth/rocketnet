#!/bin/sh
# check_format.sh: run clang-format 18 (--dry-run -Werror) over rocketnet
# sources and report unformatted files. With file arguments it checks only
# those files; with none it checks every tracked C/C++ source except the
# vendored trees (sodium/, tests/xdp_host/linux/, tests/xdp_host/bpf/) and
# generated files. A generated file is one whose first 20 lines hold a comment
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
        | grep -vE '^(sodium|tests/xdp_host/linux|tests/xdp_host/bpf)/' || true)"
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

bad=0
total=0
for f in "$@"; do
    total=$(( total + 1 ))
    if ! "$CLANG_FORMAT" --dry-run -Werror --style=file "$f" >/dev/null 2>&1; then
        echo "$f: not formatted"
        bad=$(( bad + 1 ))
    fi
done

if [ "$bad" -eq 0 ]; then
    echo "ok: $total files formatted"
    exit 0
fi
echo "$bad of $total files not formatted"
exit 1
