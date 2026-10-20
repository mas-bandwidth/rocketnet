# Client I/O

The first deliverable exposes one client object through `sdk/rocketnet.h`.
The caller owns its frame loop and calls `rocketnet_client_update`, drains
`rocketnet_client_receive_messages`, runs simulation, queues messages, and
calls `rocketnet_client_send_packets` last.

By default the portable client transport owns a background I/O thread.
Datagrams cross bounded handoff queues between that thread and the caller
thread. `update` and `send_packets` perform bounded handoff work and do not
wait for the I/O thread. With `step_driven = 1`, the caller performs transport
receive and send work inside those calls and no client I/O thread starts.

Receive authentication, replay checks, packet opening and message admission
must preserve these invariants:

- a buffer is owned by exactly one side of a handoff at a time;
- an opened sequence is replay-checked before its payload is admitted;
- a receive buffer is retired exactly once;
- queue pressure refuses or defers work within fixed capacity and never turns
  a public call into a wait;
- shutdown joins owned background work within the bounded destroy contract.

The executable invariants live in `tla/RNClientIo.tla`,
`tla/RNClientIoDecrypt.tla`, the unit suite, and the functional transport
matrix. Performance profiling and longer saturation campaigns are planned
hardening work.
