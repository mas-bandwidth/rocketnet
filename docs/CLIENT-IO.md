# Client socket I/O off the main thread

The client transport's goal here is less work on the game's main thread, not
more throughput. At the base of this design (`sprint/rocketnet-proof` at
`dfd313f`) the main thread calls the driver once per packet in
`rocketnet_client_transport_send_packets`
(`sdk/rocketnet_client_transport.cpp`, the send walk at lines 1001-1017), and
`rocketnet_client_transport_update` takes a mutex that the receive thread
holds for its whole drain (`rocketnet_client_update_swap_receive_buffers`,
line 515; `rocketnet_client_receive_packets`, lines 1234-1273). The receive
thread polls every 1 ms (line 1283) and stamps each datagram with the wall
clock (line 1265). This document measures that, designs the replacement,
models it in TLA+ (`tla/RNClientIo.tla`) and cuts the implementation cards.

## Measured

The meter is `bench/client_main_thread_cost.cpp` (see
`bench/client_main_thread_cost.md`): a real encrypted loopback connection,
P payloads of 200 bytes per frame, and two timed windows on the main thread,
`send_packets` and `update`. Release build with `make`, portable driver, on a
64-core Linux x86-64 bench (kernel 7.0.0-34-generic, g++ 15.2.0), three runs of
2000 measured frames per P, at the base above. Each cell is the median of the
three runs; all values are microseconds.

| P | send p50 | send p99 | send max | update p50 | update p99 | update max | total p50 | total p99 | total max |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 6.763 | 15.960 | 30.007 | 7.835 | 156.195 | 187.175 | 14.758 | 165.603 | 193.697 |
| 8 | 48.952 | 87.816 | 219.015 | 9.017 | 280.561 | 741.182 | 58.280 | 337.599 | 876.598 |
| 32 | 284.318 | 508.913 | 3297.709 | 370.030 | 1287.195 | 4959.403 | 661.431 | 1676.422 | 5241.467 |

The raw runs:

```
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=6.763 send_us_p99=19.557 send_us_max=30.007 update_us_p50=7.293 update_us_p99=155.033 update_us_max=176.865 total_us_p50=14.748 total_us_p99=165.233 total_us_max=182.936 received=2186
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=6.493 send_us_p99=15.660 send_us_max=29.225 update_us_p50=8.076 update_us_p99=163.039 update_us_max=271.854 total_us_p50=14.758 total_us_p99=170.242 total_us_max=278.287 received=2178
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=7.744 send_us_p99=15.960 send_us_max=41.148 update_us_p50=7.835 update_us_p99=156.195 update_us_max=187.175 total_us_p50=15.599 total_us_p99=165.603 total_us_max=193.697 received=2194
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=50.335 send_us_p99=85.051 send_us_max=126.659 update_us_p50=9.097 update_us_p99=282.014 update_us_max=549.229 total_us_p50=59.573 total_us_p99=337.599 total_us_max=622.738 received=17565
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=48.913 send_us_p99=87.816 send_us_max=219.015 update_us_p50=8.766 update_us_p99=280.561 update_us_max=741.182 total_us_p50=57.930 total_us_p99=338.300 total_us_max=876.598 received=17584
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=48.952 send_us_p99=102.965 send_us_max=419.463 update_us_p50=9.017 update_us_p99=266.474 update_us_max=3021.105 total_us_p50=58.280 total_us_p99=329.243 total_us_max=3072.022 received=17584
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=273.387 send_us_p99=508.202 send_us_max=3373.172 update_us_p50=370.030 update_us_p99=1287.195 update_us_max=4959.403 total_us_p50=661.431 total_us_p99=1676.422 total_us_max=5241.467 received=70368
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=284.318 send_us_p99=508.913 send_us_max=3297.709 update_us_p50=365.782 update_us_p99=1328.684 update_us_max=4452.394 total_us_p50=659.567 total_us_p99=1736.945 total_us_max=4736.401 received=70368
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=313.173 send_us_p99=577.833 send_us_max=850.629 update_us_p50=417.580 update_us_p99=1189.481 update_us_max=5736.924 total_us_p50=750.991 total_us_p99=1640.845 total_us_max=6066.257 received=70352
```

The same meter in the second attempt of this card, at `sprint/rocketnet-proof`
`bfcb888` (no client transport change since the base above), on a 16-core
Linux x86-64 bench that was loaded (load average about 50) during the runs,
three runs of 2000 frames per P, medians, microseconds. The p99 and max
columns carry that load; the p50 columns agree with the table above in shape:

| P | send p50 | send p99 | send max | update p50 | update p99 | update max | total p50 | total p99 | total max |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 4.779 | 7.584 | 24.065 | 7.364 | 299.907 | 2604.586 | 12.123 | 305.196 | 2609.695 |
| 8 | 38.863 | 49.723 | 2111.049 | 12.143 | 289.197 | 3797.181 | 51.546 | 517.981 | 3836.544 |
| 32 | 155.649 | 182.148 | 2869.348 | 190.865 | 781.081 | 4004.185 | 349.428 | 945.937 | 4154.815 |

At P = 32 update's median is 187 to 194 us here and 366 to 418 us on the
first bench: about 6 to 13 us of per-reply work for each of the 32 replies,
on the main thread whatever the I/O design does.

What the main thread pays for:

- **The send walk.** `send_packets` costs about 6 us per packet at the
  median (one `sendto` each): 49 us at P = 8 and 284 us at P = 32, with p99
  at 88 us and 509 us. That is all syscall work that another thread can do.
- **Waiting on the receive thread.** A scratch build (not committed) timed
  the mutex acquire in the update swap and the receive thread's hold of the
  same mutex. The main thread waited 0.04 us at the median and 71-86 us at
  p99 (max 282 us) at P = 1, 8 and 32; the receive thread held the mutex
  67-85 us at the median per non-empty drain (p99 109-166 us), because it
  holds it across every `recvfrom` of the drain. So at P = 1 about half of
  update's p99 of 156 us is a lock wait, not protocol work.
- **Bursts from the 1 ms poll.** The receive thread drains on a 1 ms sleep,
  so one update sees a burst; with the decrypt and the protocol per datagram
  on the main thread, update p50 rises to 370 us at P = 32 (about 11 us per
  reply). Decryption is not moved by this design (see What is left out), but
  the burst shape is: with a wait on readable, datagrams reach the buffer as
  they arrive.
- **At P = 32 the total p99 is 1.7 ms**, the order of the 0.5 to 1 ms per
  frame games report for socket calls on the main thread.

## The design

One client I/O thread replaces the receive thread (created at line 407 of
`sdk/rocketnet_client_transport.cpp`). It owns every socket call of the
transport once the transport is connected and running.

**The hand-off and the two rings.** The payload ring becomes two rings, A
and B, of the same layout (`rocketnet_client_payload_ring_t`). Game threads
fill the ring that is Filling exactly as today (one atomic add per slot,
sealed in place by `finish_packet`). `send_packets` runs with the fills
joined (the phase contract it already has) and does three things: it records
the ring's committed count, publishes the ring to the I/O thread with one
release store of a ring state word (Filling to HandedOff), and wakes the I/O
thread once. Then it switches the fill pointer to the other ring and returns.
No driver call is made on the main thread.

**Reuse only after sent.** The I/O thread sends every slot of a handed-off
ring (state Sending), then stores Sent with release. The main thread reopens
a ring (Sent to Filling, slot count to zero) only after an acquire load sees
Sent. If `send_packets` finds the other ring not yet Sent, the frame is late:
it counts a new counter, `CLIENT_SEND_LATE` in `sdk/rocketnet_counters.h`, and waits for Sent (a short
spin, then the platform wait). It never reuses a ring the I/O thread still
reads. At the measured cost (under 0.6 ms for 32 packets) a ring is Sent long
before the next frame, so a late frame is an overload signal, not a path.

**The protocol packet and disconnect.** The packet `update` builds
(`rocketnet_client_protocol_update`) and the disconnect burst of
`rocketnet_client_transport_disconnect` are sealed into a slot of the Filling
ring, like a payload, and handed off by the same step: `update` ends with a
hand-off when it committed a packet, and `disconnect` ends with one. Both run
on the main thread with fills joined.

**The wake.** The I/O thread waits on "socket readable or woken", with no
timeout while running (a bounded timeout only as a guard, never a cadence).
The wake is level-triggered: an eventfd on Linux, an `EVFILT_USER` kevent on
macOS and iOS, a manual-reset event on Windows. A wake set while the I/O
thread is not waiting makes the next wait return at once, so a check-then-wait
race cannot lose a hand-off. A handed-off ring leaves as soon as the I/O
thread is scheduled, not on the next millisecond.

**The receive path: the handoff.** The I/O thread drains the socket into
its own buffer (the existing `rocketnet_client_receive_buffer_t`, two of
them). Neither thread takes a lock. Each buffer carries one atomic state word,
`std::atomic<int> state`, with four values; each transition has one owner:

| transition | thread | memory order |
|---|---|---|
| Free to Fill (take the buffer to fill next, `num_packets = 0`) | I/O | acquire load, then plain store |
| Fill to Ready (publish) | I/O | release store |
| Ready to Exposed (expose) | main, at the start of `update` | acquire load, then plain store |
| Exposed to Free (return) | main, at the end of `update` | seq_cst store |

and the transport carries one more atomic, `std::atomic<bool>
receive_publish_owed`, written by the I/O thread and read by `update`. At the
start the I/O thread fills buffer 0 and buffer 1 is Free.

The I/O thread, at the end of every drain (the receive call returned zero, or
its buffer reached `ROCKETNET_NUM_CLIENT_PACKETS`), with `n` datagrams in its
buffer:

1. If `n` is zero, nothing to publish.
2. Otherwise, if the other buffer's state loads Free: take it (Free to Fill),
   publish its own buffer (Fill to Ready), store `receive_publish_owed =
   false`, and fill the taken buffer from now on.
3. Otherwise (the other buffer is Ready or Exposed): store
   `receive_publish_owed = true` (seq_cst), then load the other buffer's state
   again (seq_cst); if it is Free now, do step 2. If not, keep the batch: keep
   reading while the buffer has room, and leave datagrams in the socket once
   it is full.

It never blocks in `wait` while step 2 is possible, and it blocks with
unpublished datagrams only after step 3 has stored the owed flag.

`update`, every frame:

1. At its start, if a buffer's state loads Ready (acquire), expose it (Ready
   to Exposed) and process its datagrams (the checks, the counters, the
   decrypt and the protocol, as at BASE).
2. At its end, whether or not step 1 exposed anything this frame, return the
   buffer it holds: `num_packets = 0`, then Exposed to Free (seq_cst store),
   then load `receive_publish_owed` (seq_cst); if it is true, call the
   driver's `wake`.

The pair of seq_cst store-then-load on each side (the I/O thread's owed store
then state load; update's state store then owed load) is what keeps the wake:
at least one side sees the other's store, so either the I/O thread publishes
itself or update wakes it. With release and acquire alone both loads may miss,
the I/O thread sleeps on a batch nobody wakes, and the datagrams wait for the
next arrival. A datagram read while update holds a buffer reaches the next
update.

Why this rule. The first draft swapped in one step: update returned its
exposed buffer only when it exposed a Ready one. With two buffers, one
Exposed and the other holding datagrams, the I/O thread has no Free buffer to
publish into and update has no Ready one to expose, so nothing after the
first exposed batch reaches update; the reversed witness
`MCRNClientIoBrokenReceiveStall.cfg` is that defect (`NoReceiveStall`, seven
steps). Returning at the end of every update fixes it at the least
main-thread cost: one acquire load at the start, one seq_cst store and one
seq_cst load at the end, and a `wake` syscall only on a frame where the I/O
thread blocked with an owed batch. A third buffer does not remove the wake (a
full third buffer behind a Ready and an Exposed one is the same blocked
state) and adds a buffer of `ROCKETNET_NUM_CLIENT_PACKETS` frames. Publishing
into the buffer update holds writes memory update reads
(`MCRNClientIoBrokenSwapWhileReading.cfg`, `ExposedStable`) or needs a lock,
and the lock is the 71-86 us p99 wait measured at BASE. The cheap size
prefilter stays on the I/O thread; the full checks, the counters and the
decrypt stay in `update`.

**The clock.** `rocketnet_client_transport_config_t` gains
`double (*time_callback)( void * context )`, read once at create; NULL means
`rocketnet_platform_time`. The I/O thread stamps each datagram's
`receive_time` with it (today's line 1265). Unit tests set it to a test
clock that starts at 1000.0 and is advanced by the test, so no test sleeps
or reads the wall clock.

**The I/O step.** The I/O thread's loop body is one function,
`rocketnet_client_io_step( transport, block )`: take a handed-off ring and
send it, drain and publish, then wait if `block` and nothing is left. The
thread calls it in a loop; unit tests call it directly with `block = false`
and a fake driver, on the test's thread, so the hand-off, reopen, publish,
owed flag and stop order are tested deterministically with no thread and no
socket.

**Stop and destroy order.** `destroy` stores quit and wakes the I/O thread.
The I/O thread finishes the ring in flight (so a disconnect burst handed off
just before destroy is sent), sends nothing new, and exits. The main thread
joins it, then closes the driver, then frees the transport. Nothing is freed
while the I/O thread can touch it (`FreedOnlyAfterJoin`).

## The interface

`rocketnet_client_driver_operations_t` in `sdk/rocketnet_driver.h` gains four
optional callbacks after the existing four, so every existing initializer
stays valid and leaves them NULL:

```c
struct rocketnet_client_driver_operations_t
{
    void * (*create)( void *, const rocketnet_client_driver_config_t *, rocketnet_address_t * );
    void (*destroy)( void * );
    void (*send)( void *, const rocketnet_address_t *, const uint8_t *, int );
    int (*receive)( void *, rocketnet_address_t *, uint8_t *, int );

    // optional; I/O thread only. Sends num_packets datagrams in one batch,
    // borrowing sends and the bytes they point at for the call only.
    void (*send_batch)( void * driver, const rocketnet_platform_send_t * sends, int num_packets );
    // optional; I/O thread only. Drains up to max_packets into caller
    // storage at packet_data + i * packet_stride, writing from[i] and
    // packet_bytes[i]; returns the count, zero when drained. Borrows the
    // storage for the call only.
    int (*receive_batch)( void * driver, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets );
    // optional, with wake; I/O thread only. Blocks until the socket is
    // readable, a wake is set, or timeout_seconds pass; consumes the wake.
    // Returns true when readable or woken.
    bool (*wait)( void * driver, double timeout_seconds );
    // optional, with wait; any thread. Sets the level-triggered wake, so a
    // wait in progress or the next wait returns at once.
    void (*wake)( void * driver );
};
```

`rocketnet_client_driver_open` checks once at create that `wait` and `wake`
are both set or both NULL. A backend with `wait` and `wake` runs the I/O
thread design; `send_batch` and `receive_batch` are used when set, and the
per-packet `send` and `receive` otherwise, called from the I/O thread. A
backend without `wait` and `wake` keeps the BASE path unchanged: per-packet
sends on the main thread and the polling receive thread. The comment above
the struct changes from "Clients retain the existing transport receive
thread" to name both paths. `tools/exports.txt` gains nothing: the struct is
internal and the config field is a struct member, not an export.

## The gate

The gate is total main-thread time per frame: **total_us_p99 at most 100 us at
P = 32**, send plus update, on the platform's own member. Each implementation
card that switches a platform's client driver to the I/O thread design runs
the meter there, three runs per P, BASE and head in the same session on the
same machine, and reports medians for P = 1, 8 and 32 beside this table.

| measure | BASE (Linux, above) | gate |
|---|---:|---:|
| total_us_p99, P = 32 | 1676.422 | at most 100 |
| send_us_p99, P = 32 | 508.913 | at most 10 (one release store and at most one wake) |
| total_us_p99, P = 1 and P = 8 | 165.603 and 337.599 | no worse than BASE |

What the 100 us gate means for this design. The I/O thread design removes the
send walk (509 us p99 at P = 32) and the receive-mutex wait (71-86 us p99),
and leaves in `update` the per-reply work: the checks, the decrypt and the
protocol, about 6 to 13 us per reply, so 187 to 418 us per frame at P = 32
(update p50 at BASE is 370 us at P = 32). That alone is above 100 us. So these
cards can meet the send row and the P = 1 and P = 8 rows, but the P = 32 total
cannot be met while the per-reply work stays on the main thread: each
implementation card reports its P = 32 total against 100 us and names the
gap, and the gate is reached only by the follow-on that moves the per-reply
decrypt and checks onto the I/O thread (`rocketnet-client-io-decrypt`, its
own design with its own model, see What is left out). Moving it leaves update
a copy-out of already-verified payloads and the protocol state, which the
same meter measures.

## Per-platform cards

**rocketnet-client-io-transport (lands first, platform-generic).** PATHS:
`sdk/rocketnet_client_transport.cpp`, `sdk/rocketnet_driver.h`,
`sdk/rocketnet_driver.cpp` (the open-time check), `sdk/rocketnet.h` and
`sdk/rocketnet_config.cpp` (the `time_callback` field),
`sdk/rocketnet_counters.h` (`CLIENT_SEND_LATE`), a new
`test_unit_client_io.cpp`, `tla/COVERAGE.tsv` (client-io to current) and the
code comments citing `tla/RNClientIo.tla` actions. It adds the two rings, the
hand-off, the reopen-after-Sent rule with the late counter, the receive
handoff exactly as The receive path states it (the state word per buffer, the
owed flag and its seq_cst pairing, the return at the end of every update), the
clock callback, `rocketnet_client_io_step`
and the stop order, and the open-time check; no built-in driver gains `wait`
yet, so every platform stays on the BASE path in production. Tests, all unit
tier with a fake client driver and the step called directly, clock from 1000.0:
`client_io_handoff_sends_ring_once`, `client_io_ring_not_reused_before_sent`
(the late counter moves, the ring is not rewritten),
`client_io_exposed_buffer_stable_until_update`,
`client_io_update_returns_buffer_every_update` (two buffers: a batch read
while update holds the other is exposed by the next update, the stall of
`MCRNClientIoBrokenReceiveStall.cfg`),
`client_io_owed_publish_wakes_io_thread` (the I/O step stores owed and
blocks; the return at the end of update calls the fake driver's `wake`
exactly once),
`client_io_receive_time_from_callback`,
`client_io_destroy_sends_in_flight_disconnect`,
`client_io_backend_without_wait_uses_base_path`. Red at BASE:
`./bin/test client_io_handoff_sends_ring_once` prints
`unknown unit test client_io_handoff_sends_ring_once`, exit 2. Needs: this
card.

**rocketnet-client-io-linux.** PATHS: `sdk/rocketnet_driver_builtin.cpp`,
`sdk/rocketnet_platform.cpp` (Linux section), `sdk/rocketnet_platform.h`,
`functional/rocketnet_functional_tests.cpp`, `bench/client_main_thread_cost.md`.
The portable client on Linux gains `wait` (poll on the socket and an eventfd),
`wake` (eventfd write), `send_batch` (`sendmmsg`) and `receive_batch`
(`recvmmsg`). Tests: a functional loopback case
`client_io_linux_handoff_round_trip` (real socket, real thread) and the
driver-contract check in `./bin/test_driver`; the gate rows above from the
meter. io_uring is not used: at the measured cost the I/O thread is idle most
of the frame, `sendmmsg` already makes one syscall per frame, and io_uring is
often disabled in containers and sandboxes; a card may revisit it with a
measurement. Red at BASE: `./bin/functional --run client_io_linux_handoff_round_trip`
prints `unknown functional test client_io_linux_handoff_round_trip`, exit 2.
Needs: rocketnet-client-io-transport.

**rocketnet-client-io-apple (macOS and iOS).** PATHS:
`sdk/rocketnet_driver_builtin.cpp`, `sdk/rocketnet_platform.cpp` (Mac and iOS
section), `functional/rocketnet_functional_tests.cpp`. The msgx client gains
`send_batch` and `receive_batch` through the existing pump
(`rocketnet_platform_socket_send_packets` at line 1997 and
`rocketnet_platform_socket_receive_packets` at line 2123, `sendmsg_x` and
`recvmsg_x` with their per-packet fallback), and `wait`/`wake` through one
kqueue holding the socket's `EVFILT_READ` and an `EVFILT_USER` trigger.
Tests: functional `client_io_apple_handoff_round_trip` (SKIPPED twin
elsewhere), `./bin/test_driver`, the meter gate on a macOS member. Red at
BASE: `./bin/functional --run client_io_apple_handoff_round_trip` prints
`unknown functional test client_io_apple_handoff_round_trip`, exit 2. Needs:
rocketnet-client-io-transport.

**rocketnet-client-io-windows.** PATHS: `sdk/rocketnet_driver_builtin.cpp`,
`sdk/rocketnet_platform.cpp` (Windows section), `sdk/rocketnet_platform.h`,
`functional/rocketnet_functional_tests.cpp`. The portable client on Windows
gains `wait` (`WSAEventSelect` with `FD_READ` on the socket and
`WaitForMultipleObjects` over that event and a manual-reset wake event) and
`wake` (`SetEvent`); `send_batch` loops `WSASendMsg` on the I/O thread.
Tests: functional `client_io_windows_handoff_round_trip`, the native driver
contract run, the meter gate on the Windows member. Red at BASE: the same
`--run` line prints `unknown functional test client_io_windows_handoff_round_trip`,
exit 2. Needs: rocketnet-client-io-transport.

## What is left out

- **Decryption and the per-reply checks stay on the main thread in these
  cards.** At P = 32 that work is 187 to 418 us per frame, above the 100 us
  gate, so the gate needs the follow-on `rocketnet-client-io-decrypt`: a
  design card with its own TLA+ model, because the key, the replay window and
  the counters would then be written by the I/O thread and read by update.
  It is cut after rocketnet-client-io-transport lands and is measured with
  the same meter.
- **RIO on Windows and io_uring on Linux** for the client: not needed for
  the gate at the measured cost; a later card may measure them.
- **The server transport** is unchanged.
- **The model** abstracts slots, datagrams and buffers to counts and states;
  a full receive buffer is the same owed path as a buffer with no Free one
  to swap to (the model's buffers have no capacity bound); the late
  counter's spin, the guard timeout, packet bytes and the clock are not
  modelled. Each action is atomic, which is the seq_cst pairing of the
  handoff; a release and acquire implementation of the owed flag is outside
  what the model checks.

## The model

`tla/RNClientIo.tla` (wrapper `tla/MCRNClientIo.tla`, document
`tla/README-RNClientIo.md`) models this design: the two send rings, each
slot's owner, the receive buffers' handshake, the wake, and the thread phase.
Update's two steps are the actions `Expose` and `Return`; the owed flag is
`IoOwe`. `MCRNClientIo.cfg` checks `TypeOK`, `NoReuseBeforeSent`,
`ExposedStable`, `FreedOnlyAfterJoin` and `NoReceiveStall` (while the
transport runs, datagrams in the I/O thread's buffer always have a next step
toward update), and under weak fairness of the I/O thread, the join and
update's two steps `HandedOffEventuallySent`, `StopEventuallyJoins`,
`FilledEventuallyExposed` (a buffer of received datagrams is eventually
exposed) and `ArrivedEventuallyDelivered` (every datagram that arrives
reaches update). The three reversed witnesses,
`MCRNClientIoBrokenReuseBeforeSent.cfg`,
`MCRNClientIoBrokenSwapWhileReading.cfg` and
`MCRNClientIoBrokenReceiveStall.cfg`, each violate exactly their declared
invariant.
