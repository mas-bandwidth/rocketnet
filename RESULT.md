RESULT: rn-uring-gso-gro-b sha=b6d614eb2e8db0b183c5728c5aeae99cf8ed0271 tier: frontier

Verdict: HOLD
Head: b6d614eb2e8db0b183c5728c5aeae99cf8ed0271
Branch: sprint/rn-uring-gso-gro-b.w4.g2.e1

Why HOLD: the branch carries rn-uring-gso-gro-a (newest head ada3673b, merged) because this card builds on its send side, and gro-a has not landed. Until it lands, the diff against main names files outside this card's PATHS: tla/MCRNUringDriver.cfg, tla/MCRNUringDriverBrokenFreeRunAtSubmit.cfg, the four other MCRNUringDriverBroken*.cfg (RunRetiredByCqe = TRUE), and RESULT.md. After gro-a lands, a merge of main removes them and the card can land.

Carried: attempt 2's local head a1ec49f3 (pushed head 0e21a26f plus the docs commit), merged; then gro-a ada3673b, merged; then main (merge d18e95df). No reader findings were open.

Fixed this attempt:
- MCRNUringDriver.cfg timed out at the 110 s budget once KernelFill could coalesce (exit 124, 184189 distinct at 110 s on hetzner2). The reap now removes the segments it copies from bufdg, and BufferReturnedAfterCopy reads "a returned buffer holds no datagram still to copy": the same rule without per-buffer history. PASS in 53.0 s, 93559 distinct.
- RUNS.tsv had picked up duplicate stale rows for fragmentsandacks, rnblocks and rnuringdriver from the earlier merges; it is main's rows plus the seven fresh rnuringdriver records (tlacheck merge --keep). groups --stale [] ; check_tla_coverage --all COVERAGE OK 105 rows.

TLC (pinned tlacheck 0c97d595, jar 936a2620, hetzner2, workers 2, 110 s; vision was at load 81 on 64 cores): MCRNUringDriver.cfg PASS exit 0; BrokenReturnBeforeSplit exit 12 BufferReturnedAfterCopy (trace Init, Submit, KernelFill(1) with 1..2, Reap copies {1} leaving {2}, ReturnBuffer(1) -> Ring holding 2; checked by hand against rocketnet_server_uring_receive_cqe: io_uring_buf_ring_add follows the segment loop); ResetBeforeReap 12 NoReuseWhileKernelOwned; OneBank 12 ExposedStable; UnboundedReap 12 ReapWithinBudget; NoRearmRetry 13 temporal; FreeRunAtSubmit 12 NoReuseWhileKernelOwned.

Gate, amd-2 at d18e95df (main merged): gcc DEV ON make 0 warnings, ./bin/test exit 0 (test_uring_recvmsg_gro_stride runs), ./bin/functional exit 0 (test_io_uring_transport_server_wire_gro_ipv4, _wire_gro_ipv6, _receive_rearms_after_no_buffers run); gcc DEV OFF, clang DEV ON, clang DEV OFF all build exit 0 with 0 warnings; clang ./bin/test exit 0. amd-2 lo has tx-udp-segmentation on, so the wire_gro cases' GRO_BATCHES > 0 assertion is armed there. test_uring_recvmsg_gro_stride does not compile at BASE (no stride parse), so it is red at BASE.

Memory per queue: before 4096 x 2048 B = 8 MiB; after 4096 x 67584 B = 264 MiB (4 default queues: 32 MiB -> 1056 MiB), all allocated at create; pages become resident only where the kernel writes.

Load (server amd-2 alone, 500 clients amd-1 + 500 hulk, io_uring, full, 60 s, 80000 kbps; shared busy benches, one run each):
- base 1a2f2c43 run 86be413d06c382b3: server_update p99 40820 us, p50 7567; server_receive_messages p99 173 us; send_packets p99 39821; missed_due_slots 6000, load FAIL, delivery PASS.
- head b7acfd61 (same uring source as the final head) run c5a2b8f83d0018f4: server_update p99 10184 us, p50 6526; server_receive_messages p99 114 us; send_packets p99 10037; missed_due_slots 0, load PASS, delivery PASS. UDP_SEGMENT yes, UDP_GRO yes.
Timing FAIL both against the 100 us budget (pre-existing). Not done: receive_packets has no call row in the load_split receipt (it runs inside server_update), and receive CQEs per update are not printed; tools/ is outside PATHS. Under this load each datagram is from a distinct flow, so GRO does not coalesce (inference).

Proposed roadmap diff (not applied), docs/roadmap.sexp:141 "Receive coalescing by the kernel (UDP_GRO on Linux, URO on Windows), split by stride": cells (- n y n n y n -) -> (- n y y n y n -), add evidence (io_uring "<land sha> test_io_uring_transport_server_wire_gro_ipv4, test_io_uring_transport_server_wire_gro_ipv6 (GRO_BATCHES > 0, byte-exact, in order), test_uring_recvmsg_gro_stride; tla/RNUringDriver.tla Segments"), and drop io_uring from the note.

Tool defect: `nsc.sh friend stage` refused "--pg is required" although the wrapper sets NOVA_PG_DSN; passing --pg explicitly worked.
