# Buffer handoff decision (2026-10-21)

## Decision

Across RocketNet, every cross-thread bulk handoff must use bounded,
preallocated buffers with distinct producer, ready, and consumer ownership.
Trade planned memory for predictable performance. Publish ownership with a
small mutex-protected pointer or index exchange. An existing correct atomic
ownership transfer may remain where it already satisfies the same ownership,
progress, and no-work-under-lock rules. The mutex protects the exchange only:
copying, scanning, reset, codec work, and socket I/O stay outside its critical
section.

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

Keep frequently walked ownership and per-client metadata compact. Store bulk
packet payloads, held-message storage and other cold data separately, with
stable indices or pointers connecting the two. A hot-array scan must not
stride through large payload-bearing records merely to read a flag or count.
This separation must preserve the published view's lifetime and the exact
allocation plan; memory savings are not a reason to weaken ownership or
reduce the supported client count.

## Applying the decision

New cross-thread bulk paths and redesigns must follow this handoff contract;
the added buffers are an explicit memory-for-performance tradeoff, not an
optional optimization gated on a timing threshold. Keep existing correct atomic
ownership-transfer paths when they satisfy the same safety, progress, and
no-work-under-lock requirements; do not rewrite them solely to introduce a
mutex.

Runtime network I/O runs on background workers: socket reads and writes, completion
handling, receive reposting and send-batch preparation belong to the worker
that owns the relevant socket, queue or ring. The caller fills its owned send
storage and consumes published receive storage. A caller handoff never waits
for a worker or performs bulk work under the exchange mutex. A worker may
retain a full bank and apply bounded backpressure; it must not overwrite a
ready or exposed bank. If the caller's budget ends inside a claimed receive
bank, retain the bank and its cursor for the next call.

This is transport architecture, not a claim that all endpoint work is
off-thread. Protocol state transitions, authentication and packet sealing
remain owned by the endpoint caller unless a separate design explicitly
transfers their state and lifetime. Architecture conformance, functional
correctness and the 100-microsecond timing target are separate validation
claims.

The reviewed Flow/Space main-flip examples are useful precedent for separating
producer and consumer views. That observation does not establish that every
existing worker path follows this decision. This is a required design rule for
cross-thread bulk handoffs, not a claim that existing paths have all been
converted or gated. The asynchronous server-stats implementation remains
subject to its source and platform gates; this decision records the architecture,
not a completion or conformance claim.
