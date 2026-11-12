#!/usr/bin/env bash
# The cross-endian interoperability run: tools/bigendian/interop.c over real
# sockets between a big-endian guest and this little-endian host, both ways.
#
#   1. a big-endian server in the guest, little-endian clients here
#   2. a little-endian server here, big-endian clients in the guest
#
# The guest is the s390x guest of .github/workflows/nightly-bigendian-system.yml:
# reachable over ssh at 127.0.0.1:<ssh port> as runner with <key>, its source
# tree at ~/src with bin/bigendian_interop built, and QEMU user networking
# forwarding udp 127.0.0.1:<guest udp port> on this host to the same port in
# the guest. From the guest this host is 10.0.2.2 (QEMU user networking).
#
# Usage: tools/bigendian/run_interop.sh <host bigendian_interop> <ssh port> <key> <guest udp port> <host udp port> <clients>
#
# Prints INTEROP_BE_SERVER_LE_CLIENTS:<PASS|FAIL> and
# INTEROP_LE_SERVER_BE_CLIENTS:<PASS|FAIL>, each side's log, and exits 0 only
# when both passed. Every wait is bounded: the program's own deadline, and a
# kill of a server whose clients have finished and that has not exited 120
# seconds later.

set -uo pipefail

if [ "$#" -ne 6 ]; then
    echo "usage: $0 <host bigendian_interop> <ssh port> <key> <guest udp port> <host udp port> <clients>"
    exit 2
fi

host_binary="$1"
ssh_port="$2"
ssh_key="$3"
guest_port="$4"
host_port="$5"
clients="$6"

SSH=(ssh -i "$ssh_key" -p "$ssh_port"
    -o StrictHostKeyChecking=no
    -o UserKnownHostsFile=/dev/null
    -o IdentitiesOnly=yes
    -o ConnectTimeout=5
    -o LogLevel=ERROR
    runner@127.0.0.1)

work=$(mktemp -d)

# the server's public key from its log, once it has printed it; empty if the
# server exits first or does not print it within 600 seconds
server_key() {
    local log="$1" pid="$2" i=0 key=""
    while [ "$i" -lt 600 ]; do
        key=$(sed -n 's/^server public key \([0-9a-f]*\)$/\1/p' "$log")
        if [ -n "$key" ]; then
            echo "$key"
            return 0
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            return 1
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

# wait for a server for at most 120 seconds after its clients finished
wait_server() {
    local pid="$1" i=0
    while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 120 ]; do
        sleep 1
        i=$((i + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        echo "the server did not exit 120 seconds after its clients finished"
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        return 1
    fi
    wait "$pid"
}

run() {
    local label="$1" server_log="$2" client_log="$3"
    shift 3
    local verdict=FAIL key=""
    if key=$(server_key "$server_log" "$server_pid"); then
        if "$@" "$key" "$clients" > "$client_log" 2>&1; then
            if wait_server "$server_pid"; then
                verdict=PASS
            fi
        else
            wait_server "$server_pid"
        fi
    else
        wait_server "$server_pid"
    fi
    echo "--- $label: server"
    cat "$server_log"
    echo "--- $label: clients"
    cat "$client_log" 2>/dev/null
    echo "$label:$verdict"
    [ "$verdict" = PASS ]
}

rc=0

"${SSH[@]}" "cd ~/src && exec ./bin/bigendian_interop server 127.0.0.1:${guest_port} ${clients}" > "$work/be_server.log" 2>&1 &
server_pid=$!
run INTEROP_BE_SERVER_LE_CLIENTS "$work/be_server.log" "$work/le_clients.log" \
    "$host_binary" client "127.0.0.1:${guest_port}" || rc=1

"$host_binary" server "10.0.2.2:${host_port}" "$clients" > "$work/le_server.log" 2>&1 &
server_pid=$!
run INTEROP_LE_SERVER_BE_CLIENTS "$work/le_server.log" "$work/be_clients.log" \
    "${SSH[@]}" "cd ~/src && exec ./bin/bigendian_interop client 10.0.2.2:${host_port}" || rc=1

rm -rf "$work"
exit "$rc"
