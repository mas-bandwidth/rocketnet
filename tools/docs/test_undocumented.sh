#!/bin/sh
# test_undocumented.sh: verify that undocumented.sh classifies warnings by
# first-matching row in docs/mrdocs/areas.tsv (pinning
# rocketnet_message_client_required_memory -> configuration,
# rocketnet_frame_sync_stats_t::dropped_frames -> counters, file rows, and
# unassigned) and supports --area filtering.
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT="$ROOT/tools/docs/undocumented.sh"
FIXTURE="$ROOT/tools/docs/testdata/mrdocs.log"
AREAS="$ROOT/docs/mrdocs/areas.tsv"

output="$(sh "$SCRIPT" "$FIXTURE" "$AREAS")"

expected="$(cat << 'EXPECTED'
sdk/rocketnet.h	340	rocketnet_init	function	lifecycle
sdk/rocketnet.h	1363	rocketnet_frame_sync_stats_t	record	counters
sdk/rocketnet.h	1366	rocketnet_frame_sync_stats_t::dropped_frames	variable	counters
sdk/rocketnet.h	1476	ROCKETNET_UNRELIABLE	macro	reliability
sdk/rocketnet.h	1770	rocketnet_message_client_required_memory	function	configuration
sdk/rocketnet.h	1814	rocketnet_message_client_send	function	messages
sdk/rocketnet.hpp	34	rocketnet::ClientTransport::update	function	cpp
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

cfg_only="$(sh "$SCRIPT" --area configuration "$FIXTURE" "$AREAS")"
if [ "$cfg_only" != "sdk/rocketnet.h	1770	rocketnet_message_client_required_memory	function	configuration" ]; then
    echo "FAIL: --area configuration did not isolate rocketnet_message_client_required_memory" >&2
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

# MrDocs cannot attach a block comment to each X-macro expansion. Keep a
# source-level pin so a newly appended counter cannot silently lack its public
# name, meaning, unit, class, and family documentation.
python3 - "$ROOT/sdk/rocketnet_counters.h" <<'PY'
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text()
def undocumented(text):
    block = text.split("#define ROCKETNET_COUNTER_ROWS( X )", 1)[1].split("\nenum\n{", 1)[0]
    missing = []
    previous = ""
    for line in block.splitlines():
        row = re.match(r"\s*X\(\s*([A-Z0-9_]+)\s*,", line)
        if row:
            name = row.group(1)
            expected = "@brief ROCKETNET_COUNTER_" + name + ":"
            if not (expected in previous and "Unit: occurrences." in previous
                    and "Class:" in previous and "family:" in previous
                    and previous.strip().endswith("*/ \\")):
                missing.append(name)
        if line.strip():
            previous = line
    return missing

assert not undocumented(source), "undocumented counter rows: " + ", ".join(undocumented(source))
needle = "    X( PACKETS_RECEIVED, ARRIVAL, NONE ) \\\n"
assert needle in source, "counter-row mutation anchor changed"
mutated = source.replace(needle, needle + "    X( NEW_UNDOCUMENTED_ROW, REFUSAL, NONE ) \\\n", 1)
assert undocumented(mutated) == ["NEW_UNDOCUMENTED_ROW"], "new undocumented row stayed green"
print("ok: every counter row has a named meaning/unit comment and a planted row is rejected")
PY
