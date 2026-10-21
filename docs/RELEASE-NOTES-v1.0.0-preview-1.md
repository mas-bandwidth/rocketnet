# RocketNet v1.0.0-preview-1

RocketNet is a client/server UDP library for multiplayer games. It provides
a C API that can be called from C or C++. Your game supplies message bytes,
drives the frame loop and decides what those messages mean.

## What is in this preview

- Encrypted connections established with backend-signed connect tokens.
- Reliable and unreliable messages, with ordering, sequencing and optional
  delivery receipts across independent channels.
- Message fragmentation and reassembly within configured limits.
- In-place message writes through start, finish and abort calls.
- Batch receives, connection state, traffic statistics and send budgets.
- A development library with token creation and a network simulator for
  latency, jitter, loss and duplication.
- Static and shared libraries, CMake package support and example programs.

The game loop has five steps. Update each endpoint, drain received messages,
simulate the game, write outgoing messages, then send packets. Consume or
copy each received batch before the next receive or update invalidates it.

Initialize each config with its default-config function before setting the
fields your game needs. Check status results and connection state. The
public header describes valid ranges, defaults and ownership.

## Connecting players

Your backend authenticates a player and returns an opaque connect-token
string. The client passes that string to RocketNet. Keep the backend's
signing secret on the backend.

A server generates a keypair when it is created. Register its public key
and reachable addresses with your backend before issuing tokens for it.
Development builds provide a published backend key and a token helper for
local testing. Those development credentials are not production credentials.

## Platforms and transports

The desktop targets are Linux, macOS and Windows. The source includes these
transports. Availability depends on the build and the host configuration.

| Platform | Network transports |
| --- | --- |
| Linux | portable UDP, GSO, io_uring, AF_XDP |
| macOS | portable UDP, msgx |
| Windows | portable UDP, IOCP, RIO |

An in-process loopback transport is available for local integration tests.
AF_XDP requires host provisioning and separate privileged validation. A
successful ordinary test run does not establish AF_XDP attachment or
line-rate throughput. See the [platform support policy](PLATFORMS.md) and
[transport guide](../TRANSPORTS.md) for the supported configurations.

## Known issues and remaining work

This is a preview for integration and feedback. The final deliverable still
includes hardening, longer testing and performance work.

- Frame-time targets are not yet established under a 1000-client game load.
  Slot scans, memory resets and packet processing can cause frame spikes.
- Client disconnect and destruction wait for the network thread to exit.
  No shutdown time bound is promised.
- Large server configurations can require substantial memory. Size the
  endpoint blocks before creating them and choose limits for your game.
- Long soaks, sustained per-transport load and privileged AF_XDP throughput
  remain separate validation work.
- The example soak has a known failure when many clients disconnect at once.
  Reconnect and UDP-loopback functional cases have also shown timing failures
  under heavy host load.
- On macOS, the portable dual-stack connection test can intermittently fail
  to connect before its deadline. The failure remains a known issue even
  when a repeat run passes.
- The full macOS functional suite exceeds the 120-second gate limit. The
  timed-out runs do not establish a full-suite pass.
- Protocol refusal paths, token-claim behavior and low-rate reliable-message
  timeouts still have follow-up work recorded in the roadmap.

The [Known issues table](../ROADMAP.md#known-issues-addressed-in-the-final-deliverable)
records the individual issues and the planned fixes. Consult it before
choosing a deployment configuration.

## Getting started

Use [BUILDING.md](../BUILDING.md) to build and link the library, the
[examples](../examples/README.md) to try the frame loop, and
[rocketnet.h](../sdk/rocketnet.h) for the API contract.

The release label is `v1.0.0-preview-1`. CMake's numeric package version is
`1.0.0`; its exported version string carries the full preview label.
