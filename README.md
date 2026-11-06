[![CI](https://github.com/mas-bandwidth/rocketnet/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/mas-bandwidth/rocketnet/actions/workflows/ci.yml)

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/rocketnet-logo-dark.png">
  <source media="(prefers-color-scheme: light)" srcset="assets/rocketnet-logo-light.png">
  <img alt="RocketNet" src="assets/rocketnet-logo-light.png" width="640">
</picture>

**rocketnet** is a client/server UDP transport and message library for games with thousands of players.

## Quick start

Start with [Getting started](docs/GETTING-STARTED.md). It takes you through
building the library, sending a message over an in-memory connection, and
running a client and server in separate processes over UDP.

With the [platform prerequisites](docs/BUILDING.md#prerequisites) installed, from
the repository root on Linux or macOS:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DROCKETNET_DEV=ON -DROCKETNET_BUILD_EXAMPLES=ON
cmake --build build --target example_hello --parallel 4
./bin/example_hello
```

On Windows, use the [Windows build commands](docs/BUILDING.md#windows).
The example prints `server received: hello from the client`, then `PASS`
after a second client connection and cleanup checks. It needs no socket or
backend service.

<a id="documentation"></a>

## Connect your game

| Task | Start here |
| --- | --- |
| Build, install, or embed the SDK | [Building](docs/BUILDING.md) |
| Update, receive, simulate, queue, and send each frame | [Integration guide](docs/USAGE.md#frame-loop) |
| Understand tokens and key ownership | [Sessions](docs/SESSION.md), [Keys](docs/KEYS.md) |
| Replace development tokens with a backend | [Production integration](docs/PRODUCTION.md) |
| Choose and deploy a transport | [Transports](docs/TRANSPORTS.md) |
| Size buffers, capacity, and host resources | [Memory and capacity](docs/MEMORY.md) |
| Diagnose a refused call, missing message, or shutdown wait | [Troubleshooting](docs/ERRORS-AND-LOGGING.md) |
| Find reference, engineering, and qualification documents | [Documentation index](docs/README.md) |

For a complete C program, see [the integration example](docs/INTEGRATION-EXAMPLE.md).
For an AI coding agent, start with [AGENTS.md](docs/AGENTS.md). The
[public header](sdk/rocketnet.h) defines the C API; [STANDARD.md](docs/STANDARD.md)
links to the normative wire and message protocols.

## Building

The default build is for development. Before deployment, build with
`-DROCKETNET_DEV=OFF`, link the resulting production library, and supply your
title's backend public key, protocol id, and packet filter. Production builds
refuse development credentials and the test packet filter. Follow the
[production integration guide](docs/PRODUCTION.md); changing a build flag
alone does not provide a production backend.

## Repository layout

| Directory | Contents |
| --- | --- |
| `sdk/` | Library sources, including the XDP program in `sdk/xdp/` |
| `examples/` | Programs demonstrating the public API |
| `tests/` | Unit and functional tests, fuzzing, client/server harnesses, load and soak tools |
| `benchmarks/` | Transport and instruction-count measurement harnesses |
| `proofs/` | Formal models and proofs, grouped under `tla/` and `cbmc/` |
| `standard/` | Normative protocol documents, rationale, and checked constant pins |
| `docs/` | Integration guides, engineering notes, decisions, and recorded evidence |
| `tools/` | Developer utilities and repository checks |

## License and credits

**rocketnet** is licensed under the [GNU Affero General Public License v3.0](LICENSE).

See [Credits](docs/CREDITS.md) for its creator and sponsor, and
[Security](docs/SECURITY.md) for reporting a vulnerability.
