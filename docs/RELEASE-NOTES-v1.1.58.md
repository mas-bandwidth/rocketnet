# RocketNet v1.1.58

Changes compared with the `v1.1.56` tag. A point release: no packet layout or
protocol version change, and v1.1.58 endpoints interoperate with every v1.x
endpoint.

v1.1.57 was labelled on `main` but never tagged or published. Its changes,
described in [the v1.1.57 notes](RELEASE-NOTES-v1.1.57.md), ship for the first
time in v1.1.58. The changes below come on top of them.

VLAN servers must be re-provisioned with the new scripts/rocketnet-provision.

Public surface added in this release (beyond v1.1.57):

- One counter, `XDP_KERNEL_DROPPED_NO_SESSION`, appended at id 291: the
  contract holds 292 counters and no released id moved.

- xdp:
  - The kernel program drops a session packet from a source that holds no
    session, counted in `XDP_KERNEL_DROPPED_NO_SESSION`, and a reused slot is
    marked while its old forget is still owed. A kept key's zero replay mark
    waits for the forgets too.
  - The filter passes the frames the frame reader refuses (wrong IP version,
    broadcast) on to the reader instead of judging them itself.
  - A server on a VLAN interface needs the parent interface's
    rx-vlan-offload off; the pre-flight warns when it is on, with the
    platform's error text.
  - Pre-flight and provisioning: queue k is paired with cpu num_os_cpus + k,
    irqaffinity is written as a cpu list, the hints name the address's
    interface, and provisioning refuses to unload the program under a live
    server, failing closed when the program's identity cannot be read.
- Client:
  - A small-order key in the server's challenge ends the connection attempt
    as a protocol violation (-10).
  - A standalone acknowledgement stamps the send clock.
- gso: a datagram shorter than a prefilter header is refused by the size pass
  and counted as the main thread counts it; there is no separate native size
  drop.
- io_uring: one destination refusing many segmented runs counts as one
  refusing destination, and only a destination the table does not already
  hold is counted.
- gso and io_uring placement: one clamp for the operating system's cpu count.
- Linux gateway lookup: a short netlink send is a system error (`EPROTO`).
- Comments and documentation: the payload ring, send pass hold, reserve
  refusal counters, segment cap and end reasons say what the code does; the
  Linux deploying, macOS, memory and load, performance, testing and process
  documents are corrected, and the performance record publishes the current
  X710, Windows and AWS measurements with their limits.
- The wire standard: second late batch of text fixes, configuration fields in
  the t2.3 shape, iocp stop drops and send memory bases, and io_uring source
  and no-buffer drops.
- Tests:
  - The embedded (P4), handshake and token (P5), reset, secrets and replay
    (P6) and native transport (P10) unit packages can now fail where they
    claim to, each with planted red lines.
  - On macOS the msgx client's latency row reports its delay without failing
    on it, as the portable client's does (a v1.1.57 change): under the shared CI
    suites the delay measures the machine's load, not the library.
  - The rio pin-refusal case checks that a refused pin moves
    `RIO_PIN_REFUSALS` by one, and the io_uring plan rows run only where the
    io_uring transport is built.
- Models: `RNRioWorker` (create's inline drain abandons an unposted socket;
  `RNRioDriver` is marked historical); the iocp receive admission, send lanes
  and rio lane layout; the live fleet run and split start; the ack block
  wrap, session acks and replay windows; the client I/O models and the
  update clock records; and the CBMC bound and reconstruct claims.
- Test seam (ROCKETNET_DEV only): the send queue recount behind the transport
  swap seam checks for a missing operations table first, so the mass
  disconnect unit case no longer faults.
- CI: the TLA+ trace check has a tenth shard for the msgx reach predicates.
- The Linux, macOS and Windows light memory pins and the loopback hashes are
  measured for this tree.

Merged branches (beyond those in v1.1.57):

- `fix/v1.1.x/xdp-vlan-offload`
- `fix/v1.1.x/v1157-disconnect-together-crash`
- `fix/v1.1.x/hostile-input-cost`
- `fix/v1.1.x/handshake-and-ack-latency`
- `fix/v1.1.x/gso-short-datagram-count`
- `fix/v1.1.x/segment-refusal-distinct-destinations`
- `fix/v1.1.x/reserve-refusal-counter-wording`
- `fix/v1.1.x/send-pass-hold-comment`, `fix/v1.1.x/send-ring-order-comment`
- `fix/v1.1.x/live-fleet-script-repairs`
- `fix/v1.1.x/standard-text-3`
- `tests/v1.1.x/p4-embedded-units`, `tests/v1.1.x/p5-handshake-token-units`,
  `tests/v1.1.x/p6-reset-secrets-replay-units`,
  `tests/v1.1.x/p10-native-transport-units`
- `models/v1.1.x/cbmc`, `models/v1.1.x/windows-rio-worker`,
  `models/v1.1.x/windows-iocp-layouts`, `models/v1.1.x/fleet-split`,
  `models/v1.1.x/ack-replay`, `models/v1.1.x/client-io`
- `fix/v1.1.x/nightly-ci-reds`, `fix/v1.1.x/v1157-hot-repairs`
- `ci/v1.1.x/trace-tenth-shard`
- `docs/v1.1.x/linux-deploying`, `docs/v1.1.x/macos-server-limits`,
  `docs/v1.1.x/memory-load`, `docs/v1.1.x/performance-stella`,
  `docs/v1.1.x/process-and-release-notes`, `docs/v1.1.x/testing-hardening`,
  `docs/v1.1.x/iocp-peak-roadmap`

- Tag: `v1.1.58`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.58).
