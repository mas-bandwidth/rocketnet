#!/usr/bin/env bash
# test_run_byte_cases.sh: run_byte_cases.sh against a fixture tree and a fake
# unit binary (a shell script answering --list and --run). Green: every
# named case registered and passing. Red: one failing case; one case named in
# the source and not registered; the registrations spelled otherwise (zero
# cases named, the class W1 of VERIFY-1); one case short of the floor.
# No build, no emulator; Linux sed (the runner's own sed expressions).
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
script="$root/tools/bigendian/run_byte_cases.sh"
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT

make_tree() {
    # $1: directory, $2: vector count, $3: filter count, $4: macro spelling
    local dir=$1 vectors=$2 filters=$3 macro=$4 i
    mkdir -p "$dir/tests"
    : > "$dir/tests/test_unit_standard_a.cpp"
    : > "$dir/tests/test_unit_connect_token_vectors.cpp"
    : > "$dir/tests/test_unit_xdp_filter_host.cpp"
    : > "$dir/registered.txt"
    for ((i = 0; i < vectors; i++)); do
        echo "$macro( test_vector_$i );" >> "$dir/tests/test_unit_standard_a.cpp"
        echo "test_vector_$i" >> "$dir/registered.txt"
    done
    for ((i = 0; i < filters; i++)); do
        echo "$macro( test_xdp_filter_host_$i );" >> "$dir/tests/test_unit_xdp_filter_host.cpp"
        echo "test_xdp_filter_host_$i" >> "$dir/registered.txt"
    done
    printf '%s\n' '#!/usr/bin/env bash' \
        'here=$(dirname "$0")' \
        'if [ "$1" = --list ]; then cat "$here/registered.txt"; exit 0; fi' \
        'if [ "$1" = --run ] && [ "$2" = "${FAIL_CASE:-}" ]; then echo "case failed"; exit 1; fi' \
        'exit 0' > "$dir/fake_test"
    chmod +x "$dir/fake_test"
}

expect() {
    # $1: label, $2: wanted exit (0 or nonzero), $3: directory, $4: line the output must hold
    local label=$1 want=$2 dir=$3 line=$4 rc=0
    (cd "$dir" && "$script" ./fake_test) > "$dir/out.txt" 2>&1 || rc=$?
    if { [ "$want" = 0 ] && [ "$rc" != 0 ]; } || { [ "$want" != 0 ] && [ "$rc" = 0 ]; }; then
        echo "test_run_byte_cases FAIL: $label: exit $rc"
        cat "$dir/out.txt"
        exit 1
    fi
    if ! grep -qF -- "$line" "$dir/out.txt"; then
        echo "test_run_byte_cases FAIL: $label: output lacks '$line'"
        cat "$dir/out.txt"
        exit 1
    fi
    echo "ok: $label (exit $rc)"
}

make_tree "$tmp/green" 58 30 REGISTER_UNIT_TEST
expect "every case registered and passing" 0 "$tmp/green" "byte cases: 88 run, 0 failed (58 vector, 30 XDP filter)"

make_tree "$tmp/failing" 58 30 REGISTER_UNIT_TEST
(export FAIL_CASE=test_vector_7; expect "one failing case" 1 "$tmp/failing" "check failed: test_vector_7")

make_tree "$tmp/unregistered" 58 30 REGISTER_UNIT_TEST
sed -i.bak '/^test_xdp_filter_host_3$/d' "$tmp/unregistered/registered.txt"
expect "a named case not registered" 1 "$tmp/unregistered" "test_xdp_filter_host_3 is named in the source but not registered"

make_tree "$tmp/zero" 58 30 RN_REGISTER_UNIT_TEST
expect "zero cases named (macro spelled otherwise)" 1 "$tmp/zero" "the sources name 0 vector and 0 XDP filter cases"

make_tree "$tmp/short" 57 30 REGISTER_UNIT_TEST
expect "one vector under the floor" 1 "$tmp/short" "the sources name 57 vector and 30 XDP filter cases"

echo "test_run_byte_cases: 5 cases passed"
