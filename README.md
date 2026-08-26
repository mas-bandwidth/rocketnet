# RocketNet

Low-level network transport library. It's fucking fast!

RocketNet is a client/server UDP transport with zero copy send and receive, an
AF_XDP kernel-bypass fast path on Linux, and portable socket paths everywhere
else. It carries the flow codebase forward under a new name, narrowed to one
thing: a packet-level transport worthy of a 1000 player game.

Right now it is direct mode only: a client connects to a server address and
exchanges raw packets. The requirements are locked in
[REQUIREMENTS.md](REQUIREMENTS.md): first the minimal secure connect —
netcode-style connect tokens on public key crypto, with no secret shared
between the backend and the servers, so player hosted servers are never
excluded — then platform transports behind an easy-to-extend seam, then
reliability, flows and broadcast on top. Security construction is tracked in
issue #6, transports in issue #2.

## Building

See [BUILDING.md](BUILDING.md). Binaries land in `dist/`; run the test suite
with `./dist/test`.

## Documentation

- [sdk/rocketnet.h](sdk/rocketnet.h) — the public header.
- [CLAUDE.md](CLAUDE.md) — contributor guide.

Private repository. License: [AGPL-3.0](LICENSE).
