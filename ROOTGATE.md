# Root gate for rocketnet-xdp-ipv6-receive (isolated-kernel tier)

Gate status: isolated-kernel tier MET; real-NIC hardware tier OWED, waiting on the dedicated XDP machine.

What ran: `test_xdp_filter -v` as root on a Linux bench member (amd-1, hostname b1), in a temp dir created and removed by the run. The test loads the XDP program and drives it with `BPF_PROG_TEST_RUN`; it attaches no program to any interface and creates no veth or netns. It is the verifier plus the program's verdicts, not XDP mode on a NIC.

Head (66d8282f), exit 0:

    verifier: processed 727 insns (limit 1000000) max_states_per_insn 4 total_states 75 peak_states 73 mark_read 0
    ipv4_rocketnet_packet_redirects: ok
    ipv6_rocketnet_packet_redirects: ok
    ipv6_extension_header_passes: ok
    ipv6_fragment_passes: ok
    ipv6_wrong_address_passes: ok
    ipv6_short_header_drops: ok
    ipv6_too_large_drops: ok
    ipv6_icmpv6_passes: ok
    ipv6_same_drops_as_ipv4: ok
    config_layout_matches_header: ok
    test_xdp_filter: 10 cases passed

Base (the same test binary against the object compiled from base `server_xdp.c`; the test file is new in this PR, so there is no base build of it), exit 1. The full failing-case list was not kept; the worker's report gives these lines:

    verifier: processed 356 insns (limit 1000000) max_states_per_insn 4 total_states 35 peak_states 35 mark_read 0
      FAIL ipv4 max-size packet: verdict XDP_DROP, wanted XDP_REDIRECT
    ipv4_rocketnet_packet_redirects: FAILED
      FAIL ipv6 max-size packet to the configured address: verdict XDP_PASS, wanted XDP_REDIRECT
    ipv6_rocketnet_packet_redirects: FAILED
    8 of 10 cases failed

Cleanup on the host: only the temp dir was created and it was removed; after, `ip link show` had no veth and `ip link show | grep -c xdp` was 0.

Not done: the real-NIC hardware tier (an XDP program attached to a provisioned NIC).
