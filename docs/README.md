# RocketNet documentation

Choose a path by what you want to do. The introductory path is build → run a
message exchange → integrate a frame loop → connect a backend → qualify a deployment.

## Learn and integrate

| Goal | Guide |
| --- | --- |
| First successful connection and message | [Getting started](GETTING-STARTED.md) |
| Copy a complete C integration | [Integration example](INTEGRATION-EXAMPLE.md) |
| Install or embed the SDK | [Building](BUILDING.md) |
| Own the frame loop, buffers, and endpoint lifetime | [Integration guide](USAGE.md) |
| Understand channels, delivery, and receipts | [Messages and channels](CHANNELS.md), [reliability example](examples/reliability.md) |
| Understand connection identity | [Sessions](SESSION.md), [Keys](KEYS.md) |
| Replace local development tokens | [Production integration](PRODUCTION.md), [backend service examples](../examples/backend/README.md) |
| Work with an AI coding agent | [Agent integration guide](AGENTS.md) |
| Explore one feature at a time | [Example catalogue](../examples/README.md) |

## Configure, deploy, and diagnose

| Goal | Reference |
| --- | --- |
| Choose a transport | [Transport selection](TRANSPORTS.md) |
| Prepare a host | [Linux](LINUX.md), [macOS](MACOS.md), [Windows](WINDOWS.md), [GameLift](GAMELIFT.md) |
| Provision native server transports or XDP | [Deployment](DEPLOYING.md) |
| Set environment values | [Environment inventory](ENVIRONMENT.md), [config fields](USAGE.md#configuration) |
| Budget memory, clients, and host resources | [Memory and capacity](MEMORY.md), [detailed storage model](LOAD.md) |
| Diagnose a failure | [Troubleshooting, errors, and logging](ERRORS-AND-LOGGING.md) |
| Evaluate security support and report an issue | [Security](SECURITY.md) |

## API, protocols, and engineering

| Question | Authority or design document |
| --- | --- |
| Exact C API names, types, defaults, and call contracts | [Public header](../sdk/rocketnet.h) |
| Wire and message protocol requirements | [Standards index](STANDARD.md) |
| Public layout and binary compatibility | [ABI evolution](ABI-EVOLUTION.md) |
| Transport implementation interfaces | [Transport internals](TRANSPORT-INTERNALS.md), [client I/O](CLIENT-IO.md) |
| Outgoing message ownership and allocation | [Outgoing build](OUTGOING-BUILD.md), [sequence buffers](SEQUENCE-BUFFERS.md) |
| Build rules, test tiers, sanitizers, and toolchains | [Contributor build reference](BUILD-REFERENCE.md), [test scaffolding](TEST-SCAFFOLDING.md) |
| Verification models and their execution inventory | [TLA inventory](TLA-INVENTORY.md), [model guide](../proofs/tla/README.md) |
| Development conventions | [Development notes](CLAUDE.md), [style](STYLE.md) |

## Qualification and history

Start with [Testing and qualification](TESTING.md). The [release process](RELEASING.md#default-ci-requirements)
uses the release commit's green CI run as qualification. The index explains
what each test tier establishes and labels older qualification records as history. [Release notes](RELEASE-NOTES-v1.0.0-preview-4.md)
describe migrations; [Hardening](HARDENING.md) explains the verification methods
and their limits. A plan, model, host-shim test, or successful build is
not interchangeable with an executed live-transport or performance result.

[Run records](runs/) and [audit records](audits/) retain their tested commit,
date, platform, and scope. They are historical evidence, not continuously
updated API references. Their original outcomes remain intact as the guides evolve.

## Maintaining these documents

Keep one owner for each contract: the public header for C behavior, the
standards for wire behavior, Keys for key ownership, and the environment
inventory for deployment variables. Tutorial prose should link to those
owners rather than duplicate full enums or config declarations. A contradiction
needs reconciliation with the authority; a link alone does not resolve it.

Keep examples executable. Mark complete programs for the compile/run checker;
cite source ranges for tutorial excerpts and check them when examples change.
Keep a usable first-success path for every supported platform. Preserve old
deep links when moving a section, and label historical evidence by scope.

From the repository root, the documentation checks are:

```sh
python3 tools/docs/check_links.py
python3 tools/docs/check_getting_started.py
sh tools/docs/check_agents_md.sh all
```

The compile/run check requires built `rocketnet` and `rocketnet_dev` libraries
under `build/`. These checks validate documentation structure and examples;
they do not requalify all platforms or performance. The CI documentation job
also builds the generated API reference and checks for undocumented symbols.
