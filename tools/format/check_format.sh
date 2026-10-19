#!/bin/sh
# check_format.sh: run clang-format 18 (--dry-run -Werror) over rocketnet
# sources and report unformatted files. With file arguments it checks only
# those files; with none it checks every tracked C/C++ source except the
# vendored trees (sodium/, test/xdp_host/linux/, test/xdp_host/bpf/) and
# generated files. Generated files, named here, are recognized by their first
# 20 lines carrying the word "generated": none exist in the tree today, so the
# skip list is empty and kept for when a generator lands.
set -eu

CLANG_FORMAT="${CLANG_FORMAT:-clang-format-18}"
if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
    echo "check_format.sh: clang-format-18 not found" >&2
    exit 2
fi

if [ $# -gt 0 ]; then
    files="$*"
else
    files="$(git ls-files | grep -E '\.(c|cc|cpp|h|hpp|inc)$' \
        | grep -vE '^(sodium|test/xdp_host/linux|test/xdp_host/bpf)/')"
    filtered=""
    for f in $files; do
        if sed -n '1,20p' "$f" | grep -qi 'generated'; then
            continue
        fi
        filtered="$filtered $f"
    done
    files="$filtered"
fi

bad=0
total=0
for f in $files; do
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
