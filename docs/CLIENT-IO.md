# Client I/O

`sdk/rocketnet.h` exposes one client object.
The caller owns its frame loop and calls `rocketnet_client_update`, drains
`rocketnet_client_receive_messages`, runs simulation, queues messages, and
calls `rocketnet_client_send_packets` last.

By default the client transport owns a background I/O thread that does every
socket send and receive. Datagrams cross between that thread and the caller
thread in preallocated buffers: three send rings, each owned by one side at a
time (`sdk/rocketnet_buffer_handoff.h`, `proofs/tla/RNBufferHandoff.tla`), and
three receive buffers shared through the receive handoff the server transports
use (`sdk/rocketnet_receive_handoff.h`, `proofs/tla/RNReceiveHandoff.tla`). Only
buffer indices and counts move, under a mutex held for
that exchange (and, in development builds, for a pending network-simulator
setting hand-over via `rocketnet_network_simulator_reconfigure` and
`rocketnet_network_simulator_take_setting`). `update` and `send_packets` only
try-acquire that mutex, for buffer exchanges and for taking a pending
development simulator setting alike: when the I/O thread holds it, the
exchange or the setting waits for the next call, so neither waits on the I/O
thread. Only `rocketnet_client_network_simulator`, which hands a new setting
over, acquires it blocking. Sends then leave with the next hand-off
(`CLIENT_SEND_DEFERRED`) and received datagrams with the next update. In a
development library only, the client config's `step_driven` (or
`ROCKETNET_CLIENT_STEP_DRIVEN=1`) lets deterministic
tests perform transport receive and send work inside those calls without a
client I/O thread. A production library refuses the field at create, does not
recognize that variable, and always uses the normal worker-owned route.

On receive, the I/O thread appends datagrams to its buffer and, at the end of
each pass, commits the buffer's count under the mutex; once update has started
on that buffer, the commit moves the I/O thread to the free one. Update's
try-acquire at its start takes every datagram committed before it, the rest of
the buffer it left (the tail) first, within its 1024-packet budget; the rest
wait for the next update in order, and none is processed twice or overwritten.
A datagram committed before an update that took the mutex is returned by that
update (`DeliveredByNextCall`). The legacy receive path shares the same handoff.

On the legacy path (a connection created with an active development link
simulator or without wait/wake), `stop` transfers the simulator to the worker
using the `quit` publication without calling the backend's `send`, and the
worker drains delayed packets before reporting completion; step-driven
transports drain in their explicit shutdown step. On the I/O-core path (when a
development link-simulator setting was applied live after create), the I/O
worker never owns the link simulator, so `rocketnet_client_transport_stop`
drains queued simulator packets on the caller's thread via
`rocketnet_client_link_drain( client_transport, DBL_MAX )` (invoking the
backend's `send`) before publishing `quit` (`proofs/tla/RNClientLinkStop.tla`
`Stop`). Polling (`stopped`) performs no backend work, and reconfiguration
after stop is refused.

At its exit, after its last send and before it publishes `stopped`, the I/O
worker reads every datagram still queued on its socket, counts each as
`CLIENT_RECEIVE_DISCARDED_AT_STOP`, and closes the backend's handles with its
`close` operation (`rocketnet_client_io_discard_at_stop`,
`proofs/tla/RNClientIoShutdown.tla` `WorkerQuiesce`). A datagram that arrives
later meets a closed port, which the operating system counts, so none waits
unread for destroy to free it. A transport racing both address families does
the same for both: its worker reads and counts what is queued on every family
still open, then closes them, the losing family already closed if the race
committed (`proofs/tla/RNClientRace.tla` `ReaderExit`).

Custom BASE drivers without wait/wake retain caller-thread per-packet sends for
normal traffic, while the disconnect burst is sent from the library's receive
worker thread (`rocketnet_client_transport_flush_disconnect`); this
compatibility path does not provide the built-in send offload guarantee, and a
custom `BASE` driver's `send` callback can be called on the library's thread.
Built-in loopback uses the same I/O core with its own wait and wake: an event-only wake per client, set by a send that makes the client's queue non-empty, so an idle loopback client sleeps until a datagram is queued for it, it is handed work or stopped.
The optional development link simulator is also an explicit exception to the
built-in worker-offload guarantee: while running, its frame work and backend
sends remain caller-owned, and its shutdown drain runs either on the legacy
worker or during `stop` on the I/O-core path as described above. This exception
is development-only and does not change the normal built-in transport handoff
contract.

Each built-in client transport fills the same operations
(`rocketnet_client_transport_io_operations_t`, `sdk/rocketnet_driver.h`) and
runs under this I/O thread: create opens the socket and its wake and
allocates everything, connect connects the socket to the one server, and the
I/O thread alone calls send_batch, receive_batch and wait. On Linux, `portable`
batches with plain `sendmmsg` and `recvmmsg`. A transient send refusal keeps
the unsent suffix in its I/O-owned ring. The next attempt resumes at that
cursor; later rings wait in order, and zero progress uses the existing wake
wait. No socket-owned copy or allocation is added. Shutdown attempts only
the remaining suffix through the existing terminal send path, counting its
refused packets before returning the ring. The optional internal progress
callback borrows packet bytes only during its call and returns an original
input prefix; other backends keep their synchronous callback contract.
The `gso` transport
(`sdk/rocketnet_client_gso_linux.cpp`, the Linux `auto` choice) sends each run of
equal-sized datagrams as one `UDP_SEGMENT` message and falls back to plain
messages without loss when the kernel refuses (TRANSPORTS.md, UDP
coalescing). It owns GSO send state (`rocketnet_client_gso_t`) and registers
`send_batch_progress` (`rocketnet_client_gso_send_batch_progress_operation`):
when the socket is connected, `rocketnet_client_gso_send_batch_progress` sends
the runs and, on a full socket (`EAGAIN`, `EWOULDBLOCK`, `ENOBUFS`), returns
the consumed prefix; an unconnected socket delegates to
`rocketnet_platform_socket_send_packets_mmsg_progress`. Either way the unsent
suffix stays at the I/O core's `send_slot` cursor for a retry.
On macOS, `msgx` uses `sendmsg_x` and `recvmsg_x`. On Windows, `iocp`
(`sdk/rocketnet_client_iocp_windows.cpp`, the Windows `auto` choice) waits on a completion
port of its own: wait is `GetQueuedCompletionStatusEx`, wake posts one wake
completion (at most one is outstanding, and a wake that receive_batch
dequeues is owed to the next wait, so the wake stays level-triggered), and
receive_batch copies completed overlapped receives and re-posts each after
its copy. The kernel owns a posted receive's buffer until its completion is
dequeued, a state RNClientIo does not have; `proofs/tla/RNClientIocp.tla` models it
(Post, Complete, Dequeue, Drain, Wake, Close, Free): a buffer is never copied
while posted, destroy frees only when every posted receive is drained, and
after Close every posted receive completes and drains. Its sends are
synchronous and add nothing to RNClientIo.

The client's `auto` order is the native client first and `portable` last:
`gso` then `portable` on Linux, `msgx` then `portable` on macOS, and `iocp`
then `portable` on Windows.
`portable` is the fallback everywhere: a native client whose create refuses
is logged once and the transport order opens `portable` (TRANSPORTS.md, "Client
transports").

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

The receive budget limits admitted packets. The backend also bounds work
spent inspecting discarded datagrams, so sustained discarded traffic cannot
keep a drain running indefinitely before receive publication or the worker's
next stop check. These work limits are not wall-clock completion bounds; see
[staging and drain bounds](TRANSPORT-INTERNALS.md#staging-and-drain-bounds).

The optional `time_callback` in the client config (development builds only)
supplies receive-stamp time;
it is not the game clock passed to update. It may run on the I/O thread and on
the caller's thread when an explicitly step-driven client is updated. Stop
and completion queries do not read that clock. Keep its function and context
valid through destroy, and make it safe for its calling threads. The game
clock supplied to time-taking API calls must be finite, nondecreasing, and
consistent across calls.

The executable invariants live in `proofs/tla/RNBufferHandoff.tla`,
`proofs/tla/RNClientIo.tla`, `proofs/tla/RNClientIoDecrypt.tla`, the unit suite, and the
functional transport matrix.

A development-only step-driven test advances shutdown explicitly with update (or its I/O-step
seam), then checks completion. Checking alone never drives the backend. A game
using the normal threaded mode keeps its event loop running while it polls.
Keep the allocator context, callback context, and transport memory alive through
completion and synchronous destroy; requesting stop does not transfer ownership.

The development client link simulator belongs to the caller during normal
updates. On the legacy path, `stop` publishes the `quit` request without
sending and transfers the simulator queue to the worker, which drains surviving
packets, including delayed disconnect copies, before publishing `stopped`
(explicit step-driven transports perform this drain in their shutdown step); on
the I/O-core path, `rocketnet_client_transport_stop` drains the simulator queue
directly on the caller's thread via `rocketnet_client_link_drain` before
publishing `quit` (`proofs/tla/RNClientLinkStop.tla` `Stop`). Simulator
reconfiguration is refused after stop. `test_client_simulator_stop_defers_backend_send` and
`test_client_legacy_receive_handoff_and_backpressure` exercise these boundaries
without threads, sleeps, or wall-clock deadlines.
