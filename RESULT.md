RESULT: rn-linux-hard-killed-native-b sha=e8041e4392cc5f75f14779c38d906667658a9fc8 tier: heavy

Head: e8041e4392cc5f75f14779c38d906667658a9fc8
Branch: sprint/rn-linux-hard-killed-native-b.w4.g1.e1
Verdict: LAND

w4: rocketnet_xdp_veth_format_link and its char-array spelling of the banned word are deleted; rocketnet_xdp_veth_pair_up_queues( bool ipv6, int veth_queues, ... ) carries main's iproute2 link-add line byte-identical, so no added line holds the word. Gate at e8041e43: gcc and clang, ROCKETNET_DEV=ON and OFF, make -j4 on amd-1 exit 0 with 0 warning lines each; ./bin/test ALL TESTS PASSED; 18 socket cells CASE_EXIT 0 on amd-1; six xdp cases CASE_EXIT 0 on hulk in vng with ci.yml's --append "irqaffinity=0 isolcpus=1 nohz_full=1" (without it the xdp server create is refused at posix_hard_killed_client.cpp:256); COVERAGE OK 105 rows.

The three hard-killed kinds are registered for the xdp server cell with a portable client, IPv4 and IPv6. On hulk, inside virtme-ng of the host kernel, with `--append "irqaffinity=0 isolcpus=1 nohz_full=1"`, each case exited 0 at this head:

CASE_EXIT:test_posix_hard_killed_client_xdp_ipv4_client_portable:0
CASE_EXIT:test_posix_hard_killed_client_xdp_ipv6_client_portable:0
CASE_EXIT:test_posix_send_to_closed_port_xdp_ipv4_client_portable:0
CASE_EXIT:test_posix_send_to_closed_port_xdp_ipv6_client_portable:0
CASE_EXIT:test_posix_client_server_gone_xdp_ipv4_client_portable:0
CASE_EXIT:test_posix_client_server_gone_xdp_ipv6_client_portable:0

Each case logged "server xdp queue 0 refused the zero copy bind (Operation not supported), binding in copy mode" and "server found no gateway on rnxdpfam0". The veth guest has no zero-copy support and no gateway. The cases still exited 0. XDP was not run as root on the hulk host.

The Linux socket cells of native-a ran in the host `./bin/functional` at the same head: portable, gso and io_uring, IPv4 and IPv6, for hard-killed, closed-port and server-gone (18 cases). The suite then printed ALL TESTS PASSED. FUNC_EXIT:0. `./bin/test` printed ALL TESTS PASSED. TEST_EXIT:0. The xdp cells skip on the host because rocketnet_test_xdp_veth_runs_here is false there.

`make` on hulk exited 0 and the log has no compiler warning lines. `make ROCKETNET_DEV=OFF` on amd-2 exited 0 with no warning lines. `python3 tools/check_tla_coverage.py --all` printed COVERAGE OK 105 rows.

The far MAC is read with `ip netns exec`, because setns does not switch the mounted sysfs.

Proposed docs/roadmap.sexp diff, not applied (line 449, platforms, Linux cell). macOS and Windows stay `-`.

```
-    (:feature "Linux: a hard-killed client or an ICMP error never stalls portable, gso, io_uring or xdp" :cells (n - -) :note "planned for the final deliverable")
+    (:feature "Linux: a hard-killed client or an ICMP error never stalls portable, gso, io_uring or xdp" :cells (y - -) :evidence ((Linux "e8041e43 on hulk. vng: six xdp cases, portable client, IPv4 and IPv6, CASE_EXIT 0; zero-copy bind refused (Operation not supported) and the queue bound in copy mode. Host ./bin/functional: 18 socket cells (portable, gso, io_uring) then ALL TESTS PASSED.")))
```

Not done: docs/roadmap.sexp and ROADMAP.md were not edited.
