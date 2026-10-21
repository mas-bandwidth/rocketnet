# Pure protocol machines

RocketNet keeps protocol decisions separate from transport I/O. A pure machine
stores its state in an owned struct, receives events and time as arguments,
and returns actions or state changes. Its transition functions do not read a
clock, allocate memory, perform I/O, or consult behavior-changing global
state. Cryptographic primitives are pure when every key, nonce and input is an
argument.

Construction may allocate fixed storage and obtain entropy before transitions
begin. Logging and counters may observe a transition but must not influence
its result. Transport adapters execute the actions a pure machine produces.

The boundaries in this release are:

- handshake and client-state transitions;
- slot admission and connection reset;
- cookie rotation and token/address history;
- replay-window and acknowledgement state;
- message ordering, sequencing, fragmentation and receipts;
- bounded client I/O and send-handoff ownership.

The matching TLA+ modules can be driven without sockets or wall time. Unit
tests compare boundary inputs and actions, while functional tests cover their
composition with transports. New transition code belongs inside the smallest
machine that owns its state; an adapter may translate events but must not
create a second source of protocol truth.
