# RocketNet

Low-level client/server UDP transport library.

## Building

See [BUILDING.md](BUILDING.md). Binaries land in `bin/`, and on Windows in
`bin\Release\`; run the test suite with `./bin/test`, or with
`bin\Release\test.exe` on Windows (PowerShell or cmd.exe).

## Documentation

- [AGENTS.md](AGENTS.md) — guide for AI coding agents integrating RocketNet into a game.
- [USAGE.md](USAGE.md) — how to drive it: the frame, message sending and
  receiving, channels, configuration, and state transitions.
- [docs/LINUX.md](docs/LINUX.md) — the Linux server guide: the transport ladder
  (`xdp`, `gso`, `io_uring`, `portable`) and pinning, host requirements,
  provisioning, socket errors and ICMP, limits, containers, and verifying a host.
- [docs/MACOS.md](docs/MACOS.md) — building on macOS, the msgx and portable
  transports, App Nap and QoS, socket errors, the firewall prompt, lldb and
  verification on a Mac.
- [docs/PLATFORMS.md](docs/PLATFORMS.md) — supported, nightly, and unsupported platform rows and the CI workflows that prove them.
- [TRANSPORTS.md](TRANSPORTS.md) — native transport transport registry, capabilities, and extension interface.
- [sdk/rocketnet.h](sdk/rocketnet.h) — the public header, and the authority
  wherever a contract is written down.
- [examples/README.md](examples/README.md) — the feature-focused examples series, starting with hello.
- [standard/TRANSPORT.md](standard/TRANSPORT.md) — the transport standard: the wire format.
- [standard/MESSAGES.md](standard/MESSAGES.md) — the message standard.
- [standard/RATIONALE.md](standard/RATIONALE.md) — the rationale for both.
- [ROADMAP.md](ROADMAP.md) — the releases, streams and cards planned, rendered from the work record.
- [CLAUDE.md](CLAUDE.md) — contributor guide.

Private repository. License: [AGPL-3.0](LICENSE).
