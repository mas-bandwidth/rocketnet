#!/usr/bin/env bash
#
# Runs inside the guest run.sh boots, as the guest's root, on the guest's own
# kernel. Everything here (the bpffs mount, the namespaces, the veth, the
# attached program, the AF_XDP socket) lives and dies with the VM; the only
# path written on the host is OUT, the run's own directory.
#
#     guest.sh rows   KERNEL OUT BUILD REPO MODULES     each row its own pass or fail line
#     guest.sh record KERNEL OUT BUILD REPO MODULES     capture the development-key handshakes
#
# MODULES is the kernel's module tree when the kernel is a distribution
# package (veth is a module there), or "-" when everything is built in.

set -u

mode=$1 kernel=$2 out=$3 build=$4 repo=$5 modules=$6
bin="$build/bin"
manifest="$repo/test/xdp_pcaps/manifest.tsv"
pcaps="$repo/test/xdp_pcaps"
object="$build/objects/server_xdp.o"

row() {
    printf '%s\t%s\t%s\n' "$kernel" "$1" "$2" >> "$out/rows.tsv"
    printf 'ROW %s %s %s\n' "$kernel" "$1" "$2"
}

echo "guest: $(uname -r) as $(id -un)"
if [ "$(id -u)" != 0 ]; then
    echo "guest: not root inside the guest; every row fails"
fi

mount -t bpf bpf /sys/fs/bpf 2>/dev/null || true
ulimit -l unlimited 2>/dev/null || true

# veth, from the kernel's own module tree when it is not built in
load_module() {
    local name=$1 file
    [ "$modules" = "-" ] && return 0
    grep -q "^$name " /proc/modules 2>/dev/null && return 0
    file=$(find "$modules" -name "$name.ko*" 2>/dev/null | head -1)
    [ -n "$file" ] || return 0
    case "$file" in
        *.zst) zstd -qdc "$file" > "/tmp/$name.ko" && insmod "/tmp/$name.ko" ;;
        *.xz) xz -dc "$file" > "/tmp/$name.ko" && insmod "/tmp/$name.ko" ;;
        *) insmod "$file" ;;
    esac
}
load_module veth

# the two ends of the corpus network: the server end in netns srv, the
# client end in netns cli, with the addresses manifest.tsv's #config names.
# The MTU is 2000 so the frames past the 1472-byte receive ceiling, and an
# IPv6 packet at the ceiling, can be sent at all; veth keeps driver-mode XDP
# at that size.
make_network() {
    ip netns add srv && ip netns add cli &&
    ip link add vsrv type veth peer name vcli &&
    ip link set vsrv netns srv && ip link set vcli netns cli &&
    ip -n srv link set lo up && ip -n cli link set lo up &&
    ip -n srv link set vsrv address 02:00:00:00:02:01 &&
    ip -n srv link set vsrv mtu 2000 && ip -n cli link set vcli mtu 2000 &&
    ip -n cli link set vcli address 02:00:00:00:02:02 &&
    ip -n srv addr add 10.211.0.1/24 dev vsrv && ip -n srv addr add fd00:211::1/64 dev vsrv nodad &&
    ip -n cli addr add 10.211.0.2/24 dev vcli && ip -n cli addr add fd00:211::2/64 dev vcli nodad &&
    ip -n srv link set vsrv up && ip -n cli link set vcli up
}

if ! make_network > "$out/network.log" 2>&1; then
    echo "guest: the veth network could not be made: $(tail -1 "$out/network.log")"
fi

# ---------------------------------------------------------------------------
# record: the library's development-key client and server on the portable
# driver, once over IPv4 and once over IPv6, captured on the server's veth

if [ "$mode" = record ]; then
    for family in ipv4 ipv6; do
        case $family in
            ipv4) address="10.211.0.1:40000" ;;
            ipv6) address="[fd00:211::1]:40000" ;;
        esac
        rm -f "$out/stop"
        ip netns exec srv "$bin/xdp_vm_replay" capture vsrv "$out/recorded_$family.pcap" 40000 24 "$out/stop" > "$out/capture_$family.log" 2>&1 &
        capture=$!
        ip netns exec srv env ROCKETNET_DRIVER=portable "$repo/bin/server" "$address" > "$out/server_$family.log" 2>&1 &
        server=$!
        sleep 1
        ip netns exec cli env ROCKETNET_DRIVER=portable timeout -s INT 3 "$repo/bin/client" "$address" > "$out/client_$family.log" 2>&1
        sleep 1
        kill -INT "$server" 2>/dev/null; wait "$server" 2>/dev/null
        touch "$out/stop"; wait "$capture"
        echo "guest: recorded $family: $(cat "$out/capture_$family.log")"
    done
    exit 0
fi

# ---------------------------------------------------------------------------
# verifier_loads: every BPF object through this kernel's verifier

verifier_ok=1
: > "$out/verifier.txt"
for o in "$build"/objects/*.o; do
    echo "## $(basename "$o")" >> "$out/verifier.txt"
    "$bin/xdp_vm_attach" verify "$o" >> "$out/verifier.txt" 2>&1 || verifier_ok=0
done
insns=$(grep '^verified_insns' "$out/verifier.txt" | awk '{ printf "%s=%s ", $2, $3 }')
if [ "$verifier_ok" = 1 ] && [ -n "$insns" ]; then
    row verifier_loads "PASS verified_insns ${insns% }"
else
    row verifier_loads "FAIL $(grep -v '^##' "$out/verifier.txt" | tail -3 | tr '\n' ' ')"
fi

# ---------------------------------------------------------------------------
# test_run_replay_matches_manifest: every corpus frame through BPF_PROG_TEST_RUN

if "$bin/xdp_vm_attach" testrun "$object" "$manifest" "$pcaps" > "$out/testrun.txt" 2>&1; then
    row test_run_replay_matches_manifest "PASS $(tail -1 "$out/testrun.txt")"
else
    row test_run_replay_matches_manifest "FAIL $(tail -1 "$out/testrun.txt"); see testrun.txt"
fi

# ---------------------------------------------------------------------------
# native_attach_on_veth, af_xdp_copy_bind_receives,
# attached_replay_matches_manifest: one attached server, the corpus sent from
# the client namespace

rm -f "$out/ready" "$out/stop" "$out/received.pcap" "$out/counters.txt"
ip netns exec srv "$bin/xdp_vm_attach" serve "$object" vsrv native "$manifest" \
    "$out/received.pcap" "$out/counters.txt" "$out/ready" "$out/stop" > "$out/serve.log" 2>&1 &
serve=$!
for _ in $(seq 1 200); do
    [ -f "$out/ready" ] && break
    kill -0 "$serve" 2>/dev/null || break
    sleep 0.1
done

ip -n srv -d link show dev vsrv > "$out/link_attached.txt" 2>&1
if grep -q '^attached native mode' "$out/serve.log" && grep -Eq 'mtu [0-9]+ xdp ' "$out/link_attached.txt"; then
    row native_attach_on_veth "PASS ip -d link shows driver-mode xdp on vsrv"
elif grep -q 'refused by the kernel' "$out/serve.log"; then
    row native_attach_on_veth "FAIL $(grep 'refused by the kernel' "$out/serve.log" | head -1)"
else
    row native_attach_on_veth "FAIL $(head -c 300 "$out/link_attached.txt" | tr '\n' ' ') $(tail -2 "$out/serve.log" | tr '\n' ' ')"
fi

sent="not sent"
if [ -f "$out/ready" ]; then
    sent=$(ip netns exec cli "$bin/xdp_vm_replay" send vcli "$manifest" "$pcaps" 2>&1 | tail -1)
fi
touch "$out/stop"
wait "$serve"

received=$(sed -n 's/^af_xdp received \([0-9]*\) frames$/\1/p' "$out/serve.log")
if grep -q '^af_xdp bound copy mode' "$out/serve.log" && [ "${received:-0}" -gt 0 ]; then
    row af_xdp_copy_bind_receives "PASS bound in XDP_COPY on queue 0; $received frames arrived"
else
    row af_xdp_copy_bind_receives "FAIL bind: $(grep -m1 -e 'af_xdp bound' -e 'AF_XDP bind' "$out/serve.log" || echo 'never reached'); received ${received:-none}"
fi

if [ -f "$out/received.pcap" ] && [ -f "$out/counters.txt" ] &&
   "$bin/xdp_vm_replay" compare "$manifest" "$pcaps" "$out/received.pcap" "$out/counters.txt" > "$out/compare.txt" 2>&1; then
    row attached_replay_matches_manifest "PASS $sent; $(grep '^socket frames' "$out/compare.txt")"
else
    row attached_replay_matches_manifest "FAIL $sent; $(grep -v '^  counter [A-Z_]* [0-9]*$' "$out/compare.txt" 2>/dev/null | head -3 | tr '\n' ' ')"
fi

# ---------------------------------------------------------------------------
# detach_leaves_no_program

ip -n srv -d link show dev vsrv > "$out/link_detached.txt" 2>&1
pins=$(find /sys/fs/bpf -mindepth 1 2>/dev/null | head -5 | tr '\n' ' ')
programs=$(bpftool prog show 2>/dev/null | grep -c ' xdp ' || true)
if grep -q '^detached$' "$out/serve.log" && ! grep -q 'prog/xdp' "$out/link_detached.txt" && [ -z "$pins" ] && [ "$programs" = 0 ]; then
    row detach_leaves_no_program "PASS no xdp on vsrv, no pin under /sys/fs/bpf, no xdp program loaded"
else
    row detach_leaves_no_program "FAIL serve: $(tail -1 "$out/serve.log"); pins: ${pins:-none}; xdp programs loaded: $programs"
fi

ip netns del srv 2>/dev/null
ip netns del cli 2>/dev/null
exit 0
