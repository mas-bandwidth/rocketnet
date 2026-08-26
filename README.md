# RocketNet

Low-level network transport library. It's fucking fast!

RocketNet carries the flow codebase forward under a new name: a client/server
UDP transport with zero copy send and receive, an AF_XDP kernel-bypass fast
path on Linux, portable socket paths everywhere else, and the netcode.io
connect-token security model. It runs the space game today.

The planned C API — three packet types (reliable in-order, latest, unreliable),
uint64 destinations addressing a client or a flow of subscribed clients, and
swappable transports behind one seam — does not exist yet. We iterate towards
it from this base; the design record lives in issue #2 and the target header in
PR #3.

## Building

See [BUILDING.md](BUILDING.md). Binaries land in `dist/`; run the test suite
with `./dist/test`.

## Documentation

- [sdk/rocketnet.h](sdk/rocketnet.h) — the public header.
- [CLAUDE.md](CLAUDE.md) — contributor guide.

Private repository. License: [AGPL-3.0](LICENSE).
