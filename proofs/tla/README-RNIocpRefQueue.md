# RNIocpRefQueue: the IOCP reference queue at the code's layout

`RNIocpRefQueue.tla` models the bounded MPMC reference queue of the IOCP
server transport (`sdk/rocketnet_server_iocp_windows.cpp`:
`rocketnet_iocp_queue_initialize`, `rocketnet_iocp_queue_push` and
`rocketnet_iocp_queue_pop`) with the code's own arithmetic: the cells, their
sequences, the two free-running position counters and the signed difference
of the `uint64_t` counters. `free_sends` and `retry_receives` are this queue.
The `server-iocp-ref-queue` row in `COVERAGE.tsv` tracks the coverage.

## The layout

| Model | Code |
|---|---|
| `Cap`, `Cells` | `queue->capacity`, a power of two (`rocketnet_iocp_queue_initialize` asserts it) |
| `seq[i]`, `ref[i]` | `queue->cells[i].sequence`, `queue->cells[i].ref` |
| `enqueue`, `dequeue` | `queue->enqueue`, `queue->dequeue` |
| `Index(p)` | `position & ( queue->capacity - 1 )` |
| `Wrap`, `PosMod` | `uint64_t` arithmetic; `PosMod` is the code's 2^64 |
| `SDiff(a, b)` | `(int64_t) ( a - b )` |
| `ppos[p]`, `cpos[c]` | each thread's local `position` |
| `Ops` | the operations a queue carries: `send_queue_size` sends on `send_queue_size` cells, and every receive on a `retry_receives` of at least that many |

## Actions

| Action | Code |
|---|---|
| `PushStart` | `rocketnet_iocp_queue_push`: `position = enqueue.load` |
| `PushRead` | the sequence read and the signed difference: zero goes on to the CAS, above zero reloads `position`, below zero is the refusal (`return false`) |
| `PushClaim` | `enqueue.compare_exchange_weak( position, position + 1 )`; a failure reloads `position` |
| `PushPublish` | `cell->ref = ref`, then `sequence.store( position + 1 )` |
| `PopStart` | `rocketnet_iocp_queue_pop`: `position = dequeue.load` |
| `PopRead` | the sequence read: published, empty (`return false`) or passed |
| `PopClaim` | `dequeue.compare_exchange_weak( position, position + 1 )` |
| `PopTake` | `*ref = cell->ref`, then `sequence.store( position + capacity )` |

Each read, CAS and publish is its own step, so a thread can stall between
any two of them while the others run.

## The push a stalled pop refuses

A pop claims position q with its CAS and only then reads the reference and
stores q + capacity, which frees the cell for the push one round later.
While it sits between the two, the push at q + capacity reads sequence
q + 1, finds the difference below zero and returns false, with the queue
not full: every other operation can have gone round meanwhile. Two live
operations are enough, whatever the capacity. With the queue holding at
most `Cap` operations, as the code sizes it, that stalled pop is the only
way a push is refused (`RefusalOnlyWhilePopStalled`).

In the model a refused push keeps its operation and reloads `enqueue`, as a
passed push does, until the stalled pop has stored its sequence and the
cell is free. `RefusedNeverLost` holds every refused operation to its
producer, a cell or a pop, and under `SpecFair` every operation released is
queued and popped (`EveryOperationPopped`).

The code does not retry yet. `rocketnet_iocp_release_send` (on
`free_sends`) and `rocketnet_iocp_post_receive` (on `retry_receives`)
assert the push and, when it is refused, mark the operation `RETIRED`: a
send slot or a receive is gone for the life of the server. That is the
reversed witness `MCRNIocpRefQueueBrokenDropRefused.cfg`, and the retry is
owed in the code.

## Properties

- `NoOverwrite`: no publish writes over a reference still unread.
- `NoPhantom`: every pop returns a reference published and not yet taken.
- `WithinCapacity`, `CellsMatchPositions`, `InqIsCells`: the claimed
  positions are within a capacity of each other and each sits in its own
  cell, at its own sequence.
- `RefusedNeverLost`: a refused push's operation is still held, queued or
  taken.
- `RefusalOnlyWhilePopStalled`: with `Ops <= Cap`, a push about to be
  refused has a pop of the cell's last round claimed and not freed.
- `EveryOperationPopped` (temporal, under `SpecFair`): every operation
  released is queued and popped.

## Cases

Group `rniocprefqueue` in `CASES.tsv`; the records are in `RUNS.tsv`,
measured on a linux-amd64 bench (Java 21.0.12.1, the pinned JAR, two
workers). `PosMod = 32` in every case but the wrap witness.

| Case | Change | Expected | Exit | Distinct | Seconds |
|---|---|---|---:|---:|---:|
| `MCRNIocpRefQueue.cfg` | the code, two cells, two producers, two consumers, five operations, no bound on those live | pass | 0 | 1112080 | 15.6 |
| `MCRNIocpRefQueueCap4.cfg` | the code, four cells, two producers, one consumer, six operations, no bound | pass | 0 | 130234 | 4.4 |
| `MCRNIocpRefQueueStalledPop.cfg` | the code's sizing, two cells, at most two live, four operations, `SpecFair` | pass | 0 | 58768 | 7.1 |
| `MCRNIocpRefQueueStalledPopCap4.cfg` | the code's sizing, four cells, at most four live, one producer, two consumers, six operations, `SpecFair` | pass | 0 | 8422 | 2.5 |
| `MCRNIocpRefQueueBrokenCapacityNotPow2.cfg` | `Cap = 3` | `NoOverwrite` | 12 | 19 | 1.1 |
| `MCRNIocpRefQueueBrokenInitOffByOne.cfg` | `InitSlack = 1`: initialize stores i + 1 in cell i | `NoPhantom` | 12 | 9 | 1.1 |
| `MCRNIocpRefQueueBrokenPositionWrap.cfg` | `PosMod = 2 * Cap`: the counters wrap | `NoOverwrite` | 12 | 23682 | 2.2 |
| `MCRNIocpRefQueueBrokenDropRefused.cfg` | `DropRefused = TRUE`: the caller retires a refused operation, as the code does today | `RefusedNeverLost` | 12 | 320 | 1.2 |

Checked by hand on the bench and not committed as cases: with
`DropRefused = FALSE` at the stalled-pop sizes, an invariant `refused = {}`
fails (the refusal is reached, so `RefusedNeverLost` and
`RefusalOnlyWhilePopStalled` are not vacuous); `RefusalOnlyWhilePopStalled`
without its `Ops <= Cap` guard fails at `Ops = 4`, `Cap = 2` (a full queue
refuses with no pop stalled); and `EveryOperationPopped` fails under
`DropRefused = TRUE`.

## Counterexamples, checked by hand against the code

**BrokenDropRefused** (`Cap = 2`, one producer, two consumers, at most two
live): the producer pushes operation 1 at position 0 and operation 2 at
position 1 (cell sequences 1 and 2, `enqueue` 2). Consumer 1 claims
position 0 (`dequeue` 1) and stalls before its store. Consumer 2 claims
position 1, takes operation 2 and stores 1 + 2 = 3 in cell 1. Operation 3
is released (two live: 1 and 3); its push loads `enqueue` 2, reads cell
2 & 1 = 0, whose sequence is still 1, and `(int64_t) ( 1 - 2 )` is below
zero: `rocketnet_iocp_queue_push` returns false. On `free_sends` this is a
caller thread stalled in `rocketnet_iocp_acquire_send` between its CAS and
its store while another caller acquires the second send, sends it, and its
completion releases it: `rocketnet_iocp_release_send` asserts the push and
marks the send `RETIRED`, and the send slot never returns. Operation 3 is
in no cell, no thread and no pop.

**BrokenCapacityNotPow2**: with three cells `position & 2` sends positions
0 and 1 to cell 0, so the second push overwrites the first while it is
unread. **BrokenInitOffByOne**: cell 0 starts at sequence 1, so the first
pop reads it as published and returns a reference never pushed.
**BrokenPositionWrap**: a producer stalls between its sequence read and its
CAS while the others take `enqueue` a whole wrap of 2 * Cap; its stale CAS
succeeds and its publish overwrites an unread reference.
