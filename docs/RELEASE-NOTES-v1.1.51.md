# RocketNet v1.1.51

Changes compared with the `v1.1.50` tag. A point release: no packet layout or
protocol version change, and v1.1.51 endpoints interoperate with every v1.x
endpoint. No counter row is added: the contract holds 290 counters and no
released id moved.

- Network threads, one setting per transport: each server transport has its
  own config field and environment variable for its network threads:
  `num_xdp_threads` (1), `num_gso_workers` (1), `num_io_uring_workers` (2),
  `num_rio_lanes` (5), `num_iocp_completion_workers` (2) and
  `num_iocp_send_threads` (2), and `num_msgx_transmit_workers` (3). The
  shared `num_xdp_queues` is retired and warns if set. RIO and IOCP run one
  thread on a wildcard bind and say so. `scripts/rocketnet-provision` defaults
  to the XDP thread count. The docs' gso and io_uring thread counts are
  corrected.
- Review fixes:
  - an IOCP server with several workers reports a bind refusal by its status;
  - the counter row texts for RIO, IOCP and io_uring are corrected;
  - a Linux worker left on cpu 0 after a failed fallback is counted and
    warned, and no real-time scheduling is asked for;
  - the pin refusal unit tests read the real path;
  - two doc corrections.
- Documentation: [Performance](PERFORMANCE.md) covers five server transports,
  results first, with the steps to reproduce them; macOS is left out for now.

Also in this release, shipped in v1.1.50 but missing from its notes file
(they are in the v1.1.50 GitHub release):

- macOS server transport: the shared dual-stack socket reserves its IPv4 port
  first, so it no longer binds over a port another socket holds for IPv4;
  that bind is now refused with `TRANSPORT_BIND_REFUSED`. The comments now say
  which other sockets can still bind the server's port.
- The word "ladder" is retired: no API or counter change; the TLA+ model
  RNClientLadder is now RNClientTransportOrder; test, fuzz-corpus and tool
  names renamed. Released notes, the preview qualification records and the
  dated audits are left as written.
- `ROADMAP.md` is back at the repository root, generated from
  `docs/roadmap.sexp`, and linked from the README, with new entries for what
  is not done and what the review before v1.2.0 found.
- Tests and tools: the per-commit trace check runs in nine shards; it refuses
  a shard that holds no layer, and its summary line counts plant reruns
  (`reruns=N`).

- Tag: `v1.1.51`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.51).
