#!/bin/sh
# host: each mutation removes one piece of the VLAN code, rebuilds, runs the
# VLAN unit cases (and, for the filter, the kernel guest), then restores it
cd /home/nova/bench/xdp-vlan
U="test_xdp_vlan_size_limits test_xdp_vlan_receive_counts_refused_tags test_xdp_vlan_parse_refuses_other_tags_and_takes_stripped_frames test_xdp_vlan_frame_matches_golden_and_round_trips test_xdp_transport_host_vlan_server_takes_its_tag test_xdp_transport_host_vlan_tagged_passes test_xdp_transport_link_query_asks_the_index test_xdp_transport_link_classify_names_every_refusal test_xdp_transport_link_parse_refuses_malformed_attributes test_xdp_transport_link_parse_reads_the_device"
build() { cmake --build build --parallel 4 > build/mutate-build.log 2>&1; echo "$1 build rc=$?"; }
run() { for t in $U; do ./bin/test --run $t > build/mutate.$1.$t.log 2>&1; echo "$1 $t rc=$?"; done; }
build M0
run M0
bash scripts/rocketnet-provision selftest > build/mutate.M0.provision.log 2>&1; echo "M0 provision-selftest rc=$?"

# M1: the codec never sees a tag
sed -i '394s/if ( ( ( f\[12\] << 8 ) | f\[13\] ) != ROCKETNET_ETHERTYPE_VLAN )/if ( true )/' sdk/rocketnet_xdp_frame.h
echo "M1 applied $(git diff --stat | tail -1)"
build M1
run M1
git checkout -- sdk/rocketnet_xdp_frame.h

# M2: the filter passes every tagged frame up the stack
sed -i '506s/if ( config == NULL || config->vlan_id == 0 ||/if ( 1 || config == NULL || config->vlan_id == 0 ||/' sdk/xdp/server_xdp.c
echo "M2 applied $(git diff --stat | tail -1)"
build M2
run M2
bash tools/run_xdp_filter_guest.sh --artifact-dir build/mutate-M2-guest > build/mutate.M2.guest.log 2>&1; echo "M2 filter-guest rc=$?"
git checkout -- sdk/xdp/server_xdp.c

# M3: classify calls every device plain
sed -i '581s/^    \*reason = NULL;$/    *reason = NULL; return ROCKETNET_XDP_LINK_PLAIN;/' sdk/rocketnet_gateway_netlink_linux.cpp
echo "M3 applied $(git diff --stat | tail -1)"
build M3
run M3
git checkout -- sdk/rocketnet_gateway_netlink_linux.cpp

# M4: the provision resolver calls every device plain
sed -i '625i echo "plain $NAME"; return' scripts/rocketnet-provision
echo "M4 applied $(git diff --stat | tail -1)"
bash scripts/rocketnet-provision selftest > build/mutate.M4.provision.log 2>&1; echo "M4 provision-selftest rc=$?"
git checkout -- scripts/rocketnet-provision

build M5-restored
bash tools/run_xdp_filter_guest.sh --artifact-dir build/mutate-M0-guest > build/mutate.M0.guest.log 2>&1; echo "M0 filter-guest rc=$?"
echo MUTATE-DONE
