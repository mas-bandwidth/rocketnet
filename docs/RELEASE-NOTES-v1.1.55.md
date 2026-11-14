# RocketNet v1.1.55

Changes compared with the `v1.1.54` tag. A point release: no packet layout or
protocol version change, and v1.1.55 endpoints interoperate with every v1.x
endpoint. No counter row is added: the contract holds 290 counters and no
released id moved.

- Handshake:
  - The client sends its next handshake packet in the same update in which a
    server reply advances it, instead of one update later.
  - A connect token refused after its signature check is not checked again in
    the same update.
- Server and client lifecycle:
  - A server start that fails keeps its transport until the transport's
    worker has exited, and a kept transport's protocol keys are wiped at the
    handoff. A failed message create waits for its transport's worker before
    freeing it.
  - Every negative update time is refused before it reaches the clock.
- Message layer:
  - The sent-packet map's eviction counter (`MESSAGE_SENT_MAP_EVICTED`) counts
    only unacknowledged sequence numbers that were queued; its documentation
    says what it counts, where eviction starts, and that receipt-only packets
    share the same ring.
  - The send store keeps a free stack per size class, so a receiver can no
    longer steer the choice of a chunk into a scan; the cost of the choice is
    counted.
  - Writer finish restarts a short channel-route start and validates the
    bytes before it writes.
- Transports:
  - The documentation names `rio` the Windows server transport of choice at
    the 1000-player peak, set explicitly with a bind address; without one it
    runs one lane and does not hold the peak. `auto` is unchanged: `iocp` is
    the default and the fallback. The Windows network adapter setup for
    `rio` and `iocp` is documented.
  - POSIX receive treats `EMSGSIZE` as weather, not a fault.
  - The loopback client waits and is woken instead of polling every
    millisecond; the loopback hub's public contract and latency timing are
    documented.
  - The server protocol reads the repeat table's first words with the
    library's 64-bit read helper.
- Build, sanitizers and load:
  - The rebuilt io_uring transport's carved blocks carry red zones.
  - Under ThreadSanitizer the test binary links with clang, and only the
    poisoned shadow ranges are unpoisoned.
  - The bundled crypto's POSIX lock tests its owner without thread-local
    storage, and random draws are taken under the crypto source.
  - The load harness counts refusals independently along the chain and
    reports streams and p99 from the run.
- Backend examples: the token minters wipe the session keys on every exit.
- Release gates and checks: a release whose version number would collide with
  an earlier one is refused; the tag row goes red only on its own plant; the
  coverage gate refuses a code citation of a model or action that does not
  exist; several checks that could pass whatever they observed now fail when
  they should.
- CI summary jobs: the job-list read lets a slow but answering GitHub API use
  the time left before its 45-second deadline, and starts another try only
  while at least ten seconds are left.
- Models: the simulation mirror rows claim only what their checks observe,
  and the writer-finish model matches the code.
- Tests: the keep-alive count case above the build maximum runs only while
  the build is below the client ceiling, so a build at the ceiling passes;
  builder, window and payload unit tests are stronger; acknowledgement
  map count checks run in development builds only; the simulation reference
  traces and the transport fuzz seed corpora are regenerated for the
  handshake change.
- The wire standard's text: promotion admission, the send budget per send
  pass, due resends waiting for transport admission, sealing scope, and the
  delivery bound, with text corrections across `MESSAGES.md`,
  `TRANSPORT.md` and `RATIONALE.md`.
- Documentation: Windows, macOS, memory and load, errors and keys, usage, and
  the process and release-notes documents now say what the code does.
- The macOS, Windows and Linux light memory pins are measured for this tree.

Known limits of this release's checks:

- The simulation reference traces for the handshake change were measured on
  Linux; they are first compared on macOS and Windows by the nightly tier.
- One trace-check shard runs close to the 120-second per-job cap.

Merged branches:

- `fix/v1.1.x/standard-conformance`, `fix/v1.1.x/standard-text-2`
- `fix/v1.1.x/sent-map-receipt-eviction`
- `fix/v1.1.x/contract-examples-tools`
- `fix/v1.1.x/hostile-input-cost`, with `fix/v1.1.x/hostile-ci-repair`
- `fix/v1.1.x/build-sanitizers-load`
- `fix/v1.1.x/gates-and-checks`
- `fix/v1.1.x/writer-finish`
- `fix/v1.1.x/loopback`
- `fix/v1.1.x/send-store-scan`
- `fix/v1.1.x/handshake-and-ack-latency`, with `fix/v1.1.x/handshake-ci-repair`
- `fix/v1.1.x/summary-api-retry`
- `fix/v1.1.x/windows-prefer-rio`, with `docs/v1.1.x/windows-adapter-setup`
- `fix/v1.1.x/v1155-a-repairs`
- `models/v1.1.x/m2-aurora`, `tests/v1.1.x/p2-aurora`
- `docs/v1.1.x/windows`, `docs/v1.1.x/macos`, `docs/v1.1.x/memory-load`,
  `docs/v1.1.x/process-and-release-notes`, `docs/v1.1.x/errors-keys`,
  `docs/v1.1.x/usage-guides`

- Tag: `v1.1.55`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.55).
