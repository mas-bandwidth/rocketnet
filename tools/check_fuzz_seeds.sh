#!/bin/sh
# check_fuzz_seeds.sh: run every fuzz seed generator and hold the committed
# corpora to what the generators write (VERIFY-5 W3, W4).
#
#   sh tools/check_fuzz_seeds.sh [--bin DIR] [--root DIR] HARNESS...
#   sh tools/check_fuzz_seeds.sh --self-test
#
# For each harness, bin/fuzz_replay_<harness> (built by CMake) runs with
# ROCKETNET_FUZZ_SEED_DIR set, as ci.yml's seed step always ran it. Then:
#
#   - the generator must exit 0 and write at least one seed;
#   - every seed it writes must be committed, byte for byte, in the harness's
#     corpus (tests/fuzz/corpus<suffix>, suffix = harness name less
#     rocketnet_fuzz). A seed the corpus lacks or holds stale is DRIFT: the
#     nightly fuzzer and the per-commit replay start from the committed
#     corpus, so a seed only the generator knows is a branch neither reaches.
#     Committed inputs the generator does not write (fuzzer finds) are kept.
#   - the harness's withhold control runs: one named seed withheld, the
#     generator must exit 1 and print INCOMPLETE. A generator that exits 0
#     with a seed missing would pass every run above, so the control proves
#     the completeness check is live. WITHHOLD below names one seed per
#     generator that has a completeness check. token_private, round_trip and
#     config have none (they write their seeds without replaying them); the
#     drift check above is what holds their seeds.
#
# A seed that is platform-dependent by design is named in PLATFORM_SEEDS: it
# must be generated and committed, but its bytes are this platform's.
#
# A drifted corpus is fixed by regenerating it: the seeds are deterministic.
#   ROCKETNET_FUZZ_SEED_DIR=tests/fuzz/corpus<suffix> ROCKETNET_FUZZ_SEED_FROM=tests/fuzz/corpus \
#       ./bin/fuzz_replay_<harness> <empty file>
set -u

# harness:variable:seed. The variable is what that generator reads.
WITHHOLD="
rocketnet_fuzz:ROCKETNET_FUZZ_SEED_SKIP:prefilter_challenge
rocketnet_fuzz_message:ROCKETNET_FUZZ_SEED_SKIP:violation_bad_form
rocketnet_fuzz_connect_token:ROCKETNET_FUZZ_WITHHOLD:seed_type_client_expired
rocketnet_fuzz_receive_structured:ROCKETNET_FUZZ_WITHHOLD:seed_delivery_stream
rocketnet_fuzz_server_transport:ROCKETNET_FUZZ_WITHHOLD:raw_ack
rocketnet_fuzz_client_transport:ROCKETNET_FUZZ_WITHHOLD:raw_ack
rocketnet_fuzz_session:ROCKETNET_FUZZ_WITHHOLD:silence
rocketnet_fuzz_xdp_frame:ROCKETNET_FUZZ_WITHHOLD:ipv6_extension_fragment
rocketnet_fuzz_uring_recvmsg:ROCKETNET_FUZZ_WITHHOLD:gro_fault_cmsg_past_control
rocketnet_fuzz_gateway_netlink:ROCKETNET_FUZZ_WITHHOLD:neighbor_enoent
"

# harness:seed: the transports a server can name differ by platform
PLATFORM_SEEDS="
rocketnet_fuzz_config:server_transport_ladder_available
"

bin=bin
root=.

withhold_entry() { # harness -> "variable:seed" or nothing
    printf '%s\n' "$WITHHOLD" | while IFS=: read -r h v s; do
        if [ "$h" = "$1" ]; then printf '%s:%s\n' "$v" "$s"; fi
    done
}

check_harness() { # harness
    harness=$1
    corpus="$root/tests/fuzz/corpus${harness#rocketnet_fuzz}"
    exe="$bin/fuzz_replay_$harness"
    problems=0
    if [ ! -x "$exe" ]; then
        echo "FAIL $harness: $exe is not built"
        return 1
    fi
    if [ ! -d "$corpus" ]; then
        echo "FAIL $harness: no committed corpus at $corpus"
        return 1
    fi
    out="$scratch/$harness"
    mkdir -p "$out"
    if ! ( cd "$root" && ROCKETNET_FUZZ_SEED_DIR="$out" ROCKETNET_FUZZ_SEED_FROM=tests/fuzz/corpus "$exe" "$scratch/empty" ) > "$scratch/$harness.log" 2>&1; then
        echo "FAIL $harness: the seed generator failed"
        tail -5 "$scratch/$harness.log"
        return 1
    fi
    generated=$(cd "$out" && find . -type f | sed 's|^\./||' | LC_ALL=C sort)
    if [ -z "$generated" ]; then
        echo "FAIL $harness: the seed generator wrote nothing"
        return 1
    fi
    count=0
    for name in $generated; do
        count=$((count + 1))
        if [ ! -f "$corpus/$name" ]; then
            echo "DRIFT $harness: $name is generated but not committed in $corpus"
            problems=1
        elif printf '%s\n' "$PLATFORM_SEEDS" | grep -qx "$harness:$name"; then
            :
        elif ! cmp -s "$out/$name" "$corpus/$name"; then
            echo "DRIFT $harness: $name differs from the committed seed in $corpus"
            problems=1
        fi
    done
    entry=$(withhold_entry "$harness")
    control="no completeness check to withhold from"
    if [ -n "$entry" ]; then
        variable=${entry%%:*}
        seed=${entry#*:}
        mkdir -p "$out-withheld"
        status=0
        ( cd "$root" && env "$variable=$seed" ROCKETNET_FUZZ_SEED_DIR="$out-withheld" ROCKETNET_FUZZ_SEED_FROM=tests/fuzz/corpus "$exe" "$scratch/empty" ) > "$scratch/$harness-withheld.log" 2>&1 || status=$?
        if [ "$status" -ne 1 ] || ! grep -q INCOMPLETE "$scratch/$harness-withheld.log"; then
            echo "FAIL $harness: with $seed withheld ($variable) the generator exited $status, not 1 with INCOMPLETE"
            problems=1
        fi
        control="$seed withheld -> INCOMPLETE"
    fi
    if [ "$problems" -eq 0 ]; then
        echo "ok $harness: $count seed(s) generated, each committed as written; $control"
    fi
    return "$problems"
}

run() {
    scratch=$(mktemp -d)
    trap 'rm -rf "$scratch"' EXIT
    : > "$scratch/empty"
    failed=0
    for harness in "$@"; do
        check_harness "$harness" || failed=1
    done
    if [ "$failed" -ne 0 ]; then
        echo "check_fuzz_seeds: FAIL (regenerate a drifted corpus as this script's header says)"
    fi
    return "$failed"
}

# a fake generator for the self-test: writes seed_a and seed_b; with
# FAKE_WITHHOLD=seed_b set it skips seed_b and, unless FAKE_SILENT is set,
# exits 1 printing INCOMPLETE
fake_generator() { # path
    cat > "$1" <<'EOF'
#!/bin/sh
printf 'a' > "$ROCKETNET_FUZZ_SEED_DIR/seed_a"
if [ "${FAKE_WITHHOLD:-}" = seed_b ]; then
    if [ -n "${FAKE_SILENT:-}" ]; then exit 0; fi
    echo "seed generator: INCOMPLETE"; exit 1
fi
printf 'b' > "$ROCKETNET_FUZZ_SEED_DIR/seed_b"
printf '%s' "${FAKE_PLATFORM:-p}" > "$ROCKETNET_FUZZ_SEED_DIR/seed_p"
exit "${FAKE_STATUS:-0}"
EOF
    chmod +x "$1"
}

self_test() {
    tmp=$(mktemp -d)
    failures=0
    case_run() { # name want(0|1) want_text, then the environment is already set
        name=$1 want=$2 text=$3
        output=$(bin="$tmp/bin"; root="$tmp/root"; WITHHOLD="rocketnet_fuzz_fake:FAKE_WITHHOLD:seed_b"; PLATFORM_SEEDS="rocketnet_fuzz_fake:seed_p"; run rocketnet_fuzz_fake 2>&1)
        got=$?
        if [ "$got" -eq "$want" ] && printf '%s\n' "$output" | grep -q -- "$text"; then
            echo "ok: $name"
        else
            echo "WRONG: $name (exit $got, wanted $want with '$text'):"
            printf '%s\n' "$output" | sed 's/^/    /'
            failures=$((failures + 1))
        fi
    }
    reset() {
        rm -rf "$tmp/bin" "$tmp/root"
        mkdir -p "$tmp/bin" "$tmp/root/tests/fuzz/corpus_fake"
        fake_generator "$tmp/bin/fuzz_replay_rocketnet_fuzz_fake"
        printf 'a' > "$tmp/root/tests/fuzz/corpus_fake/seed_a"
        printf 'b' > "$tmp/root/tests/fuzz/corpus_fake/seed_b"
        printf 'p' > "$tmp/root/tests/fuzz/corpus_fake/seed_p"
        printf 'found by the fuzzer' > "$tmp/root/tests/fuzz/corpus_fake/crash_input"
    }
    reset
    case_run "green: committed as generated, fuzzer finds kept, control live" 0 "ok rocketnet_fuzz_fake: 3 seed"
    reset; export FAKE_PLATFORM=q
    case_run "green: a platform seed's bytes may differ" 0 "ok rocketnet_fuzz_fake: 3 seed"
    unset FAKE_PLATFORM
    reset; rm "$tmp/root/tests/fuzz/corpus_fake/seed_p"
    case_run "red: a platform seed must still be committed" 1 "seed_p is generated but not committed"
    reset; printf 'B' > "$tmp/root/tests/fuzz/corpus_fake/seed_b"
    case_run "red: a committed seed is stale" 1 "seed_b differs from the committed seed"
    reset; rm "$tmp/root/tests/fuzz/corpus_fake/seed_b"
    case_run "red: a generated seed is not committed" 1 "seed_b is generated but not committed"
    reset; export FAKE_SILENT=1
    case_run "red: the withhold control exits 0" 1 "exited 0, not 1 with INCOMPLETE"
    unset FAKE_SILENT
    reset; export FAKE_STATUS=1
    case_run "red: the generator fails" 1 "the seed generator failed"
    unset FAKE_STATUS
    reset; rm "$tmp/bin/fuzz_replay_rocketnet_fuzz_fake"
    case_run "red: the generator is not built" 1 "is not built"
    reset; rm -rf "$tmp/root/tests/fuzz/corpus_fake"
    case_run "red: the corpus is missing" 1 "no committed corpus"
    rm -rf "$tmp"
    if [ "$failures" -ne 0 ]; then
        echo "check_fuzz_seeds self-test: FAIL ($failures)"
        return 1
    fi
    echo "check_fuzz_seeds self-test: PASS"
    return 0
}

if [ "${1:-}" = "--self-test" ]; then
    self_test
    exit $?
fi
while [ $# -gt 0 ]; do
    case "$1" in
        --bin) bin=$2; shift 2 ;;
        --root) root=$2; shift 2 ;;
        *) break ;;
    esac
done
if [ $# -eq 0 ]; then
    echo "usage: sh tools/check_fuzz_seeds.sh [--bin DIR] [--root DIR] HARNESS... | --self-test" >&2
    exit 2
fi
run "$@"
