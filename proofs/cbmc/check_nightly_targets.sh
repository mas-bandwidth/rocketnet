#!/usr/bin/env bash
# check_nightly_targets.sh [--self-test] [--expect FILE] [--readers FILE --partitions FILE]
#
# The nightly reader list is a wildcard over */proof.mk: a renamed directory or
# a typo in `NIGHTLY +=` dropped a reader from the nightly with nothing red
# (VERIFY-6 A21). This requires the printed reader and partition lists to equal
# NIGHTLY-TARGETS exactly, in either direction. With no list files it asks make.
#
# Prints `NIGHTLY-TARGETS OK <n> readers <m> partitions`, or one
# `NIGHTLY-TARGETS <kind> missing|unexpected <target>` line per difference and
# exits 1. Used by nightly-proofs.yml's plan job and ci.yml's lint job.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# want <kind> <expect file>: the expected targets of one kind, sorted
want() {
    awk -v k="$1" '$1==k {print $2}' "$2" | sort
}

# compare <kind> <expect file> <printed file>
compare() {
    local kind="$1" expect="$2" printed="$3" bad=0 t
    for t in $(comm -23 <(want "$kind" "$expect") <(sort "$printed")); do
        echo "NIGHTLY-TARGETS $kind missing $t (in $(basename "$expect"), not printed by make)"
        bad=1
    done
    for t in $(comm -13 <(want "$kind" "$expect") <(sort "$printed")); do
        echo "NIGHTLY-TARGETS $kind unexpected $t (printed by make, not in $(basename "$expect"))"
        bad=1
    done
    return "$bad"
}

check() {
    local expect="$1" readers="$2" partitions="$3" rc=0 n m
    [ -f "$expect" ] || { echo "NIGHTLY-TARGETS $expect not found"; return 1; }
    n="$(want reader "$expect" | wc -l | tr -d ' ')"
    m="$(want partition "$expect" | wc -l | tr -d ' ')"
    if [ "$n" -eq 0 ] || [ "$m" -eq 0 ]; then
        echo "NIGHTLY-TARGETS $expect lists $n readers and $m partitions; both must be nonzero"
        return 1
    fi
    compare reader "$expect" "$readers" || rc=1
    compare partition "$expect" "$partitions" || rc=1
    if [ "$rc" -eq 0 ]; then
        echo "NIGHTLY-TARGETS OK $n readers $m partitions"
    fi
    return "$rc"
}

self_test() {
    local dir rc=0
    dir="$(mktemp -d)"
    trap 'rm -rf "$dir"' RETURN
    printf 'reader r1\nreader r2\npartition p1\npartition p2\n' > "$dir/expect"
    printf 'r1\nr2\n' > "$dir/r_ok"
    printf 'r1\n' > "$dir/r_drop"
    printf 'r1\nr2\nr3\n' > "$dir/r_extra"
    printf 'p2\np1\n' > "$dir/p_ok"
    printf 'p1\n' > "$dir/p_drop"
    : > "$dir/empty"
    printf 'partition p1\n' > "$dir/no_readers"
    # case name, expected exit, expect file, readers, partitions, line the output must hold
    local cases=(
        "equal|0|expect|r_ok|p_ok|NIGHTLY-TARGETS OK 2 readers 2 partitions"
        "reader dropped|1|expect|r_drop|p_ok|NIGHTLY-TARGETS reader missing r2"
        "reader added|1|expect|r_extra|p_ok|NIGHTLY-TARGETS reader unexpected r3"
        "partition dropped|1|expect|r_ok|p_drop|NIGHTLY-TARGETS partition missing p2"
        "nothing printed|1|expect|empty|empty|NIGHTLY-TARGETS reader missing r1"
        "no readers expected|1|no_readers|empty|p_drop|lists 0 readers"
        "no expect file|1|absent|r_ok|p_ok|not found"
    )
    local c name code exp r p line out got
    for c in "${cases[@]}"; do
        IFS='|' read -r name code exp r p line <<< "$c"
        set +e
        out="$(check "$dir/$exp" "$dir/$r" "$dir/$p" 2>&1)"
        got=$?
        set -e
        if [ "$got" != "$code" ] || ! grep -qF -- "$line" <<< "$out"; then
            echo "self-test FAIL: $name: exit $got (want $code), output: $out"
            rc=1
        fi
    done
    if [ "$rc" -eq 0 ]; then
        echo "check_nightly_targets self-test: ${#cases[@]} cases ok"
    fi
    return "$rc"
}

expect="$here/NIGHTLY-TARGETS"
readers=""
partitions=""
while [ $# -gt 0 ]; do
    case "$1" in
        --self-test) self_test; exit $? ;;
        --expect) expect="$2"; shift 2 ;;
        --readers) readers="$2"; shift 2 ;;
        --partitions) partitions="$2"; shift 2 ;;
        *) echo "usage: $0 [--self-test] [--expect FILE] [--readers FILE --partitions FILE]"; exit 2 ;;
    esac
done

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
if [ -z "$readers" ]; then
    make -s -C "$here" print-nightly-readers > "$tmp/readers"
    readers="$tmp/readers"
fi
if [ -z "$partitions" ]; then
    make -s -C "$here" print-replay-near-partitions > "$tmp/partitions"
    partitions="$tmp/partitions"
fi
check "$expect" "$readers" "$partitions"
