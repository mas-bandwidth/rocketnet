# Integrating RocketNet with a coding agent

Use this guide to find the contract you need before editing an integration.
The [documentation index](README.md) maps the full set. All public names,
field defaults, and call contracts come from [sdk/rocketnet.h](../sdk/rocketnet.h).
Do not invent wrapper APIs or infer delivery from a synchronous call sequence.

## What RocketNet is

RocketNet is a client/server UDP transport and message library with a C API.
It moves opaque bytes; the game owns serialization and simulation. Development
builds supply local token helpers. Production clients receive tokens from a
backend service. Capacity and timing claims require the workload-specific
[qualification evidence](TESTING.md).

<a id="prerequisites"></a>

<a id="building-and-installing"></a>

<a id="linking-from-cmake"></a>

## Building and linking on Linux, Windows and macOS

Follow [Building](BUILDING.md) for platform prerequisites, library targets,
and installed or embedded CMake usage. `Release` and `ROCKETNET_DEV` are
independent settings. Compile consumers against the matching public header.

## Before production

Follow [Production integration](PRODUCTION.md). The essential boundaries are:

- Build with `-DROCKETNET_DEV=OFF` and link that production library.
- Keep the backend signing secret on the backend. Give game servers its public
  key, a title protocol id, and a title packet filter; clients get opaque tokens.
- Load deployment settings through documented environment variables or config
  fields. Callbacks, `loopback_hub`, `server_secret_key`, and `config_bytes`
  have no environment form. Do not hard-code production secrets.
- A server generates a key by default, or copies a supplied `server_secret_key`
  at create. Always start from `rocketnet_server_default_config`: create takes
  any nonzero bytes in that field as the key, including uninitialized ones.
  Wipe your own copy of a supplied secret once create returns. Register its public key and actual bound addresses through an
  authenticated application channel. [Keys](KEYS.md) owns the rotation rules.
- Add authentication and authorization to the example backend before deployment.
- Qualify the chosen client count, transport, workload, and host resources.
  The 100-microsecond main-call target is not established at 1000 clients.

## The integration loop

1. Initialize once with `rocketnet_init`; check the result.
2. Fill configs with their default functions. Check create's handle and status,
   then separately check server start and client connect. A successful connect
   begins a handshake; updates determine whether it succeeds.
3. On each endpoint: update, drain receive batches, simulate, queue messages,
   then send packets. Use finite, non-decreasing time in one clock domain.
4. Check every `start_message` pointer. Finish or abort its one reservation
   before update or send. Preflight does not replace the pointer check.
5. Request disconnect/stop, poll completion, then destroy. Keep the object and
   allocator/callback contexts alive until destroy completes. Terminate only
   after all endpoints and loopback hubs have been freed.

The full [integration guide](USAGE.md) defines buffer lifetime, backpressure,
receipts, callback behavior, and reconnect. It is the shared narrative for
human and agent integrations.

### Compilable loopback integration example

[Complete C programs](INTEGRATION-EXAMPLE.md) include a message exchange
and a separate create-only smoke check. `sh tools/docs/check_agents_md.sh all`
compiles and runs them after the libraries are built under `build/`.

## Choosing a transport

Use [Transports](TRANSPORTS.md). `auto` follows a platform/endpoint order;
a supported explicit candidate list is tried as written. A list containing
only known names unavailable on that endpoint falls back to auto with a warning.
Read the granted transport and handle startup refusal; never assume a pin worked.

### Public configuration names and defaults

Use [Configuration](USAGE.md#configuration), [Environment](ENVIRONMENT.md),
and the public header. Precedence at create is defaults, then struct, then
documented environment overrides. Keep planning and create inputs identical.

## The eight channel types

Use [Channels](CHANNELS.md#reliability) and the
[reliability example](examples/reliability.md). Ordering and sequencing
are per channel; reliability does not guarantee delivery after disconnection.

## Connect tokens and security

Use [Sessions](SESSION.md), [Keys](KEYS.md), and [Security](SECURITY.md).
Never put the backend signing secret in a client or game server. Reconnect
with a fresh token after the previous I/O worker has completed.

## Errors and refusals

[Troubleshooting](ERRORS-AND-LOGGING.md) distinguishes immediate status
codes, asynchronous client states, disconnect reasons, and counters. Capture
each call's own result. A refused send is not automatically API misuse.

## Threading rules

One caller thread at a time owns an endpoint; an application handoff must
serialize access. Allocator and logging callbacks may run on I/O threads.
Follow the complete [threading contract](USAGE.md#threads).

## Testing an integration

Run [Hello](examples/hello.md), then the [UDP pair](examples/client_server.md),
then the examples for the features you use. Check eventual connection, message
delivery, and shutdown within bounded deadlines. A single update or fixed sleep
does not establish asynchronous completion. [Testing](TESTING.md) distinguishes
this functional evidence from performance and platform qualification.

### Reviewing implementation changes

For SDK work, use [CLAUDE.md](CLAUDE.md), [contributor build checks](BUILD-REFERENCE.md),
and the [standards](STANDARD.md). Inspect widths and ownership at each boundary.
[Sequence buffers](SEQUENCE-BUFFERS.md) records the container policy and
approved fixed-array index links; do not restate it as a blanket ban on all links.
Preserve wraparound, sparse occupancy, stale-generation, and lifetime checks.

## Common mistakes

- Calling receive once: drain until NULL before the next update.
- Keeping a batch pointer across another receive or update: process or copy first.
- Finishing a refused start: finish only a live reservation.
- Sending before queueing: make send the last frame step.
- Treating DISCONNECTED as worker completion: poll the completion query.
- Assuming a design goal or an older green run qualifies a new deployment.

## Zero-copy messages

Write directly into a successful start reservation. Some fitting unreliable
messages are built in a transport packet; reliable or queued messages retain
storage for later transmission. [Sending](USAGE.md#sending-messages) defines
fallback, promotion, lifetime, and memory-exhaustion behavior. Do not depend on
which storage path a particular reservation takes.

### Why RocketNet works this way

See [Outgoing message construction](OUTGOING-BUILD.md) for the internal
ownership and packet-building design.

### Using `start`, `finish`, and `abort` correctly

See [Sending](USAGE.md#sending-messages) and
[Backpressure](USAGE.md#asking-whether-there-is-room).

### Writing a message in place

See the checked [tutorial excerpt](GETTING-STARTED.md#4-run-the-frame-loop)
and [complete integration example](INTEGRATION-EXAMPLE.md).
