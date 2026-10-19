# RocketNet

RocketNet is a low-level client/server UDP transport and message library for real-time multiplayer games in C99 (with a C++17 implementation), built on public-key connect tokens, authenticated per-connection encryption, eight reliability modes over up to 32 independent channels, per-connection send budgets, zero allocations after create, and runtime-selected server transports (`portable`, `gso`, `io_uring`, `xdp`, `msgx`, `iocp`, `rio`, `loopback`).

Supported platforms: Linux (`x86_64`, `aarch64`), macOS (`arm64`), and Windows (`x86_64`); see [docs/PLATFORMS.md](docs/PLATFORMS.md).

## Quick start

```c
#include "rocketnet.h"

int main( void )
{
    if ( !rocketnet_init( NULL ) )
        return 1;
    rocketnet_term();
    return 0;
}
```

Start with [examples/hello.c](examples/hello.c) for a complete in-process client/server loop over the loopback transport, and read [examples/README.md](examples/README.md) for the full twelve-example feature series.

## Building

See [BUILDING.md](BUILDING.md). Binaries land in `bin/`, and on Windows in `bin\Release\`; run the unit test suite with `./bin/test`, or with `bin\Release\test.exe` on Windows (PowerShell or cmd.exe).

## Documentation

- [USAGE.md](USAGE.md) — how to drive RocketNet: the five-step frame loop, configuration and environment overrides, zero-copy sending, receiving and releasing views, receipts, counters, stats, and logging.
- [BUILDING.md](BUILDING.md) — Linux, macOS, and Windows builds, dependencies, server resource plans, and test commands.
- [TRANSPORTS.md](TRANSPORTS.md) — runtime-selected server and client UDP transports, candidate lists, and fallback behavior.
- [sdk/rocketnet.h](sdk/rocketnet.h) — the public C99 header and authoritative API contract.
- [examples/README.md](examples/README.md) — the feature-focused example series, starting with `hello`.
- [standard/TRANSPORT.md](standard/TRANSPORT.md) — RN-1, the normative wire transport standard.
- [standard/MESSAGES.md](standard/MESSAGES.md) — RN-2, the normative message and channel standard.
- [standard/RATIONALE.md](standard/RATIONALE.md) — RN-R, the non-normative rationale for the transport and message standards.
- [docs/PLATFORMS.md](docs/PLATFORMS.md) — supported operating systems, architectures, compilers, and transports for v1.0.0.
- [docs/ABI-EVOLUTION.md](docs/ABI-EVOLUTION.md) — shared-library ABI stability and sized configuration evolution.
- [docs/ERRORS-AND-LOGGING.md](docs/ERRORS-AND-LOGGING.md) — status codes, `_ex` entry points, and FFI-safe logging callbacks.
- [docs/RELEASING.md](docs/RELEASING.md) — release checklist, versioning, and packaging verification.
- [SECURITY.md](SECURITY.md) — supported versions and vulnerability reporting policy.
- [CHANGELOG.md](CHANGELOG.md) — release history for v1.0.0.
- [ROADMAP.md](ROADMAP.md) — feature matrix for v1.0.0 and planned future milestones.

Copyright 2026 Más Bandwidth LLC. Private repository. License: [AGPL-3.0](LICENSE).
