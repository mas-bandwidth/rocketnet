# RocketNet v1.1.56

Changes compared with the `v1.1.55` tag. A point release: no packet layout or
protocol version change, and v1.1.56 endpoints interoperate with every v1.x
endpoint. No counter row is added: the contract holds 290 counters and no
released id moved.

- Update clock: an update time above 2^52 seconds (about 143 million years)
  is refused like a negative or backwards time: the last good time is kept,
  the call is counted as API misuse, and it logs. The ceiling judges the
  caller's own times only; a message object's clock, which can run ahead of
  the caller's times after a backwards step, is handed to its transport
  without that check, so the transport's clock keeps pace with the object's.
- Ordered channels: the held-message index keys each entry's home cell per
  connection, with a secret drawn when the connection's region is carved.
  Before, the home was fixed and public, so a sender could choose keys that
  share one run of cells and make every hold and lookup walk it.
- Send errors: the errno kept for a socket's first unexplained send failure is
  the first one recorded across all calls. Before, two calls that each failed
  with a different errno added them together, naming neither (on macOS,
  `ENOTSUP` then `EBADF` read as 54). Its only reader is the socket shutdown
  log line.
- Transport contract: `rocketnet_driver.h` says what the built-in loopback
  transport does with a datagram larger than the receive buffer: it drops it
  whole and counts it, rather than copying what fits.
- Header comments: the production build's refusals of development keys,
  tokens and the test packet filter are stated where each happens; the
  `NO_TRANSPORT_FOUND` text says when a client and a server start return it;
  the server address text says how to bind one local address; the network
  thread defaults point at the measurement `PERFORMANCE.md` records instead
  of dated runs; the iocp send-thread limit says start moves to the next
  transport; the loopback hub's 1476-byte slot is explained.
- Examples: `game_loop.c` and the example pages say what the examples do, in
  the order the server frame runs.
- Tests:
  - Message-path unit oracles check every delivery whole, every resend's
    opening, and the timeout boundary in every run.
  - Lifecycle and public API unit cases are repaired; a crypto source failure
    seam lets the key generation failure cases drive the real check, and init
    reports `CRYPTO_INIT_FAILED` and unwinds when the crypto source fails.
  - Status and configuration unit cases pin what they claim (token clock,
    development key warning, create statuses, refusal cases on the loopback
    transport, and others), with a duplicate case and a dead branch removed.
  - Simulator, loopback and transport unit oracles are strengthened, each with
    a planted red line; the client I/O stop wait has a test seam so a unit test
    neither sleeps nor hangs on it.
  - A production-build unit case passes the internal server create call's
    newer retained-transport argument; without it, the merged tree's
    production builds did not compile.
- Models: `RNUpdateClock` models the transport's clock following the message
  object's, with a reversed witness of the transport held to the caller's
  ceiling.
- The Linux, Windows and macOS light memory pins are set for this tree.

Known limits of this release's checks:

- The macOS light memory pin is derived from the Linux and Windows
  measurements; it is confirmed by the release pull request's macOS job.

Merged branches:

- `tests/v1.1.x/p1-message-oracles`
- `tests/v1.1.x/p7-lifecycle-api-units`
- `tests/v1.1.x/p8-status-config-units`
- `tests/v1.1.x/p9-simulator-loopback-driver-units`
- `fix/v1.1.x/held-index-keyed-home`
- `docs/v1.1.x/examples-and-comments`
- `fix/v1.1.x/contract-examples-tools`

- Tag: `v1.1.56`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.56).
