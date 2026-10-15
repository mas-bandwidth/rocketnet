#!/bin/bash
#
# hold.sh -- the dedicated XDP test machine's lease: one run at a time.
#
# RocketNet. Copyright 2026 Más Bandwidth LLC.
# Licensed under the GNU Affero General Public License v3.0
#
# The model is tla/XdpHardwareBench.tla (cases in tla/CASES.tsv, group
# xdphardwarebench). Each step below names the model action it implements:
#
#   Hold(r)          flock -n on $XDPHW_STATE/lock; the lock file then names
#                    this run (pid, process start time, boot id, start UTC)
#   Refused(r)       flock -n fails: refuse, naming the holder, change nothing
#   Recover          the lock was free but the lock file still names a run:
#                    that run died without its trap (a crash), so restore.sh
#                    runs first, before this run takes the machine
#   RecoverRestore   restore.sh --recover; on success the lock file is empty
#   Crash(r)         the kernel drops the flock with the dead process; the
#                    lock file keeps naming it (MarkCrash in the model)
#   Finish/Restore   the EXIT, INT and TERM traps run restore.sh on every
#                    exit path; the lock is released only after it
#
# The reversed witnesses: MCXdpHardwareBenchBrokenTwoRuns.cfg (hold without
# the lock) fails OneRunAtATime; MCXdpHardwareBenchBrokenNoRestore.cfg (a
# crash that leaves no mark) fails FreeMeansRestored.
#
# usage:
#   hold.sh <machine.conf> -- <command> [args...]
#       hold the machine, snapshot the host, run the command with the lock
#       descriptor closed in it, restore on every exit path, release
#   hold.sh <machine.conf> --recover
#       restore after a dead run (also what the next hold does on its own)
#   hold.sh <machine.conf> --status
#       print the holder and the restore-failed mark, change nothing
#   hold.sh <machine.conf> --snapshot <out-file>
#       write the host snapshot restore.sh diffs; changes nothing
#   hold.sh <machine.conf> --clear-failed
#       remove the restore-failed mark after a person has read its diff
#       and put the machine back by hand
#
# The harness never provisions and never changes the boot tier: the machine
# is brought up once by RUNBOOK.md, and every run starts from
# `rocketnet-provision <iface> <queues> verify`.

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"

usage()
{
    sed -n '/^# usage:/,/^# The harness never/p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

[ $# -ge 2 ] || usage

CONF="$1"
shift
[ -r "$CONF" ] || { echo "hold: cannot read machine conf $CONF" >&2; exit 2; }
# shellcheck disable=SC1090
. "$CONF"

: "${XDPHW_IFACE:?machine conf must set XDPHW_IFACE}"
: "${XDPHW_QUEUES:?machine conf must set XDPHW_QUEUES}"
XDPHW_STATE="${XDPHW_STATE:-/run/rocketnet-xdp-hw}"
XDPHW_SENDER_EXEC="${XDPHW_SENDER_EXEC:-ip netns exec ${XDPHW_SENDER_NETNS:-rnxdp-sender}}"

LOCK="$XDPHW_STATE/lock"
FAILED="$XDPHW_STATE/restore-failed"

boot_id()
{
    cat /proc/sys/kernel/random/boot_id 2>/dev/null || echo unknown
}

# field 22 of /proc/<pid>/stat, read past the comm field's closing paren
process_start_time()
{
    local stat
    stat="$(cat "/proc/$1/stat" 2>/dev/null)" || return 1
    stat="${stat##*) }"
    # shellcheck disable=SC2086
    set -- $stat
    echo "${20}"
}

# the interface's IRQ lines: the last field of a /proc/interrupts line names
# the interface as a whole token (ixgbe "eth1-TxRx-0", i40e and ice
# "i40e-eth1-TxRx-0"). run.sh's irq_and_core_pinning row reads the same list.
nic_irqs()
{
    awk -v ifc="$1" '
        $1 ~ /^[0-9]+:$/ {
            name = $NF
            if ( name ~ ( "(^|[^A-Za-z0-9])" ifc "([^A-Za-z0-9]|$)" ) )
                print substr( $1, 1, length( $1 ) - 1 ), name
        }' /proc/interrupts
}

# the snapshot: everything a run may change on the host, in a stable order.
# counters and timestamps are left out on purpose; a difference here is a
# state difference, never traffic.
snapshot()
{
    local out="$1" iface="$XDPHW_IFACE" siface="${XDPHW_SENDER_IFACE:-}"
    {
        echo "## ip -d link show dev $iface"
        ip -d link show dev "$iface" 2>&1
        echo "## ethtool -l $iface"
        ethtool -l "$iface" 2>&1
        echo "## ethtool -k $iface"
        ethtool -k "$iface" 2>&1
        echo "## ethtool -x $iface"
        ethtool -x "$iface" 2>&1
        echo "## ethtool -g $iface"
        ethtool -g "$iface" 2>&1
        echo "## ethtool -c $iface"
        ethtool -c "$iface" 2>&1
        echo "## ethtool -n $iface"
        ethtool -n "$iface" 2>&1
        echo "## irq affinity for $iface"
        nic_irqs "$iface" | while read -r irq name; do
                echo "irq $irq $name $(cat "/proc/irq/$irq/smp_affinity_list" 2>/dev/null)"
            done
        echo "## busy-poll knobs"
        echo "napi_defer_hard_irqs $(cat "/sys/class/net/$iface/napi_defer_hard_irqs" 2>/dev/null)"
        echo "gro_flush_timeout $(cat "/sys/class/net/$iface/gro_flush_timeout" 2>/dev/null)"
        echo "## ip route / ip neigh (permanent) for $iface"
        ip route show dev "$iface" 2>&1
        ip neigh show dev "$iface" nud permanent 2>&1
        echo "## bpftool prog show (identity lines)"
        bpftool prog show 2>&1 | grep -E '^[0-9]+:' | sed -E 's/[[:space:]]+$//'
        echo "## bpftool net show"
        bpftool net show 2>&1
        echo "## ls /sys/fs/bpf"
        ls -1 /sys/fs/bpf 2>&1
        if [ -n "$siface" ]; then
            echo "## sender: ip -d link show dev $siface"
            $XDPHW_SENDER_EXEC ip -d link show dev "$siface" 2>&1
            echo "## sender: ethtool -l -k -x $siface"
            $XDPHW_SENDER_EXEC ethtool -l "$siface" 2>&1
            $XDPHW_SENDER_EXEC ethtool -k "$siface" 2>&1
            $XDPHW_SENDER_EXEC ethtool -x "$siface" 2>&1
            echo "## sender: ip addr / route for $siface"
            $XDPHW_SENDER_EXEC ip -4 addr show dev "$siface" 2>&1 | grep -E 'inet '
            $XDPHW_SENDER_EXEC ip route show dev "$siface" 2>&1
        fi
    } > "$out"
}

holder_line()
{
    if [ -s "$LOCK" ]; then
        cat "$LOCK"
    else
        echo "(no holder recorded)"
    fi
}

mode="${1:-}"

case "$mode" in
    --snapshot)
        [ $# -eq 2 ] || usage
        snapshot "$2"
        exit 0
        ;;
    --status)
        echo "holder: $(holder_line)"
        if [ -e "$FAILED" ]; then
            echo "restore-failed: $FAILED is present; the next hold refuses until --clear-failed"
        fi
        exit 0
        ;;
    --clear-failed)
        rm -f "$FAILED"
        echo "hold: restore-failed mark cleared"
        exit 0
        ;;
    --recover|--)
        ;;
    *)
        usage
        ;;
esac

[ "$(uname -s)" = "Linux" ] || { echo "hold: Linux only" >&2; exit 2; }
[ "$(id -u)" = "0" ] || { echo "hold: root only, and only on the dedicated machine" >&2; exit 2; }

mkdir -p "$XDPHW_STATE" || exit 2
chmod 700 "$XDPHW_STATE"

# Hold(r) / Refused(r): the exclusive lock, never a network lock. fd 9 stays
# open in this shell for the life of the hold and is closed in every child.

exec 9<>"$LOCK" || { echo "hold: cannot open $LOCK" >&2; exit 2; }
if ! flock -n 9; then
    echo "hold: REFUSED: the machine is held by $(holder_line); one test at a time" >&2
    exit 75
fi

if [ -e "$FAILED" ]; then
    echo "hold: REFUSED: an earlier restore left a difference ($FAILED); read it, put the machine back, then hold.sh $CONF --clear-failed" >&2
    exit 75
fi

# Recover / RecoverRestore: the lock was free, but the lock file still names
# a run. A clean release empties the file, so that run died without its trap.

if [ -s "$LOCK" ]; then
    echo "hold: the lock file names a run that ended without restore: $(holder_line)"
    echo "hold: restoring before this run takes the machine"
    if ! "$HERE/restore.sh" "$CONF" --recover 9>&-; then
        echo "hold: REFUSED: the recovery restore did not leave the machine verified; see $FAILED" >&2
        exit 75
    fi
fi

if [ "$mode" = "--recover" ]; then
    : > "$LOCK"
    echo "hold: recovered; the machine is free"
    exit 0
fi

shift   # past --
[ $# -ge 1 ] || usage

start_time="$(process_start_time $$)"
printf 'pid=%s start_time=%s boot_id=%s since=%s command=%s\n' \
    "$$" "$start_time" "$(boot_id)" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$(basename "$1")" > "$LOCK"

rm -f "$XDPHW_STATE/pids" "$XDPHW_STATE/baseline.snap"
snapshot "$XDPHW_STATE/baseline.snap"
echo "hold: held by pid $$; baseline snapshot $XDPHW_STATE/baseline.snap"

# Finish/Restore: restore runs on every exit path. INT and TERM become exits
# so the EXIT trap is the one place restore runs.

child=""
restored=0

on_exit()
{
    local status=$?
    if [ "$restored" = "0" ]; then
        restored=1
        if [ -n "$child" ] && kill -0 "$child" 2>/dev/null; then
            kill -TERM "$child" 2>/dev/null
            wait "$child" 2>/dev/null
        fi
        if "$HERE/restore.sh" "$CONF" 9>&-; then
            : > "$LOCK"
        else
            status=1
        fi
    fi
    exit "$status"
}

trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

"$@" 9>&- &
child=$!
printf '%s %s %s\n' "$child" "$(process_start_time "$child")" "$(basename "$1")" >> "$XDPHW_STATE/pids"
wait "$child"
exit $?
