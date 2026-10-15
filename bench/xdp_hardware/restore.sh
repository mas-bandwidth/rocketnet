#!/bin/bash
#
# restore.sh -- put the dedicated XDP test machine back, unconditionally.
#
# RocketNet. Copyright 2026 Más Bandwidth LLC.
# Licensed under the GNU Affero General Public License v3.0
#
# The model is tla/XdpHardwareBench.tla: this script is Restore(r) (called
# from hold.sh's EXIT trap, which INT and TERM also reach) and
# RecoverRestore (called with --recover by the next hold when the lock file
# names a run that died without its trap). In the model a restore clears
# everything any run attached and returns the baseline; FreeMeansRestored
# holds only because the lock is released after this script passes. Here
# the same claim is checked rather than assumed: the last step diffs the
# host snapshot against the one hold.sh took, and the row
# restore_leaves_the_baseline fails on any difference. A failed restore
# leaves $XDPHW_STATE/restore-failed, and hold.sh refuses every later run
# until a person has read it (the model's abstraction: there, restore
# always succeeds; here, a restore that does not is held for a person).
#
# usage: restore.sh <machine.conf> [--recover]
#
# Steps, in order, each idempotent:
#   1. stop the per-run receiver and sender this run started (the pids
#      file names each with its process start time, so a recycled pid is
#      never signalled)
#   2. detach every XDP program on the receiver's and the sender's
#      interface (the run's verify passed only with none attached, R3)
#   3. the sockets close with their processes; confirm no AF_XDP socket
#      is left on either interface
#   4. rocketnet-provision sweep, for any pin root the run left
#   5. rocketnet-provision <iface> <queues> verify
#   6. snapshot and diff against the baseline

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"

[ $# -ge 1 ] || { echo "usage: restore.sh <machine.conf> [--recover]" >&2; exit 2; }
CONF="$1"
RECOVER="${2:-}"
# shellcheck disable=SC1090
. "$CONF"

: "${XDPHW_IFACE:?machine conf must set XDPHW_IFACE}"
: "${XDPHW_QUEUES:?machine conf must set XDPHW_QUEUES}"
XDPHW_STATE="${XDPHW_STATE:-/run/rocketnet-xdp-hw}"
XDPHW_RANGE="${XDPHW_RANGE:-1-15}"
XDPHW_PROVISION="${XDPHW_PROVISION:-$HERE/../../scripts/rocketnet-provision}"
XDPHW_SENDER_EXEC="${XDPHW_SENDER_EXEC:-ip netns exec ${XDPHW_SENDER_NETNS:-rnxdp-sender}}"

FAILED="$XDPHW_STATE/restore-failed"
failures=()

say()
{
    echo "restore: $*"
}

process_start_time()
{
    local stat
    stat="$(cat "/proc/$1/stat" 2>/dev/null)" || return 1
    stat="${stat##*) }"
    # shellcheck disable=SC2086
    set -- $stat
    echo "${20}"
}

# 1. stop the per-run processes, newest first (sender, receiver, then the
#    run body), TERM then KILL after five seconds

if [ -s "$XDPHW_STATE/pids" ]; then
    tac "$XDPHW_STATE/pids" | while read -r pid start name; do
        [ -n "$pid" ] || continue
        now="$(process_start_time "$pid" || true)"
        if [ -z "$now" ]; then
            say "$name (pid $pid) already exited"
            continue
        fi
        if [ "$now" != "$start" ]; then
            say "pid $pid is no longer $name (start time $now, recorded $start); not signalled"
            continue
        fi
        kill -TERM "$pid" 2>/dev/null
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            [ "$(process_start_time "$pid" || true)" = "$start" ] || break
            sleep 0.5
        done
        if [ "$(process_start_time "$pid" || true)" = "$start" ]; then
            kill -KILL "$pid" 2>/dev/null
            say "$name (pid $pid) killed after TERM was ignored"
        else
            say "$name (pid $pid) stopped"
        fi
    done
fi

# 2. detach. The library detaches its own program on destroy; this is for
#    a receiver that died before destroy.

detach()
{
    local exec_prefix="$1" iface="$2"
    if $exec_prefix ip -d link show dev "$iface" 2>/dev/null | grep -qE 'prog/xdp|xdpgeneric|xdpdrv|xdpoffload'; then
        say "an XDP program is still attached to $iface; detaching"
        $exec_prefix xdp-loader unload -a "$iface" 2>&1 | sed 's/^/restore:   /'
        if $exec_prefix ip -d link show dev "$iface" 2>/dev/null | grep -qE 'prog/xdp|xdpgeneric|xdpdrv'; then
            $exec_prefix ip link set dev "$iface" xdpgeneric off 2>/dev/null
            $exec_prefix ip link set dev "$iface" xdpdrv off 2>/dev/null
            $exec_prefix ip link set dev "$iface" xdp off 2>/dev/null
        fi
        if $exec_prefix ip -d link show dev "$iface" 2>/dev/null | grep -qE 'prog/xdp|xdpgeneric|xdpdrv'; then
            failures+=("an XDP program is still attached to $iface")
        fi
    fi
}

detach "" "$XDPHW_IFACE"
if [ -n "${XDPHW_SENDER_IFACE:-}" ]; then
    detach "$XDPHW_SENDER_EXEC" "$XDPHW_SENDER_IFACE"
fi

# 3. sockets: every AF_XDP socket the run opened belonged to a process
#    stopped above. ss lists any that remain (iproute2 with --xdp).

if ss --help 2>&1 | grep -q -- '--xdp'; then
    left="$(ss -a --xdp 2>/dev/null | tail -n +2 | wc -l)"
    if [ "$left" != "0" ]; then
        failures+=("$left AF_XDP socket(s) remain open on this host")
        ss -a --xdp 2>&1 | sed 's/^/restore:   /'
    fi
else
    say "this ss has no --xdp; the socket check rests on step 1's process stops"
fi

# 4. sweep crash-leftover pin roots (the script judges staleness by name)

"$XDPHW_PROVISION" sweep 2>&1 | sed 's/^/restore:   /'

# 5. verify again

if "$XDPHW_PROVISION" "$XDPHW_IFACE" "$XDPHW_QUEUES" verify "$XDPHW_RANGE" > "$XDPHW_STATE/verify-after.txt" 2>&1; then
    say "rocketnet-provision $XDPHW_IFACE $XDPHW_QUEUES verify passes after restore"
else
    failures+=("rocketnet-provision verify fails after restore (see $XDPHW_STATE/verify-after.txt)")
    sed 's/^/restore:   /' "$XDPHW_STATE/verify-after.txt"
fi

# 6. snapshot and diff

if [ -s "$XDPHW_STATE/baseline.snap" ]; then
    "$HERE/hold.sh" "$CONF" --snapshot "$XDPHW_STATE/after.snap"
    if diff -u "$XDPHW_STATE/baseline.snap" "$XDPHW_STATE/after.snap" > "$XDPHW_STATE/restore.diff"; then
        say "host snapshot identical to the baseline"
    else
        failures+=("host snapshot differs from the baseline (see $XDPHW_STATE/restore.diff)")
        sed 's/^/restore:   /' "$XDPHW_STATE/restore.diff"
    fi
else
    # tmpfs state does not survive a reboot, and neither does an attached
    # program or a bpffs pin: after a reboot there is no baseline to diff,
    # and verify (step 5) is the whole check
    say "no baseline snapshot (the state directory is new, or the machine rebooted); verify is the check"
fi

if [ ${#failures[@]} -eq 0 ]; then
    echo "ROW restore_leaves_the_baseline PASS diff=empty verify=pass${RECOVER:+ mode=recover}"
    rm -f "$XDPHW_STATE/pids"
    exit 0
fi

{
    echo "restore failed at $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    for f in "${failures[@]}"; do
        echo "  $f"
    done
} > "$FAILED"
for f in "${failures[@]}"; do
    say "FAILED: $f"
done
echo "ROW restore_leaves_the_baseline FAIL ${#failures[@]} difference(s); $FAILED holds the list${RECOVER:+ mode=recover}"
exit 1
