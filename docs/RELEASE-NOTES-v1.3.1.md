# RocketNet v1.3.1

Changes since the `v1.3.0` tag. This is a point release. The packet layout and protocol version are the same as
v1.3.0, the public API and ABI are the same, and there are no new counters (the contract still holds 315).
v1.3.1 endpoints work with v1.3.0 endpoints.

## Fixes

- The io_uring server transport (Linux) now counts the datagrams it drops for size in two separate counters, one for
  too short and one for too large. Before, a counter read made while a datagram was being counted could report an
  oversize datagram as too short for that one read. `URING_RECEIVE_DROPPED_SIZE` keeps its id and its value, which is
  now the sum of the two.
- The portable, gso and io_uring server transports now read the machine's CPU count on the creating thread, before
  their worker threads start. Before, each worker read it as it started, and on Linux that read briefly opens a file.
  A program counting its open descriptors just after server create could count that file.
- On Windows, when the iocp transport cannot create a server because it runs out of memory, the error now names
  the call that failed.

## Tests, tooling and documentation

- Functional tests wait for a lost UDP disconnect to be ended by the server timeout. The server stop case polls
  `rocketnet_server_stopped` on every pass.
- The session fuzzer's silence oracle counts client-to-server silence from when the server's slot came up. New
  regression inputs come with it.
- The planted-violation table, the TLA+ trace checks (reach predicates, and the gso trace walk at the end of its
  budget) and the release-gate tooling (a GET answered 5xx is retried, then refused) were corrected.
- The documented create-only lifecycle example is now checked against the production SDK as well as the
  development one.

Upgrade: replace v1.3.0 with v1.3.1. You do not need to change any source.
