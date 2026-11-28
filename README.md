[![CI](https://github.com/mas-bandwidth/rocketnet/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/mas-bandwidth/rocketnet/actions/workflows/ci.yml)

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/rocketnet-logo-dark.png">
  <source media="(prefers-color-scheme: light)" srcset="assets/rocketnet-logo-light.png">
  <img alt="RocketNet" src="assets/rocketnet-logo-light.png" width="640">
</picture>

**rocketnet** is a client/server UDP transport and message library for games with thousands of players.

Want to make multiplayer games with thousands of players and bandwidth at line rate? Now you can! [Details here](docs/PERFORMANCE.md).

## Quick start

[Start here](docs/GETTING-STARTED.md). We take you through
building the library, sending a message over an in-memory connection, and
running a client and server in separate processes over UDP.

## Integrate with your game

| Task | Documentation |
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

**rocketnet** is dual-licensed: `AGPL-3.0-or-later OR LicenseRef-MBSL-1.0`.

- **[GNU Affero General Public License v3.0](LICENSE)** or any later version. Free.
- **[Más Bandwidth Source License](MBSL.md)** (MBSL-1.0). The commercial license, for use that does not comply with the AGPL.

Subscribe at **https://patreon.com/masbandwidth**. The [license](MBSL.md) governs.

- USD $1 a month, or USD $100 if your group's Revenue, as defined in section 1.8, exceeded USD $1,000,000 in the preceding 12 months. Within 30 days of your first USD $100 payment, send us your legal name by email or Patreon message.
- One subscription covers all MBSL-licensed code from Mas Bandwidth LLC, for you and every entity you control.
- Pay from the first month you use rocketnet. Evaluation and development count.
- Missed months are paid by invoice: email glenn@mas-bandwidth.com. An unpaid month is not licensed under the MBSL.
- A publisher needs no subscription of its own for a product licensed to its developer. A developer the publisher does not control needs its own.
- You may not bundle rocketnet for others to build with. Anyone who builds with rocketnet needs their own subscription, or must comply with the AGPL, however they received it.
- Your product's credits must carry:

      Mas Bandwidth LLC
          rocketnet        Glenn Fiedler and Rowan Claude

A one-time payment of USD $2,500 covers one named product permanently from the month of payment. Earlier months of that product's use must still be paid. Contact glenn@mas-bandwidth.com.

The XDP program in `sdk/xdp/` keeps its own license, the GPL. It runs inside the kernel and is not part of the MBSL.

See [Credits](docs/CREDITS.md) for rocketnet's creator and sponsor, and
[Security](docs/SECURITY.md) for reporting a vulnerability.
