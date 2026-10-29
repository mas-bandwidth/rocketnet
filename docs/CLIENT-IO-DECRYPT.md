# Client receive authentication

Client receive work authenticates and opens each datagram before the message
layer can observe it. The transport owns fixed receive buffers and transfers
them through bounded queues. Each accepted packet follows this order:

1. validate its fixed header and connection mapping;
2. authenticate and open it;
3. apply the replay-window decision;
4. append its payload and acknowledgement data to the bounded update batch;
5. retire the receive buffer exactly once.

A packet that fails any check produces no application message. Queue pressure
cannot expose a partially opened packet or reuse storage that another stage
still owns. The caller sees admitted messages only through the endpoint-wide
`rocketnet_message_batch_t` queue, whose pointers remain valid until the next
receive call or update on that endpoint.

`proofs/tla/RNClientIoDecrypt.tla` states the ownership, replay and retirement
invariants. Unit and functional tests exercise the executable boundary.
Throughput profiling and off-thread tuning are planned hardening work.
