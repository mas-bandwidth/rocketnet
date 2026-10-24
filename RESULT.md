RESULT: rn-client-gso-linux-b 83101c6a99e1055c9da23e5de50bd7c248216b3a

Head: 83101c6a99e1055c9da23e5de50bd7c248216b3a
Branch: sprint/rn-client-gso-linux-b.w5.g1.e1
Verdict: done

This sha is the merge of origin/main `1a2f2c435afe1fba2120aada52954c1b798c8a40` into the sprint branch. Parents: `1e6d2ee87b665d52dea98726a686a0edd3e1bed1` and `1a2f2c435afe1fba2120aada52954c1b798c8a40`. The merge was clean. `git diff origin/main...HEAD` is the five implementation files (552 insertions, 19 deletions). Added lines contain neither forbidden word. JOB.md is untracked and is not in that stat.

Gate on intel (user nova, gcc 15.2.0, kernel 7.0.0-38-generic, 16 cores), checkout detached at this sha under `/home/nova/johnny-gso-b/rocketnet`, fetched from GitHub after the push. Start 2026-10-24T12:43:21+00:00, end 2026-10-24T12:52:48+00:00. The gate shell raised the soft descriptor limit from 1024 to 65536 (hard limit already 524288) before the commands. Locked-memory soft limit was already 1048576 KiB. Server transport for the load cell was portable. xdp was not started, not as root, and not on the uplink.

EXIT make_release 0
RELEASE_WARNING_LINES 0
CMAKE_BUILD_TYPE:STRING=Release
EXIT bin_test 0
EXIT bin_functional 0
EXIT make_clean 0
EXIT make_dev_off 0
DEV_OFF_WARNING_LINES 0
ROCKETNET_DEV:BOOL=OFF
EXIT check_tla 0
EXIT load 0

`./bin/test` printed `test_client_gso_receive_gro_fault_is_counted` and `test_client_gso_receive_splits_gro_by_stride` as passing names, then `*** ALL TESTS PASSED ***`. `./bin/functional` printed `test_gso_client_transport_wire_gro` as a passing name (the case checks the client GRO batch counter is above zero when the server transport is gso), then `*** ALL TESTS PASSED ***`. The functional log has zero lines containing `check failed`. `python3 tools/check_tla_coverage.py --all` printed `COVERAGE OK 105 rows`.

Load summary, one line, 5838 characters, ending `verdict=PASS`:

load: connected=1000 connect_s=0.680 connect_p50_s=0.650 connect_max_s=0.679 profile=default client_public_required_bytes=9871552 server_public_required_bytes=18295678336 connect_within_10s=PASS no_unexpected_disconnect=PASS reliable_delivery=PASS unreliable_integrity=PASS counters_agree=PASS sent=600000 received=600000 drops=0 server_update_p50_us=844 server_update_p99_us=2538 rss_mb=1080.47 server_transport_io=portable client_transport_io=gso fd_limit_soft=65536 fd_limit_hard=65536 fds_start=3 fds_peak=2006 fds_end=3 threads_start=1 threads_peak=1003 threads_end=1 ... verdict=PASS

Granted names in that line: server_transport_io=portable, client_transport_io=gso. sent=600000 received=600000.

No TLA module was added. UDP_GRO receive splits a coalesced buffer with rocketnet_coalesce_split into the existing receive slots and does not add a state the client I/O handoff models lack. TLC was not run. tla/RUNS.tsv was not hand-edited.

Proposed docs/roadmap.sexp diff, not applied. Row 183, client-transports, platforms, Linux cell. macOS and Windows stay `-`.

```
-    (:feature "Linux: the gso transport gets a client side (sendmmsg and recvmmsg)" :cells (n - -) :note "no PR yet; gso's client preference is 0 (unavailable) in sdk/rocketnet_drivers.inc today")
+    (:feature "Linux: the gso transport gets a client side (sendmmsg and recvmmsg)" :cells (y - -) :evidence ((Linux "83101c6a on intel, gcc Release. ./bin/test printed test_client_gso_receive_splits_gro_by_stride and test_client_gso_receive_gro_fault_is_counted, then ALL TESTS PASSED. ./bin/functional printed test_gso_client_transport_wire_gro (GRO batch counter above zero against a gso server), then ALL TESTS PASSED. Load ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=gso ./bin/load 1000 60: sent=600000 received=600000 server_transport_io=portable client_transport_io=gso verdict=PASS.")) :note "client preference in sdk/rocketnet_drivers.inc is pin-only, not a rank above portable")
```

Not done: the two unit cases were not executed at BASE. Their names are absent from origin/main (`git grep` found none); that is not a red run. No clang rebuild. docs/roadmap.sexp and ROADMAP.md were not edited. TLC was not run. JOB.md was not committed. No pull request. main was not pushed.
