# Client I/O

`sdk/rocketnet.h` exposes one client object.
The caller owns its frame loop and calls `rocketnet_client_update`, drains
`rocketnet_client_receive_messages`, runs simulation, queues messages, and
calls `rocketnet_client_send_packets` last.

By default the client transport owns a background I/O thread that does every
socket send and receive. Datagrams cross between that thread and the caller
thread in preallocated buffers: three send rings and three receive buffers,
each owned by one side at a time (`sdk/rocketnet_buffer_handoff.h`,
`tla/RNBufferHandoff.tla`). Only buffer indices move, under a mutex held for
that exchange alone. `update` and `send_packets` only try that mutex: when the
I/O thread holds it, the exchange waits for the next call, so they never wait
on the I/O thread. Sends then leave with the next hand-off
(`CLIENT_SEND_DEFERRED`) and received datagrams with the next update. With
`ROCKETNET_CLIENT_STEP_DRIVEN=1`,
the caller performs transport
receive and send work inside those calls and no client I/O thread starts.

The BASE receive path uses the same ownership queues and reserves one buffer
for the producer. Up to N-1 batches may be READY. Each update takes all oldest
READY batches that fit its 1024-packet budget, processes them outside the mutex,
and returns their indices in the same update. A busy return keeps those cleared
buffers caller-owned until the next successful exchange; no packet is processed
twice or overwritten. Batches beyond the budget remain READY in order.

Stop transfers the development link simulator to the worker using the quit
publication. The worker drains delayed packets before reporting completion;
step-driven transports drain in their explicit shutdown step. Stop and polling
perform no backend work, and reconfiguration after stop is refused.

Custom BASE drivers without wait/wake retain caller-thread per-packet sends;
this compatibility path does not provide the built-in send offload guarantee.
Built-in loopback uses the same I/O core and bounded worker-side idle polling.
The optional development link simulator is also an explicit exception to the
built-in worker-offload guarantee: while running, its frame work and backend
sends remain caller-owned. Its shutdown drain transfers to the worker as
described above. This exception is development-only and does not change the
normal built-in transport handoff contract.

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

The executable invariants live in `tla/RNBufferHandoff.tla`,
`tla/RNClientIo.tla`, `tla/RNClientIoDecrypt.tla`, the unit suite, and the
functional transport matrix. Performance profiling and longer saturation campaigns are planned
hardening work.

A step-driven test advances shutdown explicitly with update (or its I/O-step
seam), then checks completion. Checking alone never drives the backend. A game
using the normal threaded mode keeps its event loop running while it polls.
Keep the allocator context, callback context, and transport memory alive through
completion and synchronous destroy; requesting stop does not transfer ownership.

The development client link simulator belongs to the caller during normal
updates. Stop publishes the quit request and transfers its queue to the worker;
the worker drains surviving packets, including delayed disconnect copies,
before publishing stopped. Stop itself does not send. Explicit step-driven
transports perform this drain in their shutdown step. Simulator reconfiguration
is refused after stop. `test_client_simulator_stop_defers_backend_send` and
`test_client_legacy_receive_defers_contended_handoffs` exercise these boundaries
without threads, sleeps, or wall-clock deadlines.

## The overlapped send

On Windows the portable client's I/O thread posts its sends instead of
calling a blocking `sendto`, so the kernel never holds the I/O thread inside
a send (roadmap row 223; tla/RNClientIo.tla `IoSend`, `Complete`,
`IoFinishRing`, `NoFreeBeforeComplete`):

- **The interface.** `rocketnet_client_transport_overlapped_t`
  (`sdk/rocketnet_driver.h:192`) has three calls, `post_send`, `reap_sends`
  and `cancel_sends`. It is not a member of the operations table, so every
  table keeps its shape; `rocketnet_client_transport_io_overlapped`
  (`sdk/rocketnet_driver_builtin.cpp:972`) returns it for the portable client
  on Windows and NULL for every other backend, which keeps the synchronous
  `send` and `send_batch`.
- **Post.** The I/O step takes a ready ring and posts every live slot
  (`rocketnet_client_io_post_ring`, `sdk/rocketnet_client_io.h:510`): one
  `WSASendTo` (`WSASend` on the connected socket) with its own `WSAOVERLAPPED`
  record, which returns 0 or `WSA_IO_PENDING` without waiting
  (`rocketnet_platform_wake_post_send`, `sdk/rocketnet_platform.cpp:5247`).
  The ring joins the posted rings in post order with its post count.
- **Many rings in flight.** The step keeps taking ready rings and posting them
  while earlier ones are outstanding (`rocketnet_client_io_send_overlapped`,
  `sdk/rocketnet_client_io.h:578`), so one slow completion never holds back
  the ring behind it. The earlier prototype held one ring at a time: the ring
  sat Sending until its last completion, the game thread's next hand-off found
  no free ring, and the client refused payloads.
- **Reap in post order.** `reap_sends` returns how many of the oldest posts
  have completed and stops at the first one still pending
  (`rocketnet_platform_wake_reap_sends`, `sdk/rocketnet_platform.cpp:5312`);
  `rocketnet_client_io_reap` (`sdk/rocketnet_client_io.h:539`) takes them from
  the oldest posted ring first, and a ring goes back to the main thread only
  once none of its posts is outstanding. An error completion is counted in
  `SEND_FAILED`.
- **Preallocated.** The records, three rings of 256 slots
  (`ROCKETNET_CLIENT_TRANSPORT_MAX_POSTED_SENDS`), are allocated once when the
  client is created (`rocketnet_platform_wake_reserve_sends`,
  `sdk/rocketnet_platform.cpp:5217`); nothing is allocated at runtime.
- **The wait.** While a post is outstanding its completion event joins the
  wake's `WaitForMultipleObjects` set, so a completion ends the I/O thread's
  wait like the wake and the socket do (`IoWake`). No new thread, pool or
  mutex: completions are read on the I/O thread at a point the step chooses.
- **Stop and destroy.** After stop the step posts every ready ring and stop's
  final ring, cancels the posts still pending once (`cancel_sends`,
  `CancelIoEx`), and exits only when every completion has been reaped, so no
  buffer is released while the kernel may read it. Destroy also cancels and
  waits for any post still outstanding before it frees the records and closes
  the socket. A cancelled post is a dropped datagram, as a full send buffer is
  for the synchronous send.

`test_client_io_overlapped_ring_sent_on_completion`
(`tests/test_unit_client_io_transport.cpp`) pins it on the fake transport: a
posted ring is not Sent until its completion is reaped, a second ring is
posted behind an outstanding one, the oldest ring is released first, and stop
cancels the pending posts and exits only after reaping them. The measurement
across the LAN is docs/QUALIFICATION-windows-client-send.md.
