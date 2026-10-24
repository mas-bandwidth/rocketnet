RESULT: rn-linux-hard-killed-native-b sha=1f0e84e89c853b6341abf395c4f9fd16e0c81c00 tier: heavy

Head: 1f0e84e89c853b6341abf395c4f9fd16e0c81c00
Branch: sprint/rn-linux-hard-killed-native-b.w3.g2.e1
Verdict: done

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
+    (:feature "Linux: a hard-killed client or an ICMP error never stalls portable, gso, io_uring or xdp" :cells (y - -) :evidence ((Linux "1f0e84e8 on hulk. vng: six xdp cases, portable client, IPv4 and IPv6, CASE_EXIT 0; zero-copy bind refused (Operation not supported) and the queue bound in copy mode. Host ./bin/functional: 18 socket cells (portable, gso, io_uring) then ALL TESTS PASSED.")))
```

Not done: docs/roadmap.sexp and ROADMAP.md were not edited.
