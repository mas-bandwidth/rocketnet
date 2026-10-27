# RocketNet requirements

RocketNet is a low-level client/server UDP transport and message library for a
single server with up to 1,000 connected players. The public surface is a C ABI
in `sdk/rocketnet.h`; no C++ type crosses it.

## Public integration

- Messages are opaque byte buffers. Serialization and game simulation belong
  to the caller.
- Public calls perform bounded work. Create and the main frame calls are
  intended to be unnoticeable in a game profiler. Client disconnect and server stop request shutdown without waiting for workers.
  Their completion queries must return promptly; destroy requires completion
  and synchronously joins and frees. A stuck worker leaves completion pending.
- Ordinary memory, whether allocated at create or during a documented runtime
  lifecycle, uses the caller allocator. Capacity and failure are bounded and
  reported; no component may hide an allocation outside those hooks.
- The caller supplies monotonic time to update and follows the five-step frame
  order documented in `AGENTS.md` and `USAGE.md`.
- The API reports specific create, lifecycle and configuration refusals through
  `rocketnet_status_t` and exposes stats structs for runtime outcomes.
- Each public config starts with `config_bytes`, set by its default_config
  call, so the struct can grow at its end; it has documented defaults and
  strict validation at create.

## Connection and security

- A client connects with a signed connect token minted by the application's
  backend. The backend and game server share no secret; servers verify the
  backend's public signing key.
- A token fits in one 1,500-byte MTU packet and names one server with one IPv4
  address, one IPv6 address, or one of each.
- A server may serve IPv4, IPv6 or both. The request must arrive on a family and
  advertised address present in the token.
- One active token is bound to one source address and one live connection. One
  client id occupies at most one live server slot.
- Session traffic is authenticated and encrypted with per-connection keys,
  includes replay protection, and provides no property weaker than netcode
  1.02's security baseline.
- Development builds accept the published development keys, which a program
  sets into its server config explicitly, and provide a development token
  helper. A production start requires application-supplied keys.
- Player-hosted servers remain possible because token verification needs a
  backend public key rather than a backend/server shared secret.

## Messages

- The eight `rocketnet_reliability_t` modes cover unreliable, sequenced,
  reliable, ordered and receipt-bearing delivery across up to 32 channels.
- Sending uses reserve, write and finish or abort. Receiving drains one
  endpoint-wide `rocketnet_message_batch_t` queue with no release call.
- Fragmentation and reassembly carry messages up to the configured maximum
  within fixed storage.
- Reliable work is retained until delivery or the reliable timeout. Unreliable
  work may be dropped only at documented bounded resources and is counted.
- Receipt serials report terminal ACK or loss for receipt-bearing messages.
  The outcomes recorded for a connection remain readable after that
  connection ends, until its next update begins. A receipt-bearing send,
  reliable or unreliable, that is still in flight when its connection ends
  gets no terminal outcome.

## Platforms and transports

- Linux, macOS and Windows are supported.
- Runtime transport selection uses `transport`: automatic order, one pinned
  name, or an explicit ordered list. A pin fails loudly when unavailable.
- Linux includes portable UDP plus GSO, io_uring and AF_XDP server transports
  when their build and host requirements are available.
- macOS includes portable and msgx transports. Windows includes portable,
  IOCP and RIO server transports where available.
- Socket transports support multiple server instances distinguished by address
  and port. A transport documents any stronger interface or privilege
  ownership requirement.
- The caller can supply allocation, logging and assertion hooks. Ordinary
  memory uses the caller allocator.

## Configuration, memory planning, and scale qualification

- Neither `rocketnet_*_default_config` nor `rocketnet_*_light_config` is a
  universally suitable preset for every game. Callers size channels, message
  arenas, fragment reassembly slots, reliable windows, per-update packet and
  unreliable budgets, compile-time caps, and runtime memory budgets to match
  their game's concrete tick rate, payload distribution, and player count using
  the tradeoffs and formulas documented in [`docs/LOAD.md`](docs/LOAD.md) and
  [`docs/ENVIRONMENT.md`](docs/ENVIRONMENT.md).
- Callers query deterministic worst-case allocator requirements through
  `rocketnet_client_required_memory` and `rocketnet_server_required_memory`
  (or `LOAD_PLAN_ONLY=1 ./bin/load` and `bin/transport_memory`) and account
  separately for OS and kernel resources outside the user-space allocator
  (thread stacks, AF_XDP page-aligned UMEM and kernel rings, `io_uring` mapped
  rings, and kernel socket buffers).
- The default compile-time `ROCKETNET_MAX_CLIENTS` ceiling (`1000`) and the
  recorded 1,000-client results in
  [`docs/runs/lan-1000-proof.md`](docs/runs/lan-1000-proof.md)
  ([Linux gso and io_uring](docs/runs/lan-1000-proof-linux.md),
  [AF_XDP](docs/runs/lan-1000-proof-xdp.md)) (1,000 clients
  for 600 s, 300 s for AF_XDP, against Linux `gso`, `io_uring`, and AF_XDP
  copy-mode servers and a macOS `msgx` server; minimum per-client rates of
  276–283 kbps) and
  [`docs/QUALIFICATION-v1.0.0-preview-2.md`](docs/QUALIFICATION-v1.0.0-preview-2.md)
  (60-second non-timing correctness rungs at 1–1,000 clients)
  apply only under those records' exact message profile, tick rate, and
  deployment assumptions. Above 1,000 players, callers research, stabilize, and
  qualify their own configuration, memory budget, and host/NIC deployment; no
  arbitrary client count or validated-above-1000 guarantee is provided.
- Operational diagnostics and recovery follow the public status, state, and
  counter contracts in [`docs/ERRORS-AND-LOGGING.md`](docs/ERRORS-AND-LOGGING.md),
  preserving refusal and drop semantics without infinite retry loops, unchecked
  message completion, early context destruction, or forced shutdown.

## First-deliverable boundary

The first deliverable contains the public client/server API, secure connection,
message reliability, runtime transports, examples, short functional gates and
formal models for its bounded state machines. Server subscription flows,
backend-wide identity coordination, rotating keyed prefilters, long soak,
large load and performance profiling are planned work and are not current API
or performance claims.
