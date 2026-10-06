# rocketnet roadmap

Rendered mechanically from the work record's roadmap data by `python3 tools/render-roadmap.py roadmaps/rocketnet.sexp > ROADMAP.md`, run in the private work record. Do not edit this page by hand. The cards themselves, each with its brief, are kept in the record; this page lists only what each release holds.

| release | status | cards | what it holds |
|---|---|---:|---|
| v1.1.0 | next | 24 | The channels work: the message layer with UNRELIABLE and RELIABLE_ORDERED over n channels, up to the client and server objects and the C API, with the connect layer's TLA+ models and the first game integration held as the release gate. |
| v1.2.0 | planned | 5 | The other six reliability types, enabled one at a time on the existing state with the model extended first, and the RakNet-matching C# shim. |
| v1.3.0 | planned | 3 | The scaling work: 1024 and 10k channels, and about 1000 connections with channels on. |
| v1.4.0 | planned | 18 | What TODO.md and TRANSPORT_READINESS.md still list: the public C surface, XDP and driver rungs, the swarm and the long soaks, kernel replay protection, the session layer, flows and broadcast, hardening validation and the release policy. |

## v1.1.0

24 cards in 2 streams. The channels work: the message layer with UNRELIABLE and RELIABLE_ORDERED over n channels, up to the client and server objects and the C API, with the connect layer's TLA+ models and the first game integration held as the release gate.

| stream | cards | landed | ready | waiting | planned | held |
|---|---:|---:|---:|---:|---:|---:|
| rocketnet-channels | 17 | 3 | 3 |  | 10 | 1 |
| rocketnet-tla | 7 | 3 | 4 |  |  |  |

### rocketnet-channels

1. `landed` Remove internal names from docs, comments and scripts (`pro`)
2. `landed` Study RakNet's eight reliabilities and what rocketnet's design must keep open (docs/RAKNET-RELIABILITY.md) (`heavy`)
3. `landed` Design the message layer bottom up (docs/CHANNELS.md): allocator, records, reliable window, channels, blocks, scheduler, endpoint, objects, C API (`heavy`)
4. `ready` Layer 1: one fixed arena per client connection and per server slot, carved at connect, exhaustion a counted refusal (`heavy`)
5. `ready` Layer 2: message records inside a packet, with the zero-copy single-message frame form, a fuzz target and property tests (`heavy`)
6. `ready` Layer 3 safety: the TLA+ model of channels with per-send reliability, written before the reliable code (`frontier`)
7. `planned` Layer 3: the reliable window, late bounded numbering, the sent-packet map, acks and resend, the receiver's window check (`heavy`)
8. `planned` Layer 4: per-channel state and dispatch, UNRELIABLE and RELIABLE_ORDERED enabled, the other six admitted by the state (`heavy`)
9. `planned` Layer 5: blocks, large messages split, queued and reassembled from the arena like yojimbo block messages (`heavy`)
10. `planned` Layer 6: the scheduler with four priorities and the packet builder, small unreliable messages aggregated into packets (`heavy`)
11. `planned` Layer 7 safety: the TLA+ model of the endpoint and the server's slots machine, with the CONFIG announcement (`frontier`)
12. `planned` Layer 7: the endpoint and the slots machine as pure code, the CONFIG announcement (`heavy`)
13. `planned` Layer 8: the client and server objects over the client and server transports, with the zero-copy send (`heavy`)
14. `planned` Layer 9: the C API in sdk/rocketnet.h, real non-mangled symbols, ABI revision and export list (`heavy`)
15. `planned` Soak the message layer at full configured load: no malloc or free, counters asserted while running, a fuzz target per parser (`heavy`)
16. `held` HELD release gate: the first integrating game vendors the SDK and proves the two message types under real conditions (`frontier`)
17. `planned` The sodium re-vendor: the gutted two-file flatten with Ed25519 and sealed boxes, replacing the system package before anything ships (`heavy`)

### rocketnet-tla

1. `landed` Inventory of rocketnet's state machines with the ordered list of TLA+ modules to write (docs/TLA-INVENTORY.md) (`heavy`)
2. `landed` Split the socket and driver tests into the functional suite so the unit suite runs without sockets and is bounded (`pro`)
3. `landed` Link: the shared lossy UDP path model every other model instantiates (`frontier`)
4. `ready` ClientConnection: the client's connection states and every transition the code makes (`frontier`)
5. `ready` ServerConnection: the server's slots and encryption mapping, including the leak the audits suspect (`frontier`)
6. `ready` Security: the connect token lifecycle, the prefilter and keyed filter, the replay window, with an attacker (`frontier`)
7. `ready` FragmentsAndAcks: fragment reassembly, the ack window and burned sequences (`frontier`)

## v1.2.0

5 cards in 1 stream. The other six reliability types, enabled one at a time on the existing state with the model extended first, and the RakNet-matching C# shim.

| stream | cards | landed | ready | waiting | planned | held |
|---|---:|---:|---:|---:|---:|---:|
| reliability-types | 5 |  |  | 1 | 4 |  |

### reliability-types

1. `planned` Enable UNRELIABLE_SEQUENCED: the newest message per channel wins, stale ones dropped, with the model extended first (`heavy`)
2. `planned` Enable RELIABLE: delivered exactly once in any order, with the model extended first (`heavy`)
3. `planned` Enable RELIABLE_SEQUENCED: reliable delivery of the newest message per channel, with the model extended first (`heavy`)
4. `planned` Enable the three WITH_ACK_RECEIPT types: a receipt per send, acked or lost, raised once (`heavy`)
5. `waiting` The RakNet-matching C# shim over the C API, from the target surface given as data (`heavy`)

## v1.3.0

3 cards in 1 stream. The scaling work: 1024 and 10k channels, and about 1000 connections with channels on.

| stream | cards | landed | ready | waiting | planned | held |
|---|---:|---:|---:|---:|---:|---:|
| scaling | 3 |  |  |  | 3 |  |

### scaling

1. `planned` Stress the message layer at 1024 channels: arena size, per-update cost and the formula against the measured (`heavy`)
2. `planned` Stress the message layer at 10k channels, and settle the channel count's type and bounds (`heavy`)
3. `planned` Load-profile about 1000 real connections with channels on, at game-like rates and sizes with churn (`frontier`)

## v1.4.0

18 cards in 1 stream. What TODO.md and TRANSPORT_READINESS.md still list: the public C surface, XDP and driver rungs, the swarm and the long soaks, kernel replay protection, the session layer, flows and broadcast, hardening validation and the release policy.

| stream | cards | landed | ready | waiting | planned | held |
|---|---:|---:|---:|---:|---:|---:|
| remaining | 18 |  |  | 3 | 15 |  |

### remaining

1. `planned` The public C surface: rocketnet_client_transport_t and rocketnet_server_transport_t with functions, real non-mangled symbols, nothing else (`heavy`)
2. `planned` ClientTransport and ServerTransport virtual interfaces: decide the C++ ergonomics on their own merits (`pro`)
3. `planned` XDP: ipv6 in the kernel program and the userspace path (#19) (`heavy`)
4. `planned` XDP: surface the kernel filter counters through the transport counter surface (`heavy`)
5. `planned` Drivers: io_uring, then IOCP (`heavy`)
6. `planned` The socket-level swarm with Go orchestration, against real transports (`heavy`)
7. `planned` XDP test harnesses: packet blaster loads to prove line rate, and simulated clients against the XDP server (`heavy`)
8. `planned` The multi-day soaks and the hours-to-days soak at load, past 2^32 packets on one connection (`heavy`)
9. `planned` Replay protection in the kernel: a read-only high-water map the kernel never advances, sized apart from the userspace window (`frontier`)
10. `waiting` The session layer, a v2 offering: identity-based auth with a backend, the rolling magic and the keyed filter value (`frontier`)
11. `waiting` Flows and broadcast above the transport (step 3 of REQUIREMENTS.md) (`frontier`)
12. `planned` Regression-validate the XDP ownership and retry paths against the 10 Gbps production baseline (`frontier`)
13. `planned` Privileged XDP syscall-failure coverage: its own lab for construction failures (`heavy`)
14. `planned` Sized APIs for compatible struct evolution, binding generation, a lifetime-owning managed layer, structured create errors, FFI-safe logging (`heavy`)
15. `planned` Whole-transport memory estimates and operator budgets that include kernel buffers, thread stacks, allocator overhead and application memory (#155) (`heavy`)
16. `planned` Reproducible release dependencies, the supported-platform policy and the security disclosure and release process (`pro`)
17. `waiting` Independent review of the complete transport repair diff, received, not only requested (`pro`)
18. `planned` Retire TODO.md and TRANSPORT_READINESS.md into the roadmap and the contract docs as their items land (`pro`)
