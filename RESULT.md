RESULT: rn-uring-gso-gro-a sha=97de7865bd9e6360981b88121da469f1ef04b6dd tier: frontier

Head: 97de7865bd9e6360981b88121da469f1ef04b6dd
Branch: sprint/rn-uring-gso-gro-a.w4.g2.e1
Verdict: done

The code head is 97de7865, the merge of main (221e3bee) into the segmentation branch. That merge does not change sdk/rocketnet_server_uring_linux.cpp, sdk/rocketnet_server_uring.h, sdk/rocketnet_uring_codec.h, tla/RNUringDriver.tla, tla/MCRNUringDriver.tla, tla/MCRNUringDriver.cfg, tla/MCRNUringDriverBrokenFreeRunAtSubmit.cfg, or tla/README-RNUringDriver.md relative to bacbf8ad. The bench print used to count SENDMSG submissions is not in this commit.

Gate on amd-2 at 97de7865, 2026-10-24T07:15:30Z to 07:23:30Z: gcc Release make, ./bin/test, the three wire cases, test_uring_gso_message_stride_and_tail, ./bin/functional, make ROCKETNET_DEV=OFF, and a clang Release dev rebuild all exited 0. python3 tools/check_tla_coverage.py --all printed COVERAGE OK 105 rows. The new case is absent at 221e3bee, so it was not executed there.

TLC was not re-run. The model files are unchanged since the recorded rows (MCRNUringDriver.cfg PASS, MCRNUringDriverBrokenFreeRunAtSubmit.cfg PASS as an invariant violation of NoReuseWhileKernelOwned).

Load, server alone on amd-2 (100.84.43.61), 500 clients on amd-1 and 500 on hulk, ROCKETNET_SERVER_TRANSPORT=io_uring, full, 60 s, aggregate ceiling 80000 kbps. All three runs connected 1000 clients, missed_due_slots=0, delivery PASS, load PASS, bandwidth PASS. timing_verdict=FAIL against the 100 us budget; that budget is not this card's bar. server_send_packets was called 630 times in the traffic-plus-drain window (10 Hz for 63 s).

- base 221e3bee, run b72898267fb58433, clang Release dev: p50 4392 us, p99 14364 us, max 18056.779 us
- head 97de7865, run f3eda2cc014ad64a, unmodified clang Release dev binary: p50 4368 us, p99 5539 us, max 6197.578 us
- head 97de7865, run 950b7aa227fa1e8c, same source plus two integer adds and a destroy-time print: p50 4692 us, p99 6938 us, max 13737.276 us

p50 sits between 4.4 ms and 4.7 ms on all three. p99 at head was 5539 us and 6938 us. The one base run's p99 was 14364 us.

SENDMSG submissions are not in the stock receipt. A destroy-time print on the bench binaries, subtracted by the one-client warm server that load_split destroys first, gives the 1000-client server:

- head: 824642 SENDMSG SQEs, 824642 packets completed, GSO_SENDS 0, GSO_FALLBACKS 0, over 9031 send_packets calls in the whole server life
- base: 825995 SENDMSG SQEs, 825995 packets completed, over 10145 send_packets calls. Base has no GSO counter; the code submits one SENDMSG per slot

GSO_SENDS stayed 0, and submissions equal packets, so this load never grouped a run. Each of the 600 traffic ticks offers 1000 messages, one per client, so those leave as about 1000 SENDMSG submissions per traffic flush plus acks, at both revisions. The other send_packets calls are the 1 ms connect and teardown loops. Whole-life submissions per call are 91.3 at head and 81.4 at base because those short calls dilute the average. The UDP_SEGMENT probe logged yes at head before traffic.

Proposed roadmap diff, not applied. docs/roadmap.sexp line 142, "Send segmentation by the kernel (UDP GSO on Linux, USO on Windows), probed with fallback", io_uring cell n to y, evidence naming test_io_uring_transport_server_wire_gso_ipv4, _wire_gso_ipv6 (GSO_SENDS above zero, payloads byte-exact), _wire_plain (GSO_SENDS zero), test_uring_gso_message_stride_and_tail, and this measurement: at 1000 clients full, GSO_SENDS stayed 0 because each flush is one packet per destination. docs/roadmap.sexp and ROADMAP.md were not edited.
