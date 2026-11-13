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
- the claim, a compare-and-swap on the count of the queue's producer bank that
  never moves it past `S`: an index at `S` is refused. It is two steps, `Load`
  (read the counter) and `Claim` (the swap), so another thread's claim can fall
  between them, and a lost swap loads again;
- the slot computed from a pointer, against the producer of the queue the id
  names, refused unless the offset is non-negative, a whole number of strides
  and inside the bank, all tested before the division. A caller may hand finish
  a pointer a byte off the one start returned (a contract violation), and it
  must fail closed;
- the turn-over at the frame boundary (`Flush`, send_packets with the fill
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
producer are exactly `0..count-1`, the slots the worker reads), with `TypeOK`
and `OneProducer`.

Status: built. sdk/rocketnet_server_gso_linux.cpp implements it and cites each
action (`Load` and `Claim` at start, `Finish` at finish and abort, `Flush` at
the publish, `WorkerReturn` at the return; `Done` is the bound's terminal
stutter, an environment action). The code is bound to the model by the trace
check (`TraceRNGsoQueueBankLayout.tla`, tests/tla_trace_gso.cpp, `tla_trace
--layer gso`, README-TraceCheck.md): its plant, finish and abort dividing the
pointer's offset before testing it, is rejected at the first finish a byte
early on a slot past 0.

## The instance (MCRNGsoQueueBankLayout.tla)

- `Q` = 2 queues, each with `NB` = 3 banks (`ROCKETNET_GSO_TX_BANKS`) of `S` = 3
  slots; `Stride` = 4.
- 2 fill threads, each making 2 starts per fill (`Demand`), one destination on
  each queue.
- A caller may hand finish a pointer 1 byte early, exact, or 1 byte late.
- `MaxFlush` = 2 in the control. At 3 (`MCRNGsoQueueBankLayoutFlush3.cfg`) a
  queue turns over onto a bank its worker returned; that instance is 32.2M
  distinct states, over the 110 s per-case budget, and is a `bench` case with
  declared debt.

Each switch of the model is the code's in the controls; each witness changes
one: `ClaimCmp` ("ge"), `ClaimRule` ("cas"), `SlotRule` ("check"), `IdRule`
("mul"), `FlushRule` ("joined").

## Results (gx86, an AWS x86 bench; pinned jar 936a2620..., pinned tlacheck 0c97d595, 2 workers, 2026-11-12; RUNS.tsv)

| cfg | expected | result | states generated (distinct) | seconds |
|---|---|---|---|---|
| MCRNGsoQueueBankLayout.cfg | pass | pass | 7,285,611 (1,323,990) | 26.3 |
| MCRNGsoQueueBankLayoutFlush3.cfg | pass | timeout at the 110 s budget: FAIL, exit 124, declared bench debt | - | 110.2 |
| MCRNGsoQueueBankLayoutBrokenClaimPastEnd.cfg | InBounds | InBounds violated | 6,817 (2,915) | 1.1 |
| MCRNGsoQueueBankLayoutBrokenBlindStore.cfg | NoSharedSlot | NoSharedSlot violated | 69 (49) | 0.8 |
| MCRNGsoQueueBankLayoutBrokenDivideFirst.cfg | NoForeignWrite | NoForeignWrite violated | 204 (118) | 0.8 |
| MCRNGsoQueueBankLayoutBrokenIdAdd.cfg | RouteMatches | RouteMatches violated | 204 (118) | 0.9 |
| MCRNGsoQueueBankLayoutBrokenOpenFlush.cfg | NoWorkerWrite | NoWorkerWrite violated | 432 (259) | 0.9 |
| MCTraceRNGsoQueueBankLayoutSelf.cfg | pass | pass | 7 (6) | 0.9 |
| MCTraceRNGsoQueueBankLayoutSelfBrokenTurn.cfg | TraceConsistent | TraceConsistent violated | 5 (5) | 0.7 |

Deadlock is checked in every case (no cfg turns it off): `Done` stutters at the
bound, and no other state is without a successor.

The three-flush instance run to the end outside the record runner (the same
bench, the pinned jar, 28 workers, deadlock checked): pass, 190,010,293 states
generated, 32,245,891 distinct, depth 55, 1 min 47 s. With tlacheck's two
workers it does not fit the 110 s per-case budget, so its record stays the
bounded FAIL/124 of a `bench` case.

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
- **OpenFlush.** Thread 2 has loaded index 1 of queue 0's bank 0 when the flush
  turns queue 0 over while the claim is in flight; bank 0 is now its worker's,
  and thread 2's claim writes slot 1 of it. The code turns over only in
  send_packets, after the fill threads joined, and only onto a free bank.

## The trace check

`tests/tla_trace_gso.cpp` compiles the transport's own source into the walk and
drives the real `start_packet`, `finish_packet`, `abort_packet`,
`send_packets` (publish), `take_send` and `return_send` on the
`rocketnet_server_gso_test_open` fixture with two queues: no socket, no thread,
no clock. It reads every model variable from the lanes' own fields after each
step (`own` from the producer and the free FIFO, `cnt` from each bank's
`num_packets`, `turn` from the lane), the environment's choices with them.
`ROCKETNET_GSO_SEND_QUEUE_SIZE` is 3 in that file only, as the io_uring walk
creates its banks with 3 slots; every other constant is the code's (`Stride` =
`ROCKETNET_FRAME_STRIDE`, `NB` = `ROCKETNET_GSO_TX_BANKS`). The trace instance
is 2 queues, 2 threads making 4 starts each per fill, 3 destinations (two on
queue 0 by the code's hash), 6 flushes.

`Load` is a record of its own: the queue by the code's hash and the producer
and count as the lane holds them. At `Claim` the walk calls start; a counter
another thread moved since the load is the swap that lost, recorded as the
model's reload (`compare_exchange_weak` reloads inside the one call), and an
index at `S` is a start the code must refuse. A finish or abort writes a
`packet_bytes` no slot holds yet, and the walk finds the slot it wrote by
comparing every slot before and after. Returns are offered one step in four,
and in a fill drawn with a slow worker not at all, so some flushes find no free
bank and keep their producer.

At seeds 1-4 (the per-commit job) the traces are 141 to 171 records and each
passes in 5 to 7 s; the plant's traces fail `TraceConsistent` at the first
finish a byte early on a slot past 0, within 2.5 s. 256 seeds (1000-1255)
passed clean and rejected the plant. The walk prints what it did (`WALK`):
at seed 1, 53 loads, 19 claims, 4 lost swaps reloaded, 34 refused starts,
15 finishes and 4 aborts, 6 flushes with 5 turn-overs and 6 producers kept
for want of a free bank, and 5 returns.
