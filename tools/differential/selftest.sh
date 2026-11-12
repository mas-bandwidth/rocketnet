#!/usr/bin/env bash
# Self-test of the differential gate (tools/differential/run.sh): it must pass
# on the tree as it is and fail on each planted fault.
#
# Usage:
#   bash tools/differential/selftest.sh [NETCODE_DIR]
#
# The tracked files of the repository are copied to a scratch directory and
# every run happens there, so nothing in the checkout changes. First the
# stock copy must print "differential: PASS" and exit 0. Then each plant is
# applied to the copy alone, the gate is run, and it must exit nonzero having
# printed "differential: FAIL" (a build failure is not a pass for a plant);
# the file is restored before the next plant. The plants:
#
#   protocol-rung    the server's protocol id refusal removed
#                    (sdk/rocketnet_server_protocol.cpp, `if ( 0 && ...`):
#                    wrong_protocol_id and structured_flip_protocol_id are
#                    admitted, rocketnet-weaker
#   version-rung     the version refusal removed: netcode cannot see it (no
#                    comparable row carries a bad version that is not also
#                    refused by the signature), the expected rocketnet
#                    decision of the raw seed_type_client_version_invalid row
#                    catches it
#   timeout-rung     the timeout refusal removed: the permitted-stricter
#                    timeout_out_of_range row turns agree, which the expected
#                    rocketnet column catches
#   reference-row    cases.tsv's netcode column for valid_signature set to
#                    refuse:planted: in reference mode a rocketnet-weaker row,
#                    live a live/reference disagreement
#   missing-row      one corpus_cases.tsv row deleted: a corpus file with no
#                    reference row
#
# Exit status: 0 when the stock run passes and every plant fails the gate;
# 1 otherwise, naming the plant.

set -uo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
NETCODE_DIR="${1:-}"
if [ -n "$NETCODE_DIR" ]; then
    NETCODE_DIR="$(cd "$NETCODE_DIR" && pwd)"
fi

SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/differential-selftest.XXXXXX")"
trap 'rm -rf "$SCRATCH"' EXIT

( cd "$REPO_DIR" && git ls-files -z | tar --null -T - -cf - ) | tar -xf - -C "$SCRATCH" || {
    echo "selftest: could not copy the tracked files" >&2
    exit 1
}

export DIFFERENTIAL_BUILD_DIR="$SCRATCH/build-differential"
red=0
TAB=$'\t'

# gate <name> <want: pass|fail>
gate() {
    local name="$1" want="$2" log="$SCRATCH/gate-$1.log" rc=0
    bash "$SCRATCH/tools/differential/run.sh" ${NETCODE_DIR:+"$NETCODE_DIR"} > "$log" 2>&1 || rc=$?
    local last
    last="$(grep -E '^differential: (PASS|FAIL)' "$log" | tail -n 1)"
    if [ "$want" = pass ]; then
        if [ "$rc" -eq 0 ] && [ "${last#differential: PASS}" != "$last" ]; then
            echo "ok   $name: exit 0, $last"
            return
        fi
    else
        if [ "$rc" -ne 0 ] && [ "${last#differential: FAIL}" != "$last" ]; then
            echo "ok   $name: exit $rc, $last"
            grep -E '^FAIL' "$log" | head -n 3 | sed 's/^/       /'
            return
        fi
    fi
    echo "FAIL $name: wanted $want, got exit $rc, last line '${last:-none}'"
    tail -n 20 "$log"
    red=$((red + 1))
}

# plant <name> <file> <sed expression>: apply, check the file changed, run
# the gate wanting a failure, restore
plant() {
    local name="$1" file="$SCRATCH/$2" expr="$3"
    cp "$file" "$file.orig"
    sed -e "$expr" "$file.orig" > "$file"
    if cmp -s "$file" "$file.orig"; then
        echo "FAIL $name: the plant did not apply to $2"
        red=$((red + 1))
    else
        gate "$name" fail
    fi
    mv "$file.orig" "$file"
}

gate stock pass

plant protocol-rung sdk/rocketnet_server_protocol.cpp \
    '/if ( rocketnet_read_uint64( &p ) != server->protocol_id )/s/if ( /if ( 0 \&\& /'
plant version-rung sdk/rocketnet_server_protocol.cpp \
    '/if ( rocketnet_read_uint32( &p ) != ROCKETNET_WIRE_VERSION )/s/if ( /if ( 0 \&\& /'
plant timeout-rung sdk/rocketnet_server_protocol.cpp \
    '/if ( timeout_seconds <= 0 )/s/if ( /if ( 0 \&\& /'
plant reference-row tools/differential/cases.tsv \
    "s/^\(valid_signature${TAB}[^${TAB}]*${TAB}\)accept${TAB}/\1refuse:planted${TAB}/"
plant missing-row tools/differential/corpus_cases.tsv \
    "/^seed_type_client_ipv4${TAB}/d"

if [ "$red" -ne 0 ]; then
    echo "differential selftest: FAIL ($red)"
    exit 1
fi
echo "differential selftest: PASS"
