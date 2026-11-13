#!/usr/bin/env bash
# check_tool_versions.sh [--self-test] [--pins FILE] <tool>=<KEY>...
#
# Each tool must be on PATH and print, in the first line of `<tool> --version`,
# exactly the version proofs/cbmc/TOOL-VERSIONS pins for KEY: a version word
# equal to the pin, never a substring of a longer one (16.6.0 is not 6.6.0).
# A key the file does not pin, or a missing file, is refused: an empty pin
# matched every version before (VERIFY-3 C-15).
#
# Prints `<tool>: <first line> (pinned <version>)` per tool, or
# `TOOL-MISSING <tool> <pin> (<why>) on <runner>` and exits 1.
# Used by cbmc.yml and nightly-proofs.yml.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
pins="$here/TOOL-VERSIONS"
runner="${RUNNER_NAME:-$(hostname)}"

check() {
    local tool="$1" key="$2" want got word
    [ -f "$pins" ] || { echo "TOOL-MISSING $tool ($pins not found) on $runner"; return 1; }
    want="$(awk -v k="$key" '$1==k {print $2}' "$pins")"
    [ -n "$want" ] || { echo "TOOL-MISSING $tool (no pin for $key in $pins) on $runner"; return 1; }
    if ! command -v "$tool" > /dev/null 2>&1; then
        echo "TOOL-MISSING $tool $want (missing) on $runner"
        return 1
    fi
    got="$("$tool" --version 2>&1 | head -1)"
    for word in $(printf '%s\n' "$got" | grep -oE '[0-9]+(\.[0-9]+)+' || true); do
        if [ "$word" = "$want" ]; then
            echo "$tool: $got (pinned $want)"
            return 0
        fi
    done
    echo "TOOL-MISSING $tool $want (got: $got) on $runner"
    return 1
}

self_test() {
    local dir failed=0 out
    dir="$(mktemp -d)"
    trap 'rm -rf "$dir"' RETURN
    mkdir "$dir/bin"
    printf 'CBMC 6.6.0\nCADICAL 2.1.3\nESBMC 8.5.0\n' > "$dir/pins"
    stub() { printf '#!/bin/sh\necho "%s"\n' "$2" > "$dir/bin/$1"; chmod +x "$dir/bin/$1"; }
    expect() {
        local want_rc="$1" label="$2" rc=0
        shift 2
        out="$(PATH="$dir/bin:/usr/bin:/bin" "$0" --pins "$dir/pins" "$@" 2>&1)" || rc=$?
        if [ "$rc" -ne "$want_rc" ]; then
            echo "self-test FAIL: $label: exit $rc, want $want_rc: $out"
            failed=1
        fi
    }
    stub cbmc "6.6.0 (cbmc-6.6.0)"
    stub cadical "2.1.3"
    stub esbmc "ESBMC version 8.5.0 64-bit x86_64 linux"
    expect 0 "the pinned versions" cbmc=CBMC cadical=CADICAL esbmc=ESBMC
    expect 1 "a key the file does not pin" cbmc=CBMCX
    stub cbmc "16.6.0 (cbmc-16.6.0)"
    expect 1 "a version holding the pin as a substring" cbmc=CBMC
    stub cbmc "6.6.01"
    expect 1 "a version extending the pin" cbmc=CBMC
    stub cbmc "5.95.1 (cbmc-5.95.1)"
    expect 1 "another version" cbmc=CBMC
    expect 1 "an absent tool" rocketnet-no-such-checker=CBMC
    stub cbmc "6.6.0 (cbmc-6.6.0)"
    rm "$dir/pins"
    expect 1 "no pin file" cbmc=CBMC
    expect 2 "no tool named"
    [ "$failed" -eq 0 ] || return 1
    echo "check_tool_versions self-test: 8 cases ok"
}

if [ "${1:-}" = "--self-test" ]; then
    self_test
    exit $?
fi
if [ "${1:-}" = "--pins" ]; then
    pins="$2"
    shift 2
fi
if [ "$#" -eq 0 ]; then
    echo "usage: check_tool_versions.sh [--self-test] [--pins FILE] <tool>=<KEY>..." >&2
    exit 2
fi
for pair in "$@"; do
    check "${pair%%=*}" "${pair#*=}" || exit 1
done
