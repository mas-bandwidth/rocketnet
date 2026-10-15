#!/bin/bash
#
# run.sh -- one full pass of the XDP hardware tier on the dedicated machine.
#
# RocketNet. Copyright 2026 Más Bandwidth LLC.
# Licensed under the GNU Affero General Public License v3.0
#
# usage: run.sh <machine.conf>
#
# Root, Linux, the dedicated held machine only (RUNBOOK.md). run.sh holds
# the machine through hold.sh (tla/XdpHardwareBench.tla), so restore.sh runs
# on every exit path, and prints one ROW line per row:
#
#   preflight_verify                rocketnet-provision <iface> <queues> verify
#   native_attach_on_the_nic        the library attached in driver mode, never SKB
#   zero_copy_bind_per_queue        every queue's AF_XDP socket is zero copy
#   irq_and_core_pinning            queue IRQs on single isolated cores; workers on num_os_cpus + queue
#   throughput_by_hardware_counters the stepped sweep, judged by counters.py
#   latency_by_hardware_timestamps  one-way latency percentiles, labelled by clock source
#   restore_leaves_the_baseline     printed by restore.sh after the run
#
# The harness never provisions and never changes the boot tier. A failed
# preflight_verify ends the run with nothing attached.

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"

[ $# -ge 1 ] || { echo "usage: run.sh <machine.conf>" >&2; exit 2; }
CONF="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"

if [ "${XDPHW_HELD:-}" != "1" ]; then
    # hold the machine for the whole pass; the body below runs under the lock
    XDPHW_HELD=1 exec "$HERE/hold.sh" "$CONF" -- "$0" "$CONF"
fi

# shellcheck disable=SC1090
. "$CONF"

: "${XDPHW_IFACE:?}" "${XDPHW_QUEUES:?}" "${XDPHW_ADDRESS:?}" "${XDPHW_SENDER_IFACE:?}" "${XDPHW_SENDER_ADDRESS:?}"
XDPHW_STATE="${XDPHW_STATE:-/run/rocketnet-xdp-hw}"
XDPHW_RANGE="${XDPHW_RANGE:-1-15}"
XDPHW_PORT="${XDPHW_PORT:-40000}"
XDPHW_OS_CPUS="${XDPHW_OS_CPUS:-1}"
XDPHW_MAIN_CORE="${XDPHW_MAIN_CORE:-0}"
XDPHW_SENDER_NETNS="${XDPHW_SENDER_NETNS:-rnxdp-sender}"
XDPHW_SENDER_EXEC="${XDPHW_SENDER_EXEC:-ip netns exec $XDPHW_SENDER_NETNS}"
XDPHW_SENDER_QUEUES="${XDPHW_SENDER_QUEUES:-1}"
XDPHW_SENDER_CORES="${XDPHW_SENDER_CORES:?machine conf must name one sender core per sender queue, outside the receiver range}"
XDPHW_FLOWS="${XDPHW_FLOWS:-64}"
XDPHW_PAYLOAD_BYTES="${XDPHW_PAYLOAD_BYTES:-34}"
XDPHW_RATES="${XDPHW_RATES:-500000 1000000 2000000 4000000 6000000 8000000 10000000 12000000 14000000}"
XDPHW_WARMUP="${XDPHW_WARMUP:-30}"
XDPHW_WINDOWS="${XDPHW_WINDOWS:-5}"
XDPHW_WINDOW_SECONDS="${XDPHW_WINDOW_SECONDS:-60}"
XDPHW_TOL_ABS="${XDPHW_TOL_ABS:-32}"
XDPHW_TOL_REL="${XDPHW_TOL_REL:-0.000001}"
XDPHW_PROBE_COUNT="${XDPHW_PROBE_COUNT:-20000}"
XDPHW_PROBE_INTERVAL_US="${XDPHW_PROBE_INTERVAL_US:-500}"
XDPHW_PROVISION="${XDPHW_PROVISION:-$HERE/../../scripts/rocketnet-provision}"
XDPHW_OUT="${XDPHW_OUT:-$HERE/results/$(date -u +%Y%m%dT%H%M%SZ)}"

RECEIVER="$HERE/bin/xdp_hw_receiver"
SENDER="$HERE/bin/xdp_hw_sender"

mkdir -p "$XDPHW_OUT"
exec > >(tee -a "$XDPHW_OUT/run.log") 2>&1

rows_failed=0
row()
{
    # row <name> PASS|FAIL <detail>
    echo "ROW $1 $2 ${3:-}"
    echo "$1 $2" >> "$XDPHW_OUT/rows.txt"
    [ "$2" = "PASS" ] || rows_failed=$((rows_failed + 1))
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

record_pid()
{
    printf '%s %s %s\n' "$1" "$(process_start_time "$1")" "$2" >> "$XDPHW_STATE/pids"
}

echo "== run.sh $(date -u +%Y-%m-%dT%H:%M:%SZ) out=$XDPHW_OUT"
echo "kernel $(uname -r)"
ethtool -i "$XDPHW_IFACE" | sed 's/^/receiver nic: /'
$XDPHW_SENDER_EXEC ethtool -i "$XDPHW_SENDER_IFACE" | sed 's/^/sender nic: /'
( cd "$HERE" && git log --oneline -1 2>/dev/null | sed 's/^/head: /' )

for tool in ethtool bpftool xdp-loader ip python3 taskset; do
    command -v "$tool" > /dev/null || { echo "run: $tool is missing (RUNBOOK.md, toolchain)"; exit 2; }
done
[ -x "$RECEIVER" ] && [ -x "$SENDER" ] || { echo "run: build first: make -C $HERE"; exit 2; }

# ---------------------------------------------------------------- preflight_verify

if "$XDPHW_PROVISION" "$XDPHW_IFACE" "$XDPHW_QUEUES" verify "$XDPHW_RANGE" > "$XDPHW_OUT/verify-before.txt" 2>&1; then
    row preflight_verify PASS "rocketnet-provision $XDPHW_IFACE $XDPHW_QUEUES verify $XDPHW_RANGE"
else
    sed 's/^/verify: /' "$XDPHW_OUT/verify-before.txt"
    row preflight_verify FAIL "verify refused; this harness never provisions (RUNBOOK.md, bring-up)"
    exit 1
fi

# the sender side is part of the bring-up too: its interface is up, in its
# namespace, with its address

if ! $XDPHW_SENDER_EXEC ip -4 addr show dev "$XDPHW_SENDER_IFACE" 2>/dev/null | grep -q "inet $XDPHW_SENDER_ADDRESS/"; then
    row preflight_verify FAIL "the sender interface $XDPHW_SENDER_IFACE does not carry $XDPHW_SENDER_ADDRESS under '$XDPHW_SENDER_EXEC' (RUNBOOK.md, cabling)"
    exit 1
fi

# ---------------------------------------------------------------- the receiver

rx_in="$XDPHW_OUT/receiver.in"
rx_out="$XDPHW_OUT/receiver.out"
mkfifo "$rx_in"
taskset -c "$XDPHW_MAIN_CORE" "$RECEIVER" --address "$XDPHW_ADDRESS:$XDPHW_PORT" --queues "$XDPHW_QUEUES" --os-cpus "$XDPHW_OS_CPUS" \
    < "$rx_in" > "$rx_out" 2> "$XDPHW_OUT/receiver.err" &
rx_pid=$!
record_pid "$rx_pid" xdp_hw_receiver
exec 7> "$rx_in"

wait_for()
{
    # wait_for <file> <pattern> <seconds>: the last matching line
    local file="$1" pattern="$2" seconds="$3" i
    for ((i = 0; i < seconds * 10; i++)); do
        if grep -q -- "$pattern" "$file" 2>/dev/null; then
            grep -- "$pattern" "$file" | tail -1
            return 0
        fi
        sleep 0.1
    done
    return 1
}

rx_lines=0
rx_ask()
{
    # rx_ask <command> <answer-tag>: the receiver's answer line
    local before
    before="$(grep -c -- "^$2" "$rx_out" 2>/dev/null)"
    echo "$1" >&7
    local i
    for ((i = 0; i < 100; i++)); do
        if [ "$(grep -c -- "^$2" "$rx_out" 2>/dev/null)" -gt "$before" ]; then
            grep -- "^$2" "$rx_out" | tail -1
            return 0
        fi
        sleep 0.05
    done
    return 1
}

if ! ready="$(wait_for "$rx_out" '^READY\|^FAIL' 60)" || [ "${ready#READY}" = "$ready" ]; then
    cat "$XDPHW_OUT/receiver.err" "$rx_out" 2>/dev/null | tail -40 | sed 's/^/receiver: /'
    row native_attach_on_the_nic FAIL "the receiver did not start on the xdp driver"
    exit 1
fi
echo "receiver: $ready"

# ---------------------------------------------------------------- native_attach_on_the_nic

link="$(ip -d link show dev "$XDPHW_IFACE")"
loader="$(xdp-loader status "$XDPHW_IFACE" 2>&1)"
echo "$link" > "$XDPHW_OUT/ip-link-attached.txt"
echo "$loader" > "$XDPHW_OUT/xdp-loader-status.txt"
if grep -q 'falling back to skb mode' "$XDPHW_OUT/receiver.err" "$rx_out" 2>/dev/null; then
    row native_attach_on_the_nic FAIL "the library fell back to SKB mode (receiver log)"
elif echo "$link" | grep -q 'xdpgeneric'; then
    row native_attach_on_the_nic FAIL "ip -d link shows xdpgeneric (SKB mode)"
elif echo "$link" | grep -qE '(^|[[:space:]])xdp([[:space:]]|$)|prog/xdp' && echo "$loader" | grep -qiE 'native|driver'; then
    row native_attach_on_the_nic PASS "driver=xdp; $(echo "$loader" | grep -iE 'native|driver' | head -1 | tr -s ' ')"
else
    row native_attach_on_the_nic FAIL "no native-mode program on $XDPHW_IFACE (see xdp-loader-status.txt)"
fi

# ---------------------------------------------------------------- zero_copy_bind_per_queue

echo xsk >&7
wait_for "$rx_out" '^XSKEND' 10 > /dev/null
grep '^XSK ' "$rx_out" > "$XDPHW_OUT/xsk.txt"
ifindex="$(cat "/sys/class/net/$XDPHW_IFACE/ifindex")"
zc_ok=0
zc_bad=""
for ((q = 0; q < XDPHW_QUEUES; q++)); do
    line="$(grep "^XSK queue=$q ifindex=$ifindex " "$XDPHW_OUT/xsk.txt" | tail -1)"
    if echo "$line" | grep -q 'zerocopy=1'; then
        zc_ok=$((zc_ok + 1))
    else
        zc_bad="$zc_bad q$q:${line:-missing}"
    fi
done
if [ "$zc_ok" = "$XDPHW_QUEUES" ]; then
    row zero_copy_bind_per_queue PASS "$zc_ok of $XDPHW_QUEUES queues report XDP_OPTIONS_ZEROCOPY"
else
    row zero_copy_bind_per_queue FAIL "$zc_ok of $XDPHW_QUEUES zero copy;$zc_bad (copy mode is a failed row, never a pass)"
fi

# ---------------------------------------------------------------- irq_and_core_pinning

isolated="$(cat /sys/devices/system/cpu/isolated)"
in_list()
{
    # in_list <cpu> <cpulist like 1-7,9>
    local cpu="$1" part lo hi
    IFS=',' read -ra parts <<< "$2"
    for part in "${parts[@]}"; do
        lo="${part%-*}"; hi="${part#*-}"
        [ -n "$lo" ] || continue
        if [ "$cpu" -ge "$lo" ] && [ "$cpu" -le "$hi" ]; then return 0; fi
    done
    return 1
}
irq_bad=""
irq_cpus=""
irq_count=0
while read -r irq name; do
    aff="$(cat "/proc/irq/$irq/smp_affinity_list" 2>/dev/null)"
    irq_count=$((irq_count + 1))
    echo "irq $irq $name -> $aff" >> "$XDPHW_OUT/irqs.txt"
    if ! [[ "$aff" =~ ^[0-9]+$ ]]; then
        irq_bad="$irq_bad irq$irq($name)=$aff:not-single"
    elif ! in_list "$aff" "$isolated"; then
        irq_bad="$irq_bad irq$irq($name)=$aff:not-isolated"
    elif echo " $irq_cpus " | grep -q " $aff "; then
        irq_bad="$irq_bad irq$irq($name)=$aff:shared"
    fi
    irq_cpus="$irq_cpus $aff"
done < <(awk -v ifc="$XDPHW_IFACE" '$1 ~ /^[0-9]+:$/ { if ( $NF ~ ( "(^|[^A-Za-z0-9])" ifc "([^A-Za-z0-9]|$)" ) ) print substr( $1, 1, length( $1 ) - 1 ), $NF }' /proc/interrupts)

worker_bad=""
for ((q = 0; q < XDPHW_QUEUES; q++)); do
    want=$((XDPHW_OS_CPUS + q))
    hits=0
    for status in /proc/"$rx_pid"/task/*/status; do
        [ "$(awk '/^Cpus_allowed_list:/ { print $2 }' "$status")" = "$want" ] && hits=$((hits + 1))
    done
    echo "worker queue $q want cpu $want: $hits thread(s)" >> "$XDPHW_OUT/workers.txt"
    if [ "$hits" -lt 1 ]; then
        worker_bad="$worker_bad q$q:no-thread-on-cpu$want"
    fi
    in_list "$want" "$isolated" || worker_bad="$worker_bad q$q:cpu$want-not-isolated"
done
if [ "$irq_count" = "0" ]; then
    row irq_and_core_pinning FAIL "no /proc/interrupts line names $XDPHW_IFACE: this driver's irq names do not embed the interface (mlx5-class); pin and check with the driver's own tooling"
elif [ -z "$irq_bad$worker_bad" ]; then
    row irq_and_core_pinning PASS "$irq_count irqs on distinct isolated cores ($isolated); workers on $XDPHW_OS_CPUS..$((XDPHW_OS_CPUS + XDPHW_QUEUES - 1))"
else
    row irq_and_core_pinning FAIL "$irq_bad$worker_bad"
fi

# ---------------------------------------------------------------- the sender

tx_in="$XDPHW_OUT/sender.in"
tx_out="$XDPHW_OUT/sender.out"
mkfifo "$tx_in"
rx_mac="$(cat "/sys/class/net/$XDPHW_IFACE/address")"
$XDPHW_SENDER_EXEC "$SENDER" flood --iface "$XDPHW_SENDER_IFACE" --queues "$XDPHW_SENDER_QUEUES" --cores "$XDPHW_SENDER_CORES" \
    --src "$XDPHW_SENDER_ADDRESS" --dst "$XDPHW_ADDRESS:$XDPHW_PORT" --dst-mac "$rx_mac" \
    --flows "$XDPHW_FLOWS" --payload-bytes "$XDPHW_PAYLOAD_BYTES" \
    < "$tx_in" > "$tx_out" 2> "$XDPHW_OUT/sender.err" &
tx_pid=$!
record_pid "$tx_pid" xdp_hw_sender
exec 8> "$tx_in"
if ! wait_for "$tx_out" '^READY' 30 > /dev/null; then
    tail -20 "$tx_out" "$XDPHW_OUT/sender.err" | sed 's/^/sender: /'
    row throughput_by_hardware_counters FAIL "the sender did not start"
    exit 1
fi
grep '^TXMODE' "$tx_out" | sed 's/^/sender: /'

tx_ask()
{
    local before
    before="$(grep -c '^DONE' "$tx_out")"
    echo "send $1 $2" >&8
    while [ "$(grep -c '^DONE' "$tx_out")" -le "$before" ]; do
        kill -0 "$tx_pid" 2>/dev/null || return 1
        sleep 0.1
    done
    grep '^DONE' "$tx_out" | tail -1
}

# drained: two receiver snapshots 200 ms apart agree, so no frame is in
# flight between the NIC and the transport
drained_snap()
{
    local a b i
    a="$(rx_ask snap SNAP)"
    for ((i = 0; i < 50; i++)); do
        sleep 0.2
        b="$(rx_ask snap SNAP)"
        [ "$a" = "$b" ] && { echo "$b"; return 0; }
        a="$b"
    done
    echo "$b"
    return 1
}

# ---------------------------------------------------------------- throughput_by_hardware_counters

set -- $XDPHW_RATES
echo "warm-up: ${XDPHW_WARMUP}s at $1 pps, discarded (bench/PROTOCOL.md)"
tx_ask "$1" "$XDPHW_WARMUP" > /dev/null || { row throughput_by_hardware_counters FAIL "the sender died in warm-up"; exit 1; }
drained_snap > /dev/null

windows="$XDPHW_OUT/windows.jsonl"
: > "$windows"
for rate in $XDPHW_RATES; do
    step_clean=1
    for ((w = 1; w <= XDPHW_WINDOWS; w++)); do
        dir="$XDPHW_OUT/step-$rate/window-$w"
        mkdir -p "$dir"
        drained_snap > "$dir/snap-before.txt" || echo "warn: the receiver did not settle before the window"
        ethtool -S "$XDPHW_IFACE" > "$dir/rx-before.txt"
        $XDPHW_SENDER_EXEC ethtool -S "$XDPHW_SENDER_IFACE" > "$dir/tx-before.txt"
        tx_ask "$rate" "$XDPHW_WINDOW_SECONDS" > "$dir/done.txt" || { row throughput_by_hardware_counters FAIL "the sender died at $rate pps"; exit 1; }
        drained_snap > "$dir/snap-after.txt" || echo "warn: the receiver did not settle after the window"
        ethtool -S "$XDPHW_IFACE" > "$dir/rx-after.txt"
        $XDPHW_SENDER_EXEC ethtool -S "$XDPHW_SENDER_IFACE" > "$dir/tx-after.txt"
        python3 "$HERE/counters.py" window "$dir" --step "$rate" --window "$w" --queues "$XDPHW_QUEUES" \
            --tol-abs "$XDPHW_TOL_ABS" --tol-rel "$XDPHW_TOL_REL" --out "$windows"
        tail -1 "$windows" | grep -q '"clean": true' || step_clean=0
    done
    if [ "$step_clean" = "0" ]; then
        echo "step $rate pps is not clean: the sweep stops here (the knee is below it)"
        break
    fi
done
python3 "$HERE/counters.py" summary "$windows" > "$XDPHW_OUT/summary.txt"
cat "$XDPHW_OUT/summary.txt"
grep '^ROW ' "$XDPHW_OUT/summary.txt" | while read -r _ name verdict detail; do
    echo "$name $verdict" >> "$XDPHW_OUT/rows.txt"
done
grep -q '^ROW throughput_by_hardware_counters PASS' "$XDPHW_OUT/summary.txt" || rows_failed=$((rows_failed + 1))

echo quit >&8
wait "$tx_pid" 2>/dev/null

# ---------------------------------------------------------------- latency_by_hardware_timestamps

# The receiver's frames arrive through the library's XDP program, which
# requests no RX metadata, so an AF_XDP frame carries no hardware receive
# timestamp on this path; the probe is labelled software. ethtool -T is
# recorded so the report names what the NIC itself offers.
ethtool -T "$XDPHW_IFACE" > "$XDPHW_OUT/timestamping.txt" 2>&1
rx_ask lat_reset LATRESET > /dev/null
$XDPHW_SENDER_EXEC "$SENDER" probe --server "$XDPHW_ADDRESS:$XDPHW_PORT" --count "$XDPHW_PROBE_COUNT" --interval-us "$XDPHW_PROBE_INTERVAL_US" \
    > "$XDPHW_OUT/probe.out" 2> "$XDPHW_OUT/probe.err" &
probe_pid=$!
record_pid "$probe_pid" xdp_hw_sender-probe
wait "$probe_pid"
probe_status=$?
sleep 0.5
latency="$(rx_ask lat_report LATENCY)"
echo "$latency" > "$XDPHW_OUT/latency.txt"
count="$(echo "$latency" | sed -n 's/.*count=\([0-9]*\).*/\1/p')"
if [ "$probe_status" = "0" ] && [ -n "$count" ] && [ "$count" -ge $((XDPHW_PROBE_COUNT * 99 / 100)) ]; then
    row latency_by_hardware_timestamps PASS "SOFTWARE TIMESTAMPS (the library's XDP program requests no RX hardware timestamp): ${latency#LATENCY }"
else
    row latency_by_hardware_timestamps FAIL "probe status $probe_status, ${latency:-no LATENCY line}; see probe.out and probe.err"
fi

# ---------------------------------------------------------------- stop; restore follows in hold.sh's trap

echo quit >&7
for ((i = 0; i < 100; i++)); do
    kill -0 "$rx_pid" 2>/dev/null || break
    sleep 0.1
done
exec 7>&- 8>&-

echo "== rows (restore_leaves_the_baseline follows from restore.sh)"
cat "$XDPHW_OUT/rows.txt"
[ "$rows_failed" = "0" ] && exit 0
exit 1
