#!/bin/sh
# guest: the server in netns rnfw behind a veth, the client in the root netns;
# each case loads one nft ruleset in the server's netns and runs a client
cd /home/nova/bench/xdp-vlan
F=scratch-fw
ip netns add rnfw
ip link add rnfw0 type veth peer name rnfw1 netns rnfw
ip addr add 10.88.0.2/24 dev rnfw0
ip link set rnfw0 up
ip -n rnfw addr add 10.88.0.1/24 dev rnfw1
ip -n rnfw link set rnfw1 up
ip -n rnfw link set lo up
modprobe nf_conntrack 2>/dev/null
for c in none ufw ufw-notrack hard hard-notrack hard-notrack-sport; do
    ip netns exec rnfw nft flush ruleset
    if [ "$c" != none ]; then ip netns exec rnfw nft -f $F/$c.nft || echo "$c nft-load-failed"; fi
    ip netns exec rnfw sysctl -qw net.netfilter.nf_conntrack_udp_timeout=1 net.netfilter.nf_conntrack_udp_timeout_stream=2 2>/dev/null
    ip netns exec rnfw ./bin/server --address 10.88.0.1:40000 --transport gso --max-clients 8 --duration 14 > $F/$c.server.log 2>&1 &
    sleep 2
    timeout 30 ./bin/client --address 10.88.0.1:40000 --duration 6 --cycles 1 > $F/$c.client.log 2>&1
    rc=$?
    n=$(ip netns exec rnfw cat /proc/sys/net/netfilter/nf_conntrack_count 2>/dev/null)
    k=$(grep -c "to 'connected'" $F/$c.client.log)
    t=$(grep -c "timed out" $F/$c.client.log)
    echo "CASE $c client_rc=$rc connected_lines=$k timed_out_lines=$t conntrack_count_after=$n"
    wait
    sleep 4
done
