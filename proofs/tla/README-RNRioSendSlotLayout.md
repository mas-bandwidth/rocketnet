# RNRioSendSlotLayout

The RIO server transport's send slots at their layout, in
`sdk/rocketnet_server_rio_windows.cpp`. `RNRioDriver.tla` checks the slots
with a reservation that stops at the queue size; this model carries the
reservation counter at its own width, the three phases and the `send_ready`
FIFO of phases, so a counter that wraps is visible.

## State

| Model | Code |
|---|---|
| `next[p]` | `rio->next[p]`, advanced by one `fetch_add` per `rocketnet_server_rio_start_packet` |
| `Width` | the counter's modulus: 0 for a counter that never wraps within the checked calls (64 bits), an even number for a signed counter (an `int`: 2^32) |
| `slot[p][s]` | slot `s` of phase `p`: free, reserved, finished (`slot_bytes` > 0) or posted (the kernel's) |
| `fill` | `rio->fill` |
| `pstate`, `pcount` | `phase_state`, `phase_count` |
| `readyArr`, `readyHead`, `readyCount` | `send_ready`, `send_ready_head`, `send_ready_count`; tail `( head + count ) % ROCKETNET_RIO_SEND_PHASES` |
| `tries` | ghost: `start_packet` calls in this open of the fill phase |
| `reissued` | ghost: a reservation returned a slot that was not free |
| `absReady` | ghost: the phases handed and not taken, in order |

## Actions and the code they mirror

- **`Reserve`**: `rocketnet_server_rio_start_packet`: `slot = next[fill]++`;
  outside `0 .. Q - 1` it is refused (SEND_FULL), otherwise reserved. Every
  call advances the counter, refused or not.
- **`Finish(s)`**: `rocketnet_server_rio_finish_packet` (finished) or
  `rocketnet_server_rio_abort_packet` (free, never posted).
- **`SendPackets`**: `rocketnet_server_rio_send_packets` with the fill threads
  joined: `count = next[fill]`, nothing when `count <= 0`, clamped to `Q`; the
  phase is handed to the `send_ready` tail only when the next phase is idle,
  then `next[next] = 0` and `fill = next`.
- **`TakeAndPost`**: the WTake step of `rocketnet_rio_worker_pass` and
  `rocketnet_rio_post_phase`: the `send_ready` head advances modulo 3; each
  finished slot below the count is posted, the others below it zeroed.
- **`Complete(p, s)`**: `rocketnet_rio_dequeue_send`: a completion frees its slot.
- **`Retire(p)`**: the WPublish step of `rocketnet_rio_worker_pass`: a posted
  phase with nothing outstanding is idle.

While main cannot hand the fill phase (the next phase still has sends
outstanding), the fill threads keep calling `start_packet`, and nothing bounds
how many calls that is but the stall's length.

## Invariants

- **`NoReissue`**: a reservation returns only a free slot: no slot is handed
  to a second packet while it is reserved, finished or the kernel's.
- **`ReadyMatches`**: the `send_ready` positions from the head are the phases
  handed and not taken, in hand order, and exactly the phases in READY.
- **`OneFill`**: exactly one phase is open to the fill threads, and it is
  `fill`.

## The instances

`Q = 2` slots per phase, the code's three phases, at most seven calls per open
of a phase and four hands (so the `send_ready` FIFO wraps). Terminal states are
the harness's bound (`ignore-terminal`).

- **`MCRNRioSendSlotLayout.cfg`** (`Width = 0`): the counter as a 64-bit
  counter, which 2^64 calls cannot wrap in any stall. Every invariant holds.
- **`MCRNRioSendSlotLayoutBrokenIntWrap.cfg`** (`Width = 6`, the values
  -3 .. 2): the counter as an `int`, scaled down. Must fail `NoReissue`.

## The reversed witness, checked against the C

TLC's trace: two calls reserve slots 0 and 1; the third returns 2 and is
refused, leaving the counter at 3, which wraps to -3; three calls return -3,
-2 and -1 and are refused (`slot < 0`); the seventh returns 0 and reserves
slot 0 a second time while the first packet holds it. In the code
`std::atomic<int> next` makes the same walk at 2^32 calls: past
`ROCKETNET_RIO_SEND_QUEUE_SIZE` every call is refused, at `INT_MAX` the
`fetch_add` wraps to `INT_MIN`, the negative values are refused by the lower
bound, and at 0 `rocketnet_server_rio_start_packet` returns slot 0's frame
again after zeroing `slot_bytes` of a packet already finished, so two packets
are written into one frame and the first is never sent. The counter also
reads as `count <= 0` to `rocketnet_server_rio_send_packets` while it is
negative, so finished packets are not handed for that time. A counter that
cannot wrap (`MCRNRioSendSlotLayout.cfg`, the same calls) never does either.

## Finding

The `int` counter is a real bug at 2^32 `start_packet` calls within one
stall; the fix is a 64-bit counter, which the control checks.
