# Protocol state machines and deterministic tests

RocketNet separates protocol state from socket and transport I/O. Protocol
machines receive packet events and time as arguments and keep connection state
in owned structs. This separation makes their decisions testable without a
live network; it does not make every production transition a pure function.

In particular, client connect obtains a random connection salt and ephemeral
keypair. Server request processing obtains a session nonce and ephemeral
keypair, and cookie rotation obtains a new random key. These operations occur
in `sdk/rocketnet_client_protocol.cpp` and `sdk/rocketnet_server_protocol.cpp`,
not only during object construction. Protocol code also logs transitions and
invokes transport key-publication hooks. The platform adapter resolves an
automatic token-clock epoch from wall time; later token checks use that epoch
and the caller's time. See [Security](SECURITY.md#scope-and-security-posture).

The state-machine boundaries include:

- handshake and client-state transitions;
- slot admission and connection reset;
- cookie rotation and token/address history;
- replay-window and acknowledgement state;
- message ordering, sequencing, fragmentation and receipts;
- bounded client I/O and send-handoff ownership.

The [TLA+ models](TLA-INVENTORY.md) describe abstractions that can be driven
without sockets or wall time. Unit tests exercise boundary inputs and actions;
the [deterministic simulation harness](DST.md) controls its clock, randomness
and fault schedule. Functional tests cover composition with real transports.
Model purity and repeatable tests do not establish that the corresponding C++
transition has no external effects.

Keep a new decision in the smallest machine that owns its state. Pass time and
events explicitly, make entropy and callback boundaries visible, and avoid a
second source of protocol truth in an adapter. A transition that is described
as pure must receive every behavior-changing input, including randomness,
through its arguments.
