#!/usr/bin/env bash
# Self-test of tools/bigendian/run_byte_cases.sh: each planted violation must
# exit nonzero, the clean fixture must exit 0. Fixtures only: a fake unit
# binary (a shell script answering --list and --run) and a fake tree, in a
# temporary directory. Run by the lint job of ci.yml.

set -euo pipefail

script="$(cd "$(dirname "$0")" && pwd)/run_byte_cases.sh"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

failures=0
cases=0

# make_tree <dir> <vector registration line> <doc vectors> <doc filter>
make_tree() {
    local dir="$1"
    mkdir -p "$dir/tests" "$dir/docs"
    printf '%s\n' "$2" 'REGISTER_UNIT_TEST( test_standard_vector_two );' > "$dir/tests/test_unit_standard_a.cpp"
    printf '%s\n' 'REGISTER_UNIT_TEST( test_connect_token_vector );' > "$dir/tests/test_unit_connect_token_vectors.cpp"
    printf '%s\n' 'REGISTER_UNIT_TEST( test_xdp_filter_host_drops_short );' \
        'REGISTER_UNIT_TEST( test_xdp_transport_queue_default_matches_memory_plan );' > "$dir/tests/test_unit_xdp_filter_host.cpp"
    printf '| The standard'"'"'s byte vectors, %s cases, and %s XDP filter cases (`tools/bigendian/run_byte_cases.sh`) | x |\n' "$3" "$4" > "$dir/docs/PLATFORMS.md"
    # the fake binary: lists every name in its LIST file, fails the names in FAIL
    printf '%s\n' '#!/usr/bin/env bash' \
        'here="$(cd "$(dirname "$0")" && pwd)"' \
        'if [ "$1" = "--list" ]; then cat "$here/LIST"; exit 0; fi' \
        'if [ "$1" = "--run" ]; then if grep -qxF "$2" "$here/FAIL"; then echo "check failed: $2"; exit 1; fi; echo "$2"; exit 0; fi' \
        'exit 2' > "$dir/fake_test"
    chmod +x "$dir/fake_test"
    printf '%s\n' test_standard_vector_one test_standard_vector_two test_connect_token_vector test_xdp_filter_host_drops_short > "$dir/LIST"
    : > "$dir/FAIL"
}

# expect <want: 0 or 1> <name> <dir> <args...>
expect() {
    local want="$1" name="$2" dir="$3"
    shift 3
    cases=$((cases + 1))
    local got=0
    (cd "$dir" && bash "$script" "$@") > "$work/out" 2>&1 || got=$?
    if { [ "$want" = 0 ] && [ "$got" = 0 ]; } || { [ "$want" != 0 ] && [ "$got" != 0 ]; }; then
        echo "ok   $name (exit $got)"
    else
        echo "FAIL $name: want exit $want, got $got"
        cat "$work/out"
        failures=$((failures + 1))
    fi
}

good='REGISTER_UNIT_TEST( test_standard_vector_one );'

make_tree "$work/clean" "$good" 3 1
expect 0 "clean tree runs every case" "$work/clean" ./fake_test
if ! grep -qx 'byte cases: 4 run, 0 failed (3 vector, 1 XDP filter)' "$work/out"; then
    echo "FAIL clean tree: summary line wrong"
    cat "$work/out"
    failures=$((failures + 1))
fi
expect 0 "clean tree counts" "$work/clean" --count

make_tree "$work/spelling" 'REGISTER_UNIT_CASE( test_standard_vector_one );' 2 1
expect 1 "a registration spelled another way" "$work/spelling" ./fake_test
expect 1 "a registration spelled another way (--count)" "$work/spelling" --count

make_tree "$work/novectors" '' 0 1
printf '' > "$work/novectors/tests/test_unit_standard_a.cpp"
printf '' > "$work/novectors/tests/test_unit_connect_token_vectors.cpp"
expect 1 "no vector case at all" "$work/novectors" ./fake_test

make_tree "$work/nofilter" "$good" 3 0
printf '%s\n' 'REGISTER_UNIT_TEST( test_xdp_transport_queue_default_matches_memory_plan );' > "$work/nofilter/tests/test_unit_xdp_filter_host.cpp"
expect 1 "no XDP filter case at all" "$work/nofilter" ./fake_test

make_tree "$work/doclow" "$good" 2 1
expect 1 "the document states fewer vectors than registered" "$work/doclow" --count

make_tree "$work/dochigh" "$good" 4 1
expect 1 "a vector case dropped below the documented count" "$work/dochigh" --count

make_tree "$work/nodoc" "$good" 3 1
printf 'nothing here\n' > "$work/nodoc/docs/PLATFORMS.md"
expect 1 "no document row" "$work/nodoc" --count

make_tree "$work/unbuilt" "$good" 3 1
printf '%s\n' test_standard_vector_one test_standard_vector_two test_connect_token_vector > "$work/unbuilt/LIST"
expect 1 "a named case not registered in the binary" "$work/unbuilt" ./fake_test

make_tree "$work/failing" "$good" 3 1
printf '%s\n' test_standard_vector_two > "$work/failing/FAIL"
expect 1 "a failing case" "$work/failing" ./fake_test

echo "run_byte_cases self-test: $cases cases, $failures failed"
if [ "$failures" != 0 ] || [ "$cases" = 0 ]; then
    exit 1
fi
