# RNGsoQueueBankLayout: the gso per-queue send staging at the code's layout

`RNGsoQueueBankLayout.tla` models layer L2 of the gso push: each queue's send
staging in `sdk/rocketnet_server_gso_linux.cpp`. It covers what
`rocketnet_server_gso_start_packet`, `finish_packet`, `abort_packet` and the
producer turn-over in `rocketnet_server_gso_publish_send` share:
- the destination's queue (`rocketnet_server_gso_send_queue`, the
  address-and-port hash the XDP transport uses);
- the packet id `( turn * S + slot ) * Q + q`, where `turn` is the queue's
  producer turn-over count, written only by main while no fill runs, so no id
  counter is shared;
- the claim, one fetch-add on the 64-bit count of the queue's producer bank
  (`rocketnet_send_queue_claim`): the index it returns is the slot, and an
  index at or past `S` is refused and leaves the count past `S` until the
  worker resets the bank. It is one step, `Start`, so no other claim can fall
  inside it, and every reader of the count takes `min(count, S)`;
- the slot computed from a pointer, against the producer of the queue the id
  names, refused unless the offset is non-negative, a whole number of strides
  and inside the bank, all tested before the division. A caller may hand finish
  a pointer a byte off the one start returned (a contract violation), and it
  must fail closed;
- the turn-over at the frame boundary (`Flush`, send_packets with the game threads
  joined): a queue that staged anything hands its producer to its worker and
  takes a free bank, whose count the worker reset when it gave it back
  (`WorkerReturn`); a queue with no free bank keeps its producer, and main
  never waits.

The banks of every queue are laid out end to end, the worst case for a stray
pointer: one byte before a bank lands inside its neighbour. The order of the
hand-off is not here: that is `RNGsoHandoff.tla` over `RNBankFifoLayout.tla`,
per queue. Here only ownership matters (`own`: producer, worker or free).

Checked claims: `InBounds` (no slot outside `0..S-1` is handed out),
`NoSharedSlot` (no slot handed to two claims in one fill), `NoForeignWrite` (a
finish or abort writes only its own claim's slot, or nothing), `RouteMatches`
(finish finds the queue start chose), `NoWorkerWrite` (no claim or finish
writes a bank its worker owns), `StagedMatches` (the claimed slots of each
producer are exactly `0..min(count, S)-1`, the slots the worker reads), with
`TypeOK` and `OneProducer`. With `Start` one step and the turn-over only with
the fill joined, `Start` and `Finish` write only the producer, so
`NoWorkerWrite` can no longer fail: it stays checked, and
`OpenFlush` is caught one step later by `NoForeignWrite`.

Status: built. sdk/rocketnet_server_gso_linux.cpp implements it and cites each
action (`Start` at start, `Finish` at finish and abort, `Flush` at
the publish, `WorkerReturn` at the return; `Done` is the bound's terminal
stutter, an environment action). The code is bound to the model by the trace
check (`TraceRNGsoQueueBankLayout.tla`, tests/tla_trace_gso.cpp, `tla_trace
--layer gso`, README-TraceCheck.md, record format v2): its plant, finish and
abort dividing the pointer's offset before testing it, is rejected at the
first finish or abort that writes another claim's slot.

## The instance (MCRNGsoQueueBankLayout.tla)

- `Q` = 2 queues, each with `NB` = 3 banks (`ROCKETNET_GSO_TX_BANKS`) of `S` = 3
  slots; `Stride` = 4.
- 2 game threads, each making 2 starts per frame (`Demand`), one destination on
  each queue (`Dests` = {0, 1}: destination `d` is on queue `QueueOf(d)` =
  `d % Q`; the trace walk picks addresses the code's hash puts there).
- A caller may hand finish a pointer 1 byte early, exact, or 1 byte late
  (`SkewSpan` = 1, and `Skew` is the definition `-SkewSpan..SkewSpan`: a cfg
  set holds no negative number).
- What start returns is `Start`'s argument `r`, `StartResult`: the slot's
  address and the packet id, or `None` for a refused start.
- The count is unbounded in the code (64 bits, reset when the worker returns
  the bank; a producer kept for want of a free bank keeps its count); in the model
  it is bounded by `CntMax`, every start a fill can make, and `CounterWrap` = 0
  keeps it unwrapped.
- `MaxFlush` = 2 in the control. At 3 (`MCRNGsoQueueBankLayoutFlush3.cfg`) a
  queue turns over onto a bank its worker returned; that instance is over the
  110 s per-case budget, and is a `bench` case with declared debt.

Each switch of the model is the code's in the controls; each witness changes
one: `ClaimCmp` ("ge"), `ClaimRule` ("add"), `CounterWrap` (0), `SlotRule`
("check"), `IdRule` ("mul"), `FlushRule` ("joined").

## Results (aws-mut-4, x86; pinned jar 936a2620..., pinned tlacheck 0c97d595, 2 workers, 2026-11-14, each case its own bounded run; RUNS.tsv)

The move from the two-step compare-and-swap claim to the one-step fetch-add
(`Start`) dropped the control from 1,323,990 distinct states to 653,994: the
states between a thread's load and its swap are gone.

| cfg | expected | result | states generated (distinct) | seconds |
|---|---|---|---|---|
| MCRNGsoQueueBankLayout.cfg | pass | pass | 4,020,027 (653,994) | 17.0 |
| MCRNGsoQueueBankLayoutFlush3.cfg | pass | timeout at the 110 s budget: FAIL, exit 124, declared bench debt | - | 110.3 |
| MCRNGsoQueueBankLayoutBrokenClaimPastEnd.cfg | InBounds | InBounds violated | 908 (382) | 0.8 |
| MCRNGsoQueueBankLayoutBrokenBlindStore.cfg | NoSharedSlot | NoSharedSlot violated | 69 (49) | 0.7 |
| MCRNGsoQueueBankLayoutBrokenDivideFirst.cfg | NoForeignWrite | NoForeignWrite violated | 33 (23) | 0.7 |
| MCRNGsoQueueBankLayoutBrokenIdAdd.cfg | RouteMatches | RouteMatches violated | 33 (23) | 0.7 |
| MCRNGsoQueueBankLayoutBrokenOpenFlush.cfg | NoForeignWrite | NoForeignWrite violated | 11,759 (3,355) | 1.0 |
| MCRNGsoQueueBankLayoutBrokenNarrowCounter.cfg | NoSharedSlot | NoSharedSlot violated | 200 (147) | 0.8 |
| MCTraceRNGsoQueueBankLayoutSelf.cfg | pass | pass | 6 (5) | 0.7 |
| MCTraceRNGsoQueueBankLayoutSelfBrokenTurn.cfg | TraceConsistent | TraceConsistent violated | 4 (4) | 0.7 |

Deadlock is checked in every case (no cfg turns it off): `Done` stutters at the
bound, and no other state is without a successor.

The three-flush instance of the two-step claim, run to the end outside the
record runner (bench-nyc-6, the pinned jar, 28 workers, deadlock checked,
2026-11-13), passed at 32,245,891 distinct states, depth 55, in 1 min 47 s.
With tlacheck's two workers the one-step model still does not fit the 110 s
per-case budget, so its record stays the bounded FAIL/124 of a `bench` case.

## The witnesses, as TLC printed them

Each was checked by hand against the broken rule.

- **ClaimPastEnd.** The claim refuses only an index past `S`, so the fourth
  claim on a bank is handed slot `S`, one past the array.
- **BlindStore.** Both threads load index 0 of queue 0's producer. Thread 1's
  claim takes slot 0; thread 2 stores 1 without comparing and is handed slot 0
  too.
- **DivideFirst.** Thread 2 holds slot 1 (pointer 4, stride 4) and finishes
  with the pointer a byte early. The offset 3 truncates to slot 0, thread 1's.
  The code tests the offset for a whole stride before dividing, so it refuses.
- **IdAdd.** The id is `turn * S + slot + q`. A claim on queue 0's slot 1 gets
  id 1, and finish takes `1 mod 2 = 1`, the wrong queue.
- **OpenFlush.** Thread 1 holds slot 0 of queue 0's bank 0 when the flush turns
  queue 0 over with the fill still open, and onto the next bank whatever its
  owner. Its finish meanwhile fails closed (pointer 0 lies before banks 1 and
  2), but the third turn-over brings bank 0 back as the producer, reset, and
  thread 1's finish writes slot 0 of the new fill, a slot it holds no claim
  on. The code turns over only in send_packets, after the game threads
  joined, and only onto a free bank.
- **NarrowCounter.** The counter wraps at 4 (`CounterWrap` = 4). Three starts
  take slots 0 to 2, a fourth is refused and moves the counter from 3 to 0,
  and the fifth is handed slot 0, which thread 1 still holds. The code's
  counter is 64 bits and reset when the worker returns the bank, so it never
  wraps.

## The trace check

`tests/tla_trace_gso.cpp` (record format v2, README-TraceCheck.md) drives the
library's own `start_packet`, `finish_packet`, `abort_packet`, `send_packets`
(publish), `take_send` and `return_send` on the
`rocketnet_server_gso_test_open_slots` fixture with two queues: no socket, no
thread, no clock. The fixture opens each producer bank with 3 slots
(`send_slots`, which the walk writes as `S`); every other constant is the
code's (`Stride` = `ROCKETNET_FRAME_STRIDE`, `NB` = `ROCKETNET_GSO_TX_BANKS`).
After each step the walk reads every model variable from the lanes as the
library holds them (`rocketnet_server_gso_test_send_lane`: `own` from the
producer and the free FIFO, `cnt` from each bank's `num_packets`, `turn` from
the lane), the environment's choices with them. The trace instance is 2
queues, 2 threads making 4 starts each per frame, 3 destinations (destination
`d` an address the code's hash puts on queue `d % 2`), a pointer up to a byte
off either way, and flushes bounded by the walk's length.

One record per call (`tla_step`), and the action a record names is the one
the call's result names. Each start is one `Start` record: the thread, the
destination and what start returned (the pointer and packet id, or `None`),
the queue the code's hash gave read back from the lane. A start at or past
`S` is refused and leaves the count past the slots, which the walk counts
(`past_end`) and the trace must reach (`CounterPastEnd`); a walk with none
fails. `Load` and `Claim` are the BlindStore witness's two steps and are not
traced. A finish or abort writes a `packet_bytes` no
slot holds yet, and the walk finds the slot it wrote by comparing every slot
before and after. The worker's take and return are one record, the bank the
take returned an argument. Returns are offered one step in four, and in a
fill drawn with a slow worker not at all, so some flushes find no free bank
and keep their producer. `Done` is untraced: the walk's flush bound is its
length, so the model's end is never reached. The row's reach predicates are
`BankFull`, `TurnedOverEveryBank`, `NoFreeBank` and `CounterPastEnd`.

The plant (`ROCKETNET_GSO_SEND_SLOT_PLANT`, in
`rocketnet_server_gso_send_slot`) divides the slot out of the pointer's
offset before any test, so a pointer a byte early on slot `s` writes slot
`s - 1`, another claim's. The walk reports it (`PLANT-AT`) at the record where
a finish or abort wrote a slot not its own; TLC must reject the plant's trace
there.

At seeds 1-8 (amd-3, 2026-11-14) every trace is 401 records (400 steps) and
TLC accepts each clean trace with 401 distinct states, about 7.5 s a trace
with the JVM's start. The plant fired 11 to 30 times a seed, first at records
6, 6, 5 and 6 at seeds 1-4, and TLC rejected each plant trace at that first
record. The walk prints what it did (`WALK`): at seed 1, 115 claims, 102
refused starts that each left the counter past the slots, 79 finishes and 36
aborts, 30 flushes with 41 turn-overs and 17 producers kept for want of a
free bank, and 38 returns. The planted rows `ci/tla-trace/gso-plant-accepted`
(the model letting a finish's foreign write pass) and
`ci/tla-trace/gso-start-returns-wrong` (start's packet id naming slot 1 for
slot 2) are red.
