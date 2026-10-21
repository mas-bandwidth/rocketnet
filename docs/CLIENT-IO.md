# Client I/O

`sdk/rocketnet.h` exposes one client object.
The caller owns its frame loop and calls `rocketnet_client_update`, drains
`rocketnet_client_receive_messages`, runs simulation, queues messages, and
calls `rocketnet_client_send_packets` last.

By default the client transport owns a background I/O thread.
Datagrams cross bounded handoff queues between that thread and the caller
thread. `update` and `send_packets` do bounded handoff work. They can briefly
wait for a lock held by the I/O thread. With `ROCKETNET_CLIENT_STEP_DRIVEN=1`,
the caller performs transport
receive and send work inside those calls and no client I/O thread starts.

Receive authentication, replay checks, packet opening and message admission
must preserve these invariants:

- a buffer is owned by exactly one side of a handoff at a time;
- an opened sequence is replay-checked before its payload is admitted;
- a receive buffer is retired exactly once;
- queue pressure refuses or defers work within fixed capacity and never waits
  on a full queue;
- disconnect requests retirement without waiting; disconnected reports worker
  quiescence without joining, draining, calling callbacks, or freeing;
- destroy requires disconnected, asserts it, then synchronously joins and
  releases storage. A worker stuck in a platform call keeps completion false;
- internal client and server transports use the same stop, stopped, destroy
  order. Their stop requests and completion checks never wait for workers.

The optional `time_callback` in `sdk/rocketnet_testing.h` supplies receive-stamp time;
it is not the game clock passed to update. It may run on the I/O thread and on
the caller's thread when an explicitly step-driven client is updated. Stop
and completion queries do not read that clock. Keep its function and context
valid through destroy, and make it safe for its calling threads. The game
clock supplied to time-taking API calls must be finite, nondecreasing, and
consistent across calls.

The executable invariants live in `tla/RNClientIo.tla`,
`tla/RNClientIoDecrypt.tla`, the unit suite, and the functional transport
matrix. Performance profiling and longer saturation campaigns are planned
hardening work.

A step-driven test advances shutdown explicitly with update (or its I/O-step
seam), then checks completion. Checking alone never drives the backend. A game
using the normal threaded mode keeps its event loop running while it polls.
Keep the allocator context, callback context, and transport memory alive through
completion and synchronous destroy; requesting stop does not transfer ownership.
