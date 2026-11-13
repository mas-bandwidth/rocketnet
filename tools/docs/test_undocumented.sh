#!/bin/sh
# test_undocumented.sh: verify that undocumented.sh classifies warnings by
# first-matching row in docs/mrdocs/areas.tsv (pinning
# rocketnet_message_client_required_memory -> configuration,
# rocketnet_frame_sync_stats_t::dropped_frames -> counters, file rows, and
# unassigned), supports --area filtering, and refuses (exit 1, naming the
# count) a log with an undocumented ROCKETNET_EXPORT function.
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT="$ROOT/tools/docs/undocumented.sh"
FIXTURE="$ROOT/tools/docs/testdata/mrdocs.log"
AREAS="$ROOT/docs/mrdocs/areas.tsv"

TMP="$(mktemp -d)"
trap 'rm -rf -- "$TMP"' EXIT

# The fixture's five functions carry ROCKETNET_EXPORT, the header's macro, so
# the whole log is refused: it prints every row, then names the count.
rc=0
output="$(sh "$SCRIPT" "$FIXTURE" "$AREAS" 2>"$TMP/stderr")" || rc=$?
if [ "$rc" != 1 ]; then
    echo "FAIL: undocumented.sh exited $rc on five undocumented ROCKETNET_EXPORT functions, expected 1" >&2
    cat "$TMP/stderr" >&2
    exit 1
fi
if ! grep -qF "error: 5 undocumented public function(s)" "$TMP/stderr"; then
    echo "FAIL: undocumented.sh did not name the five undocumented public functions" >&2
    cat "$TMP/stderr" >&2
    exit 1
fi

expected="$(cat << 'EXPECTED'
sdk/rocketnet.h	340	rocketnet_init	function	lifecycle
sdk/rocketnet.h	1363	rocketnet_frame_sync_stats_t	record	counters
sdk/rocketnet.h	1366	rocketnet_frame_sync_stats_t::dropped_frames	variable	counters
sdk/rocketnet.h	1476	ROCKETNET_RELIABILITY_UNRELIABLE	macro	reliability
sdk/rocketnet.h	1770	rocketnet_message_client_required_memory	function	configuration
sdk/rocketnet.h	1814	rocketnet_message_client_send	function	messages
sdk/rocketnet_address.h	25	rocketnet_address_t	record	connection
sdk/rocketnet_connect_token.h	150	rocketnet_generate_connect_token	function	connection
sdk/rocketnet_counters.h	575	ROCKETNET_COUNTER_PACKETS_SENT	enum-value	counters
sdk/rocketnet_platform.h	20	rocketnet_platform_init	function	platform
sdk/rocketnet_route.h	18	rocketnet_route_ip	function	packets
sdk/rocketnet_unknown.h	10	mysterious_helper	function	unassigned
EXPECTED
)"

if [ "$output" != "$expected" ]; then
    echo "FAIL: undocumented.sh output does not match expected fixture output" >&2
    echo "--- expected ---" >&2
    printf "%s\n" "$expected" >&2
    echo "--- actual ---" >&2
    printf "%s\n" "$output" >&2
    exit 1
fi

rc=0
cfg_only="$(sh "$SCRIPT" --area configuration "$FIXTURE" "$AREAS" 2>/dev/null)" || rc=$?
if [ "$rc" != 1 ] || [ "$cfg_only" != "sdk/rocketnet.h	1770	rocketnet_message_client_required_memory	function	configuration" ]; then
    echo "FAIL: --area configuration did not isolate and refuse rocketnet_message_client_required_memory (exit $rc)" >&2
    exit 1
fi

# an area with no undocumented exported function passes
counters_only="$(sh "$SCRIPT" --area counters "$FIXTURE" "$AREAS")"
if [ "$(printf '%s\n' "$counters_only" | wc -l | tr -d ' ')" != 3 ]; then
    echo "FAIL: --area counters did not list its three rows" >&2
    exit 1
fi

unassigned_only="$(sh "$SCRIPT" --area unassigned "$FIXTURE" "$AREAS")"
if [ "$unassigned_only" != "sdk/rocketnet_unknown.h	10	mysterious_helper	function	unassigned" ]; then
    echo "FAIL: --area unassigned did not isolate mysterious_helper" >&2
    exit 1
fi

if sh "$SCRIPT" "$ROOT/tools/docs/testdata/does_not_exist.log" "$AREAS" >/dev/null 2>&1; then
    echo "FAIL: undocumented.sh succeeded on a missing log file" >&2
    exit 1
fi

echo "ok: undocumented.sh fixture checks passed"
