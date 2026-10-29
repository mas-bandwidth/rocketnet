# RocketNet

RocketNet is a client/server UDP transport and message library with a C API.
It connects clients using backend-issued tokens, authenticates and encrypts
traffic, and delivers opaque messages with a choice of reliability modes.
Your application owns serialization, simulation, and the frame loop.

## Quick start

Start with [Getting started](docs/GETTING-STARTED.md). It takes you through
building the library, sending a message over an in-memory connection, and
running a client and server in separate processes over UDP.

With the [platform prerequisites](docs/BUILDING.md#prerequisites) installed, from
the repository root on Linux or macOS:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DROCKETNET_DEV=ON
cmake --build build --target example_hello --parallel 4
./bin/example_hello
```

On Windows, use the [Windows build commands](docs/BUILDING.md#windows).
The example prints `server received: hello from the client`, then `PASS`
after reconnect and cleanup checks. It needs no socket or backend service.

<a id="documentation"></a>

## Connect your game

| Task | Start here |
| --- | --- |
| Build, install, or embed the SDK | [Building](docs/BUILDING.md) |
| Update, receive, simulate, queue, and send each frame | [Integration guide](docs/USAGE.md#frame-loop) |
| Understand tokens and key ownership | [Sessions](docs/SESSION.md), [Keys](docs/KEYS.md) |
| Replace development tokens with a backend | [Production integration](docs/PRODUCTION.md) |
| Choose and deploy a transport | [Transports](docs/TRANSPORTS.md) and its platform guides |
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

## Preview status

RocketNet is a preview. Read the [preview-4 release notes](docs/RELEASE-NOTES-v1.0.0-preview-4.md)
for migration requirements and [qualification index](docs/TESTING.md) for the
scope and gaps in the recorded evidence.

- Main frame calls target less than 100 microseconds in Release builds.
  Measured server calls exceed that budget at larger client counts;
  1000-client performance is not qualified. Create may take longer.
- Capacity and memory must be planned for the selected configuration.
  Large configurations can spike in memory during shutdown; the 1000-client
  example soak can run out of memory on Linux.
- Disconnect and stop request shutdown asynchronously. Keep the object and
  callback/allocator contexts alive, poll `rocketnet_client_disconnected` or
  `rocketnet_server_stopped`, then destroy. See [lifecycle](docs/USAGE.md#connection-lifecycle).

Recorded 1000-client runs are workload-specific evidence. They do not promise
that any 1000-player game meets its latency or memory budget. The
[capacity guide](docs/MEMORY.md#qualification-and-scale) explains their limits;
the [roadmap](docs/ROADMAP.md) records remaining work.

## What the tests establish

Unit tests, live transport tests, performance runs, and model checks answer
different questions. In particular, a portable XDP host-shim test exercises
filter or frame-codec logic without loading a kernel program, binding AF_XDP,
using a NIC, or measuring throughput. Live XDP qualification must identify
copy or zero-copy mode and the host/NIC configuration. Use the
[testing guide](docs/TESTING.md) to find the evidence for the property you need.

## Repository layout

| Directory | Contents |
| --- | --- |
| `sdk/` | Library sources, including the XDP program in `sdk/xdp/` |
| `examples/` | Programs demonstrating the public API |
| `tests/` | Unit and functional tests, client/server harnesses, load and soak tools |
| `benchmarks/` | Transport and instruction-count measurement harnesses |
| `proofs/` | Formal models and proofs, grouped under `tla/` and `cbmc/` |
| `standard/` | Normative protocol documents, rationale, and checked constant pins |
| `docs/` | Integration guides, engineering notes, decisions, and recorded evidence |
| `tools/` | Developer utilities and repository checks |
