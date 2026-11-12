#!/bin/sh
# mrdocs_control.sh: the undocumented-symbol gate's positive control.
#
# `make docs-undocumented` passes when it prints no row, and it cannot tell
# "MrDocs found nothing" from "MrDocs was told not to look" (a warn-* key in
# mrdocs.yml turned off) or from a log whose wording changed (VERIFY-3 C-13).
# So this runs the same MrDocs, with the same mrdocs.yml, mrdocs.sh and
# undocumented.sh, over a scratch copy of the tree whose public header holds
# one undocumented export, and requires exactly that one row and the gate's
# refusal. Run it after `make docs`, which fetches the pinned MrDocs.
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PLANT='ROCKETNET_EXPORT int rocketnet_planted_undocumented( int value );'
ANCHOR='ROCKETNET_EXPORT void rocketnet_log_function( rocketnet_log_function_t function, void * context );'

# the MrDocs `make docs` used, found as tools/docs/mrdocs.sh finds it, so the
# scratch copy never downloads its own
MRDOCS_BIN="${MRDOCS:-}"
if [ -z "$MRDOCS_BIN" ]; then
    if command -v mrdocs > /dev/null 2>&1; then
        MRDOCS_BIN="$(command -v mrdocs)"
    fi
fi
if [ -z "$MRDOCS_BIN" ]; then
    MRDOCS_BIN="$ROOT/build/mrdocs-2026.9.29/bin/mrdocs"
fi
if [ ! -x "$MRDOCS_BIN" ] || ! "$MRDOCS_BIN" --version 2>/dev/null | grep -q "2026.9.29"; then
    echo "mrdocs_control: FAIL: no MrDocs 2026.9.29 at $MRDOCS_BIN (run make docs first)" >&2
    exit 1
fi

SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT
mkdir -p "$SCRATCH/tools" "$SCRATCH/docs"
cp -R "$ROOT/sdk" "$SCRATCH/sdk"
cp -R "$ROOT/tools/docs" "$SCRATCH/tools/docs"
cp -R "$ROOT/docs/mrdocs" "$SCRATCH/docs/mrdocs"
cp "$ROOT/mrdocs.yml" "$SCRATCH/mrdocs.yml"

if [ "$(grep -cxF "$ANCHOR" "$SCRATCH/sdk/rocketnet.h")" != 1 ]; then
    echo "mrdocs_control: FAIL: the anchor line is not in sdk/rocketnet.h exactly once: $ANCHOR" >&2
    exit 1
fi
awk -v anchor="$ANCHOR" -v plant="$PLANT" '{ print } $0 == anchor { print ""; print plant }' \
    "$ROOT/sdk/rocketnet.h" > "$SCRATCH/sdk/rocketnet.h"

if ! MRDOCS="$MRDOCS_BIN" sh "$SCRATCH/tools/docs/mrdocs.sh" > "$SCRATCH/mrdocs.out" 2>&1; then
    tail -20 "$SCRATCH/mrdocs.out" >&2
    echo "mrdocs_control: FAIL: MrDocs did not run on the planted copy" >&2
    exit 1
fi

set +e
rows="$(sh "$SCRATCH/tools/docs/undocumented.sh" "$SCRATCH/build/docs/mrdocs.log" 2>"$SCRATCH/undocumented.err")"
status=$?
# grep -c prints 0 and exits 1 on no match: the count is the verdict here
count="$(printf '%s\n' "$rows" | grep -c .)"
planted="$(printf '%s\n' "$rows" | grep -c "	rocketnet_planted_undocumented	function	")"
set -e
printf '%s\n' "$rows"
if [ "$planted" != 1 ] || [ "$count" != 1 ] || [ "$status" -eq 0 ]; then
    cat "$SCRATCH/undocumented.err" >&2
    echo "mrdocs_control: FAIL: on a header with one undocumented export the gate printed $count row(s)," \
         "$planted of them the planted function, and exited $status; it must print exactly that row and refuse:" \
         "check mrdocs.yml's warn-* keys and the phrases tools/docs/undocumented.sh reads" >&2
    exit 1
fi
echo "mrdocs_control: the planted undocumented export is the one row, and the gate refuses it (exit $status)"
