# Server disconnect bursts across updates and across the stop

`ServerStopSpread.tla` models the server transport's disconnect ring with its
own index arithmetic and the transport's send staging
(`sdk/rocketnet_server_transport.cpp`: `disconnect_bursts`,
`rocketnet_server_transport_defer_disconnect_burst`,
`rocketnet_server_transport_drain_disconnect_bursts`, the stop step and
`rocketnet_server_transport_send_capacity_available`; `sdk/rocketnet_api.cpp`
`rocketnet_server_stop`). `ServerSession.tla` coarsens the stop's seals into
its Stop step; this model is the stop as a state machine across steps.

## What is modelled

| Variable | Meaning | Code |
|---|---|---|
| `ring`, `head`, `count`, `cursor` | the ring of `RingSize` entries, each a session and its sealed count | `disconnect_bursts`, `disconnect_burst_head`, `disconnect_burst_count`, `disconnect_burst_cursor` |
| `staged`, `inflight` | frames started since the last flush, and frames flushed that the transport still holds | the staged bank (the io_uring array, the gso producer bank, the IOCP send operations, the RIO fill phase, an XDP queue) |
| `phase` | `RUNNING`, `STOPPING`, `IOSTOP` (the I/O told to stop), `STOPPED` | `state`, `io_stop_requested`, the I/O's stopped |
| `sent`, `owed`, `abandoned`, `refused`, `overflow` | ghosts | |

`Room` is `BankSize - staged - inflight`: the transport's `send_room`
operation as `send_capacity_available` returns it.

| Action | Code |
|---|---|
| `Kick(i)` | `rocketnet_server_transport_disconnect_client` on a running server: the burst appended at `( head + count ) % RingSize` while fewer than `SteadyCap` wait, else its copies dropped (the immediate seal is this append and its seals in the same update) |
| `Stop` | `rocketnet_server_stop`: every connected slot's burst appended (`rocketnet_server_transport_disconnect_client_shutdown`), then STOPPING |
| `SealOne` | one position of the drain's pass: a complete entry passed over, an unreachable start abandoning its burst, or one packet sealed while `Room > 0`; a completed head leaves with the cursor |
| `Flush` | the stop step's or the update's flush |
| `RequestIoStop` | the stop step that finds the ring empty and its seals flushed calls `rocketnet_server_transport_io_stop` |
| `IoStopped` | the workers exit; `rocketnet_server_stopped` is true |
| `Connect(i)` | environment: a client takes a free slot |
| `Complete` | environment: the kernel or worker finishes a flushed frame |
| `LoseRoute(s)`, `FindRoute(s)` | environment: a destination loses or regains its link-layer address (the XDP reply MAC) |

## Properties

- `RingHolds`: an append always finds a free entry (`RingSize = 2 * SteadyCap`).
- `SealedWithinBurst`: no entry is sealed past its burst, so no sequence of
  the burst's reserved range is ever reused.
- `NoRefusedStart`: no staged start is called that the transport refuses for room.
- `WithinBank`: the staging never holds more than the transport has.
- `RingIsOwed`: the live window holds exactly the owed bursts not yet complete
  or abandoned, each once, each entry's count the packets its session was sent,
  every other entry clear (the layout refines the abstract owed set).
- `HeadNeverDone`: a completed head leaves at once.
- `FullBurstBeforeStopped`: before the I/O is told to stop, every burst kept
  (every client connected at the stop, every burst still waiting from a
  kick) has been handed whole to the transport, but for a destination the
  transport cannot reach.
- `StopFinishes` (liveness): STOPPING leads to STOPPED, under weak fairness of
  the drain, the flush, the transport's completions and the I/O stop.

## Instance

Two slots, four connections (five for the off-by-one witness), bursts of two
packets (three in the control, so an abandoned entry can be revisited away from
the head), a staging of three frames and a ring of four entries, twice the two
a running server keeps waiting. A stop owes up to eight packets (twelve in the
control) against a staging of three, so it spreads across steps.

| Config | Expected |
|---|---|
| `MCServerStopSpread.cfg` | pass, every invariant (bursts of three) |
| `MCServerStopSpreadLive.cfg` | pass, `StopFinishes` under `FairSpec` |
| `MCServerStopSpreadBrokenSingleShot.cfg` | `FullBurstBeforeStopped` violated |
| `MCServerStopSpreadBrokenRingSmall.cfg` | `RingHolds` violated |
| `MCServerStopSpreadBrokenSteadyCapOffByOne.cfg` | `RingHolds` violated |
| `MCServerStopSpreadBrokenNoSkip.cfg` | `SealedWithinBurst` violated |
| `MCServerStopSpreadBrokenUnreachableStalls.cfg` | `StopFinishes` violated (deadlock check off: the stall is a stuck state) |

## The reversed witnesses

`Defect = "singleshot"` is the stop before the spread: every burst staged in
the stop call whatever the room, a refused start dropping its packet, and the
I/O told to stop at once. TLC's trace: a client connects, the server stops,
and the I/O stop follows with the client's burst unsent. Against the code
before this model, `rocketnet_server_stop` sealed every connected client's
ten packets through `rocketnet_server_reserve_send_frame` and then called
`rocketnet_server_transport_stop`; a staged start past the bank returned NULL,
the rest of that burst was dropped, and at 1000 clients io_uring's 2048-slot
array took about 204 clients' bursts and gso's 4096-slot bank about 409.

`MCServerStopSpreadBrokenRingSmall.cfg` is the ring of `SteadyCap` entries
the code had: two kicked bursts wait, the stop appends the two connected
clients, and the fourth append lands on a live entry.

`Defect = "steadycapoffbyone"` defers while `count <= SteadyCap`: three
bursts wait, and the stop's two appends overrun the ring of four.

`Defect = "noskip"` seals the entry at the cursor even when it is complete.
TLC's trace at bursts of three: two clients stop, client 1's first packet is
sealed, client 2's destination is lost and its burst abandoned (marked
complete), client 1's second packet is sealed, the destination is found
again, and the pass seals client 2's entry to four. In the code the skip is
the `burst->sealed == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS` test at the top
of the drain's loop.

`Defect = "unreachablestalls"` ends the drain at an unreachable start as at a
full staging: the abandoned burst waits forever and STOPPING never reaches
STOPPED.

Each trace was replayed by hand against `rocketnet_server_transport_drain_disconnect_bursts`
and `rocketnet_server_transport_stop_step`.

## Worst case at the defaults

At `ROCKETNET_MAX_CLIENTS` 1000, a stop owes at most 1000 waiting bursts and
1000 connected clients' bursts of ten: 20000 packets. The smallest staged
bank is 2048 frames (io_uring's array, the IOCP send operations and the RIO
fill phase; gso and XDP take 4096). When each flush's frames are finished by
the next step, the stop needs `ceil( 20000 / 2048 ) = 10` steps (one more when
frames staged before the stop still fill the bank), 5 with only the 1000
connected clients' 10000 packets, plus the polls until the workers exit. A
transport that holds its frames longer only spreads the same steps further;
each step's work stays bounded by the room.

## Scope

The model abstracts the seal itself, the key wipe and the allowance of a
running update (any interleaving of `SealOne` covers every allowance). Room
is one bank; XDP reports its fullest queue, so its room is conservative. The
I/O's own stop (the transport's workers and its last flush) is its
transport's model's subject.
