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
No driver call is made on the main thread. The same hand-off runs from
`rocketnet_client_transport_send_packet`, which calls `send_packets` after
every packet it writes (line 758 at `7ffb924`), and at the end of `update`
(below), so a frame commonly makes two or more hand-offs.

**Reuse only after sent; a hand-off never waits.** The I/O thread sends
every slot of a handed-off ring (state Sending), then stores Sent with
release. A hand-off first loads the other ring's state (acquire):

1. Sent: reopen it (Sent to Filling, slot count to zero), then hand off the
   Filling ring as above.
2. Filling (only before the first hand-off): hand off as above.
3. HandedOff or Sending: the hand-off is deferred. The committed slots stay
   in the Filling ring, the ring stays open to the game threads, and the
   call counts `CLIENT_SEND_DEFERRED` (a new row
   `X( CLIENT_SEND_DEFERRED, EXPECTED, NONE )` appended to
   `sdk/rocketnet_counters.h`) and returns. Nothing is lost and nothing
   waits: the slots leave with the next hand-off that finds the other ring
   Sent, from the next `send_packet`, `send_packets` or `update`.

The main thread never waits for the I/O thread on these paths, and never
reuses a ring the I/O thread still reads. Deferral is the common case, not an
overload signal: `send_packet` followed by the hand-off at the end of
`update` in the same frame finds the first ring still HandedOff whenever the
I/O thread has not been scheduled in between (the reversed witness
`MCRNClientIoBrokenHandOffWaits.cfg` is the second draft of this card, which
waited there, `HandOffNeverWaits`, seven steps). Because `update` ends with a
hand-off every frame, deferred slots leave with the first hand-off after the
I/O thread has sent the other ring, normally the next frame's update
(`CommittedEventuallyHandedOff`). While deferred slots pile up the Filling
ring can fill; `start_packet` then refuses as at BASE (`SEND_RING_FULL`).
Only `destroy` waits. `disconnect` seals its burst into the Filling ring and
hands it off like any packet (deferred when the other ring is busy, retried
by the next `update`); `destroy` makes the last hand-off itself, waiting for
the other ring to be Sent if it must (Stop and destroy order), so a
disconnect burst is on the wire before the transport is freed whether or not
an `update` ran in between.

**The protocol packet and disconnect.** The packet `update` builds
(`rocketnet_client_protocol_update`) and the disconnect burst of
`rocketnet_client_transport_disconnect` are sealed into a slot of the Filling
ring, like a payload, and handed off by the same step: `update` ends with a
hand-off attempt every frame (it retries any deferred slots, and hands off
its own packet when it committed one), and `disconnect` ends with one. Both
run on the main thread with fills joined.

**The wake.** The I/O thread waits on "socket readable or woken", with no
timeout while running (a bounded timeout only as a guard, never a cadence).
While its receive buffer is full (`ROCKETNET_NUM_CLIENT_PACKETS`
datagrams) it waits on the wake alone: the driver's `wait` takes a
`readable` argument, and the I/O thread passes false when its buffer has no
room. A full buffer leaves datagrams in the socket, so a wait that also
returned on readable would return at once, every time, and the I/O thread
would spin until `update` returns a buffer (the reversed witness
`MCRNClientIoBrokenFullWaitReadable.cfg`, `FullWaitsOnWake`). With a full
buffer the I/O thread has always stored the owed flag first (step 3 below),
so the return at the end of the next `update` wakes it.
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

and the transport carries one more atomic, `std::atomic<bool> receive_publish_owed`, written by the I/O thread and read by `update`. At the
start the I/O thread fills buffer 0 and buffer 1 is Free.

The I/O thread, at the end of every drain (the receive call returned zero, or
its buffer reached `ROCKETNET_NUM_CLIENT_PACKETS`), with `n` datagrams in its
buffer:

1. If `n` is zero, nothing to publish.
2. Otherwise, if the other buffer's state loads Free: take it (Free to Fill),
   publish its own buffer (Fill to Ready), store `receive_publish_owed = false`, and fill the taken buffer from now on.
3. Otherwise (the other buffer is Ready or Exposed): store
   `receive_publish_owed = true` (seq_cst), then load the other buffer's state
   again (seq_cst); if it is Free now, do step 2. If not, keep the batch: keep
   reading while the buffer has room, and leave datagrams in the socket once
   it is full.

It never blocks in `wait` while step 2 is possible, and it blocks with
unpublished datagrams only after step 3 has stored the owed flag. It calls
`wait( driver, readable, timeout )` with `readable` true while its buffer has
room and false while it is full; a full buffer is the only case in which it
blocks with datagrams still in the socket.

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

**Stop and destroy order.** `destroy` first empties the Filling ring: if it
holds committed slots (a deferred disconnect burst, say), it waits until the
other ring loads Sent (acquire; `rocketnet_platform_sleep( 0.0001 )` between
loads; destroy is off the frame path), reopens it and hands the Filling ring
off. Then it stores quit and wakes the I/O thread. The I/O thread finishes
every handed-off ring (so the disconnect burst is sent), sends nothing new,
and exits. The main thread joins it, then closes the driver, then frees the
transport. Nothing is freed while the I/O thread can touch it
(`FreedOnlyAfterJoin`), and no committed slot is dropped by destroy
(`CommittedEventuallyHandedOff`). In the step-driven unit mode there is no
I/O thread: `destroy` calls `rocketnet_client_io_step( transport, false )`
itself in place of the sleep and the join, so the order is tested with no
real time.

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
    // optional, with wake; I/O thread only. Blocks until a wake is set,
    // the socket is readable (only when readable is true), or
    // timeout_seconds pass; consumes the wake. With readable false the
    // socket's readiness is ignored. Returns true when woken or readable.
    bool (*wait)( void * driver, bool readable, double timeout_seconds );
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

The gate is total main-thread time per frame: **total_us_p99 at most 100 us at P = 32**, send plus update, on the platform's own member; the owner's number is that with the right transport the main thread's socket work is about 0.1 ms per frame or less. Each implementation card that switches a platform's client driver to the I/O thread design runs the meter there, three runs per P, BASE and head interleaved in the same session on the same idle machine (`uptime` recorded before each pair; a load average above a quarter of the core count voids the pair), and reports medians for P = 1, 8 and 32 beside this table. A card on a platform whose BASE differs states its own BASE row beside these.

| measure | BASE (Linux, above) | gate |
|---|---:|---:|
| send_us_p99, P = 32 | 508.913 | at most 10 (one release store and at most one wake) |
| send_us_p99, P = 8 | 87.816 | at most 10 |
| total_us_p99, P = 1 and P = 8 | 165.603 and 337.599 | no worse than BASE |
| total_us_p99, P = 32 | 1676.422 | reported against 100 with the gap; gated at most 100 only by rocketnet-client-io-gate-100us |

What the 100 us gate means for this design. The I/O thread design removes the send walk (509 us p99 at P = 32) and the receive-mutex wait (71-86 us p99), and leaves in `update` the per-reply work: the checks, the decrypt and the protocol, about 6 to 13 us per reply, so 187 to 418 us per frame at P = 32 (update p50 at BASE is 370 us at P = 32). That alone is above 100 us. So these cards can meet the send rows and the P = 1 and P = 8 rows, but the P = 32 total cannot be met while the per-reply work stays on the main thread: each implementation card reports `total_us_p99 P=32 head <x> target 100 gap <x - 100>` with the send and update windows separately, and the P = 32 total is gated at most 100 only by rocketnet-client-io-gate-100us, after the decrypt study (rocketnet-client-io-decrypt-study) decides how the per-reply decrypt and checks leave the main thread (`rocketnet-client-io-decrypt`, its own design with its own model, see What is left out). Moving it leaves update a copy-out of already-verified payloads and the protocol state, which the same meter measures.

### The gate, measured

2026-10-18, head 8ddc93e (sprint/rocketnet-client-io-gate-100us.w1.g1.e21; BASE and head are the same tree, so one measurement serves both columns), on a 16-core x86_64 Linux 6.8.0-139-generic member with g++ 13.3.0. The runs sit under ambient load (load average 6 to 13 on 16 cores, above a quarter of the core count), so p99 carries that load and an idle run would only read lower. Medians of three runs at each P, `./bin/client_main_thread_cost portable <P> 2000`:

| P | measure | BASE | head |
|---|---:|---:|---:|
| 1 | send_us_p50 / p99 | 0.880 / 1.420 | 0.880 / 1.420 |
| 1 | update_us_p50 / p99 | 0.630 / 1.350 | 0.630 / 1.350 |
| 1 | total_us_p50 / p99 | 1.520 / 2.750 | 1.520 / 2.750 |
| 8 | send_us_p50 / p99 | 1.500 / 2.120 | 1.500 / 2.120 |
| 8 | update_us_p50 / p99 | 4.260 / 4.990 | 4.260 / 4.990 |
| 8 | total_us_p50 / p99 | 5.760 / 7.590 | 5.760 / 7.590 |
| 32 | send_us_p50 / p99 | 1.580 / 5.880 | 1.580 / 5.880 |
| 32 | update_us_p50 / p99 | 12.830 / 20.441 | 12.830 / 20.441 |
| 32 | total_us_p50 / p99 | 14.440 / 51.272 | 14.440 / 51.272 |

Raw head lines:

```
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=1.580 send_us_p99=5.880 send_us_max=220.396 update_us_p50=12.830 update_us_p99=30.101 update_us_max=55.381 total_us_p50=14.440 total_us_p99=51.272 total_us_max=271.788 received=70336
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=1.230 send_us_p99=113.003 send_us_max=744.361 update_us_p50=8.550 update_us_p99=20.441 update_us_max=106.293 total_us_p50=9.770 total_us_p99=133.444 total_us_max=769.612 received=70336
client_main_thread_cost driver=portable packets_per_frame=32 frames=2000 send_us_p50=1.610 send_us_p99=2.810 send_us_max=179.975 update_us_p50=13.031 update_us_p99=17.441 update_us_max=45.521 total_us_p50=14.681 total_us_p99=20.051 total_us_max=211.246 received=70336
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=1.500 send_us_p99=2.180 send_us_max=1999.466 update_us_p50=4.260 update_us_p99=6.471 update_us_max=14.640 total_us_p50=5.760 total_us_p99=8.870 total_us_max=2008.496 received=17584
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=1.400 send_us_p99=1.990 send_us_max=4.870 update_us_p50=4.140 update_us_p99=4.990 update_us_max=10.820 total_us_p50=5.560 total_us_p99=7.590 total_us_max=12.300 received=17584
client_main_thread_cost driver=portable packets_per_frame=8 frames=2000 send_us_p50=1.500 send_us_p99=2.120 send_us_max=5.290 update_us_p50=4.290 update_us_p99=4.980 update_us_max=14.471 total_us_p50=5.790 total_us_p99=7.340 total_us_max=17.121 received=17584
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=0.850 send_us_p99=1.260 send_us_max=3.480 update_us_p50=0.600 update_us_p99=1.350 update_us_max=7.070 total_us_p50=1.440 total_us_p99=2.520 total_us_max=8.020 received=2198
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=0.880 send_us_p99=1.640 send_us_max=4.110 update_us_p50=0.630 update_us_p99=1.960 update_us_max=4.010 total_us_p50=1.520 total_us_p99=3.380 total_us_max=6.170 received=2198
client_main_thread_cost driver=portable packets_per_frame=1 frames=2000 send_us_p50=0.911 send_us_p99=1.420 send_us_max=5.791 update_us_p50=0.660 update_us_p99=1.260 update_us_max=3.030 total_us_p50=1.600 total_us_p99=2.750 total_us_max=6.802 received=2198
```

Reached: total_us_p99 P=32 head 51.272 target 100 gap -48.728.

## Per-platform cards

**rocketnet-client-io-transport (lands first, platform-generic).** PATHS:
`sdk/rocketnet_client_transport.cpp`, `sdk/rocketnet_driver.h`,
`sdk/rocketnet_driver.cpp` (the open-time check), `sdk/rocketnet.h` and
`sdk/rocketnet_config.cpp` (the `time_callback` field),
`sdk/rocketnet_counters.h` (`CLIENT_SEND_DEFERRED`), a new
`test_unit_client_io.cpp`, `tla/COVERAGE.tsv` (client-io to current) and the
code comments citing `tla/RNClientIo.tla` actions. It adds the two rings, the
hand-off that never waits (reopen after Sent, defer and count otherwise,
the retry at the end of every update, and the wait in destroy only), the
receive handoff exactly as The receive path states it (the state word per buffer, the
owed flag and its seq_cst pairing, the return at the end of every update,
`wait` with `readable` false while the buffer is full), the
clock callback, `rocketnet_client_io_step`
and the stop order, and the open-time check; no built-in driver gains `wait`
yet, so every platform stays on the BASE path in production. Tests, all unit
tier with a fake client driver and the step called directly, clock from 1000.0:
`client_io_handoff_sends_ring_once`, `client_io_ring_not_reused_before_sent`
(the ring is not rewritten while the I/O thread holds it),
`client_io_handoff_never_waits` (`send_packet` then `update` with the I/O
step not run in between: both return, `CLIENT_SEND_DEFERRED` is 1, and after
one I/O step the next `update` hands the deferred packet off and the step
after that sends it; the stall of `MCRNClientIoBrokenHandOffWaits.cfg`),
`client_io_full_buffer_waits_on_wake_only` (the fake driver holds more than
`ROCKETNET_NUM_CLIENT_PACKETS` datagrams, update holds the other buffer: the
step calls `wait` with `readable` false, and the return at the end of
update wakes it; the spin of `MCRNClientIoBrokenFullWaitReadable.cfg`),
`client_io_exposed_buffer_stable_until_update`,
`client_io_update_returns_buffer_every_update` (two buffers: a batch read
while update holds the other is exposed by the next update, the stall of
`MCRNClientIoBrokenReceiveStall.cfg`),
`client_io_owed_publish_wakes_io_thread` (the I/O step stores owed and
blocks; the return at the end of update calls the fake driver's `wake`
exactly once),
`client_io_receive_time_from_callback`,
`client_io_destroy_sends_in_flight_disconnect` (also with the burst
deferred behind a ring still HandedOff),
`client_io_backend_without_wait_uses_base_path`. Red at BASE:
`./bin/test client_io_handoff_sends_ring_once` prints
`unknown unit test client_io_handoff_sends_ring_once`, exit 2. Needs: this
card.

**rocketnet-client-io-linux.** PATHS: `sdk/rocketnet_driver_builtin.cpp`,
`sdk/rocketnet_platform.cpp` (Linux section), `sdk/rocketnet_platform.h`,
`functional/rocketnet_functional_tests.cpp`, `bench/client_main_thread_cost.md`.
The portable client on Linux gains `wait` (poll on an eventfd, plus the
socket with `POLLIN` when `readable` is true),
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
kqueue holding the socket's `EVFILT_READ` and an `EVFILT_USER` trigger; when
`readable` changes, the same `kevent` call carries `EV_DISABLE` or
`EV_ENABLE` for the read filter, so a full buffer waits on the trigger alone.
Tests: functional `client_io_apple_handoff_round_trip` (SKIPPED twin
elsewhere), `./bin/test_driver`, the meter gate on a macOS member. Red at
BASE: `./bin/functional --run client_io_apple_handoff_round_trip` prints
`unknown functional test client_io_apple_handoff_round_trip`, exit 2. Needs:
rocketnet-client-io-transport.

**rocketnet-client-io-windows.** PATHS: `sdk/rocketnet_driver_builtin.cpp`,
`sdk/rocketnet_platform.cpp` (Windows section), `sdk/rocketnet_platform.h`,
`functional/rocketnet_functional_tests.cpp`. The portable client on Windows
gains `wait` (`WSAEventSelect` with `FD_READ` on the socket and
`WaitForMultipleObjects` over that event and a manual-reset wake event, or
`WaitForSingleObject` on the wake event alone when `readable` is false) and
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
- **The model** abstracts slots, datagrams and buffers to counts and states,
  with a receive buffer of `BufferCap` datagrams (one in the committed run,
  so the full buffer with datagrams left in the socket is reached in a few
  steps); the deferred counter is a flag, destroy's wait for Sent is the
  guard on `Stop`, and the guard timeout, packet bytes and the clock are not
  modelled. Each action is atomic, which is the seq_cst pairing of the
  handoff; a release and acquire implementation of the owed flag is outside
  what the model checks.

## The model

`tla/RNClientIo.tla` (wrapper `tla/MCRNClientIo.tla`, document
`tla/README-RNClientIo.md`) models this design: the two send rings, each
slot's owner, the receive buffers' handshake, the wake, and the thread phase.
Update's two steps are the actions `Expose` and `Return`; the owed flag is
`IoOwe`; a deferred hand-off is `Defer`, and the wait's `readable` argument
is `waitReadable`. `MCRNClientIo.cfg` checks `TypeOK`, `NoReuseBeforeSent`,
`ExposedStable`, `FreedOnlyAfterJoin`, `HandOffNeverWaits` (the main thread
never blocks on the I/O thread), `FullWaitsOnWake` (a blocked I/O thread with
a full buffer waits on the wake alone) and `NoReceiveStall` (while the
transport runs, datagrams in the I/O thread's buffer always have a next step
toward update), and under weak fairness of the I/O thread, the join, a game
thread's finish of its packet, the reopen, the hand-off and update's two
receive steps `HandedOffEventuallySent`, `CommittedEventuallyHandedOff` (a
committed slot, deferred or not, is eventually handed to the I/O thread),
`StopEventuallyJoins`, `FilledEventuallyExposed` (a buffer of received
datagrams is eventually exposed) and `ArrivedEventuallyDelivered` (every
datagram that arrives reaches update). The five reversed witnesses,
`MCRNClientIoBrokenReuseBeforeSent.cfg`,
`MCRNClientIoBrokenSwapWhileReading.cfg`,
`MCRNClientIoBrokenReceiveStall.cfg`,
`MCRNClientIoBrokenHandOffWaits.cfg` and
`MCRNClientIoBrokenFullWaitReadable.cfg`, each violate exactly their declared
invariant.
