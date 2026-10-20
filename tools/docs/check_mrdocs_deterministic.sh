#!/bin/sh
# check_mrdocs_deterministic.sh: build the MrDocs reference from a clean
# build/docs several times on the same tree (default 2, or the count given as
# the one argument) and compare each run with the first, byte for byte: the
# undocumented report (tools/docs/undocumented.sh) and the generated symbol
# list (the page names under build/docs/html, each with a checksum of its
# content). Exit 0 when every run matches, nonzero with the diff when one does
# not.
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

RUNS="${1:-2}"
case "$RUNS" in
    ''|*[!0-9]*)
        echo "error: the run count must be a number: $RUNS" >&2
        exit 2
        ;;
esac
if [ "$RUNS" -lt 2 ]; then
    echo "error: the run count must be at least 2: $RUNS" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM

run=1
while [ "$run" -le "$RUNS" ]; do
    rm -rf "$ROOT/build/docs"
    if ! sh "$ROOT/tools/docs/mrdocs.sh" >"$WORK/build.$run.log" 2>&1; then
        cat "$WORK/build.$run.log" >&2
        echo "FAIL: docs build $run of $RUNS failed" >&2
        exit 1
    fi
    sh "$ROOT/tools/docs/undocumented.sh" "$ROOT/build/docs/mrdocs.log" >"$WORK/undocumented.$run.tsv"
    # the symbol list: every generated page, with a checksum of its content,
    # so a page whose links or members differ between runs shows too
    (cd "$ROOT/build/docs/html" && find . -type f -name "*.html" | LC_ALL=C sort | while IFS= read -r page; do cksum "$page"; done) >"$WORK/symbols.$run.txt"
    if [ "$run" -gt 1 ]; then
        status=0
        diff -u "$WORK/undocumented.1.tsv" "$WORK/undocumented.$run.tsv" >"$WORK/diff" 2>&1 || status=1
        diff -u "$WORK/symbols.1.txt" "$WORK/symbols.$run.txt" >>"$WORK/diff" 2>&1 || status=1
        if [ "$status" -ne 0 ]; then
            cat "$WORK/diff"
            echo "FAIL: docs run $run of $RUNS differs from run 1 on the same tree" >&2
            exit 1
        fi
    fi
    run=$((run + 1))
done

echo "ok: $RUNS docs builds gave the same undocumented report ($(wc -l <"$WORK/undocumented.1.tsv") lines) and symbol list ($(wc -l <"$WORK/symbols.1.txt") pages)"
