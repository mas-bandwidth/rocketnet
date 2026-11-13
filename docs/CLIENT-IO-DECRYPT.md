# Client receive authentication

Client receive work authenticates and opens each datagram before the message
layer can observe it. The transport owns fixed receive buffers and transfers
them through bounded queues. Each accepted packet follows this order:

1. validate its fixed header and connection mapping;
2. authenticate and open it;
3. apply the replay-window decision;
4. append its payload and acknowledgement data to the bounded update batch;
5. retire the receive buffer exactly once.

A session datagram below its type's wire floor, or with sequence zero, is
refused before authentication. The I/O thread opens a keep-alive as soon as
it has the live session keys, but a payload only once the client is
connected: until update stores the payloads word on entering connected, a
payload passes the I/O thread unopened and unmarked, and update counts it
wrong state before any open (TRANSPORT.md t11.3). The I/O thread's replay
window marks only sequences update accepts or has accepted. The store is relaxed, so
the I/O thread may still read 0 for a while after it: a payload it passes raw
then is opened and accepted by update alone, in a window after each connect
that the code bounds by no count of payloads, only by the store becoming
visible; a later copy of it opens on the I/O
thread and update refuses it as already received.

A packet that fails any check produces no application message. Queue pressure
cannot expose a partially opened packet or reuse storage that another stage
still owns. The caller sees admitted messages only through the endpoint-wide
`rocketnet_message_batch_t` queue, whose pointers remain valid until the next
receive call or update on that endpoint.

`proofs/tla/RNClientIoDecrypt.tla` states the ownership, replay and retirement
invariants. Unit and functional tests exercise the executable boundary.
Throughput profiling and off-thread tuning are planned hardening work.
