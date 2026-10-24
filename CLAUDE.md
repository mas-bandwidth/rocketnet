# RocketNet development notes

RocketNet is a low-level client/server UDP transport and message library for up
to 1,000 connected players. The public surface is the C API in
`sdk/rocketnet.h`; [AGENTS.md](AGENTS.md) is the integration guide and
[STANDARD.md](STANDARD.md) points to the normative wire documents.
A default build is a development build (`ROCKETNET_DEV` is 1); before any
deploy, follow [AGENTS.md, "Before production"](AGENTS.md#before-production).

## Boundaries

Messages are opaque byte buffers. RocketNet does not serialize application
state or run game simulation. Protocol packets are byte-aligned little-endian
layouts with no schema or bit-packing dependency.

The public client and server objects own connection, security, message and
transport state. Public calls do bounded work and do not block. All regular
memory uses the caller allocator, including documented runtime allocation;
capacity, failure and lifetime remain explicit. Background transport threads communicate through
bounded handoffs and are invisible to the public frame order.

## Layers

1. `rocketnet.{h,cpp}`: process init, allocator, logging and assertions.
2. Leaf utilities: address, hash, base64 and bounded containers.
3. Platform: threads, mutexes, clocks and sockets; system headers stay here.
4. Transport protocol: connect token, cookie, handshake, session keys, replay,
   acknowledgements and encrypted packets.
5. Message layer: reliability, ordering, sequencing, fragmentation, receipts
   and the endpoint-wide receive queue.
6. Runtime transports: portable plus platform-specific server/client I/O behind
   the common transport seam.
7. Public C API: client/server lifecycle and the five-step game frame loop.

Transport implementations satisfy common server and client interfaces. Public
call sites do not branch on a platform transport. Selectable carriers are
called transports; “driver” remains valid only for operating-system/NIC drivers
and intentionally retained physical source paths.

## Security

The cryptographic implementation is the vendored sodium subset in
`sodium/sodium.c` and `sodium/sodium.h`. The backend signs connect tokens with
Ed25519; token private data is sealed to the server's X25519 key. Each
connection derives fresh session keys. Authenticated packet headers are
associated data, sequence nonces are little-endian, and replay checks happen
before packet contents are admitted. Secret lifetimes end with explicit wipe.

Development helpers use published development keys only when the application
requests the development config. Production server start requires supplied
keys. Never weaken the production handshake to make an example convenient.

## Code rules

- Keep the public header C-compatible and keep all exports in the ABI/export
  checks.
- Preserve one source of truth for constants, config defaults, environment
  names and status values.
- Refuse invalid configuration; do not clamp or silently fall back from a pin.
- Preserve bounded capacity and overflow-safe size planning before allocation.
- Use `rocketnet_malloc` and `rocketnet_free` for every ordinary allocation,
  including runtime allocation. Document which call allocates and frees it.
- Pass time into state machines; do not read wall time inside protocol logic.
- Keep packet parsing bounded and validate every remotely controlled length and
  index before use.
- Keep per-connection reset complete: no payload, replay, ACK, message,
  receipt or secret state may cross a slot reuse.
- Finish or abort every `start_message` reservation before update or send.
- Drain receive batches before the next receive call or update invalidates
  their data pointers.

## Verification

Use the smallest gate that exercises a change, then the required platform and
ABI checks for its scope. Unit cases use no real time or sockets unless the
subject is a transport boundary. Functional cases use deterministic caller
time and explicit transport pins. TLA+ models state bounded protocol and
handoff invariants; `tla/RUNS.tsv` is their execution inventory.

Privileged XDP verification runs only through the documented Linux VM and bench
procedure. Long soak, large load, multi-host throughput and profiler campaigns
are planned hardening work and are not first-deliverable claims.
