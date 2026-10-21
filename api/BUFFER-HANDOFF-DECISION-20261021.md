# Buffer handoff decision (2026-10-21)

## Decision

Across RocketNet, prefer a bounded, preallocated buffer handoff when moving
bulk work between producer, ready, and consumer stages. Keep those ownership
roles in distinct buffers. Trade planned memory for predictable performance and
use a small mutex-protected pointer or index exchange to publish ownership.
The mutex protects the exchange only: copying, scanning, reset, codec work, and
socket I/O stay outside its critical section.

A producer or main thread must not wait for a worker to release a buffer. A
published view remains stable until its documented release/ownership transfer.
Full queues apply FIFO backpressure: refuse or defer new work without
rewriting buffers that are still owned or ready. Never overwrite an in-flight
buffer to make room. Shutdown must establish worker completion and join before
any shared buffer or synchronization object is freed.

Every buffer and queue belongs in the exact create-time memory plan, including
alignment and platform-owned storage where it can be accounted for. Capacity,
refusal behavior, ownership transitions, stop proof, and memory sizing are
part of the design contract, not later implementation details.

## Applying the decision

Prefer this handoff for new cross-thread bulk paths and when redesigning a path
whose measured frame cost justifies the added buffers. Keep existing correct
atomic ownership-transfer paths where they already satisfy the same safety and
progress requirements; do not rewrite them solely to introduce a mutex.

The reviewed Flow/Space main-flip examples are useful precedent for separating
producer and consumer views. That observation does not establish that every
existing worker path follows this decision. In particular, the asynchronous
server-stats implementation remains subject to its source and platform gates;
this decision records the architecture, not a completion or conformance claim.
