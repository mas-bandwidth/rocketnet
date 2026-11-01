# RNPortableTx: the portable server's transmit handoff

`RNPortableTx.tla` is the layout model of the transmit half of the portable
server transport in `sdk/rocketnet_driver_builtin.cpp`. The same code serves
the macOS msgx server. [`README-RNPortableRx.md`](README-RNPortableRx.md)
covers the receive half. The modelled functions are:

- `portable_send`, the copy of each packet into the producer bank;
- `portable_tx_publish`, the only ownership transition;
- `portable_tx_step`, one pass of the worker that sends the ready banks, and
  `portable_tx_worker`, the thread that runs it and waits between passes;
- the publish retry at the top of `portable_receive`;
- the TX half of `portable_stop` and `portable_stopped`.

The rule it checks is the bounded N-buffer handoff
(`docs/decisions/BUFFER-HANDOFF-DECISION-20261021.md`) with the code's
three banks:

- main owns one producer bank and fills it outside any lock;
- only bank indices change under the mutex;
- the copy, the socket send and the count reset run outside it;
- main only ever tries the mutex, so it never waits.

## The abstraction

RNPortableRx models the contract. This model carries the code's fields and
arithmetic, so it can see an index or a slot that the abstract queue cannot.

- **Queues.** `freeIdx`, `freeHead`, `freeCount` and `readyIdx`, `readyHead`,
  `readyCount` are `free_index`, `free_head`, `free_count`, `ready_index`,
  `ready_head` and `ready_count`. A head advances by `(head + 1) % N`. A
  tail write goes to `(head + count) % N`. `HeadMod` and `TailMod` are those
  two moduli; `N` is `NumBanks`, 3 in the code. The model also runs at 4.
  A position at or past `N` sets `oob`.
- **Cells.** `cells` is one array, bank `b` slot `s` at `b * Cap + s`. The
  code lays the banks' arrays end to end in one allocation
  (`portable_tx_storage_create`), so a write one past a bank's capacity lands
  in the next bank's first slot. The array has one cell past the allocation
  so the last bank's overflow is also visible. `count` is `bank->count`.
- **Contract.** `accepted` is the packets copied into a bank, in order.
  `sent` is what the worker passed to the socket. `stopDropped` is a
  producer stop counted dropped; it stays empty under the code's rule and
  fills only in the one-try witness.
  Packets dropped by `portable_send` itself (a full producer that cannot
  publish, or a send after stop) are never accepted and take no id.
- **Main.** `MCall` begins a `portable_send` of up to `Batch` packets. `MPut`
  copies one packet, or finds the producer full and publishes first.
  `MPutEnd` is the call's closing publish. `MRecv` is the publish retry in
  `portable_receive`. The publish is `MPubEmpty` (nothing to exchange),
  `MPubTry` then `MPubSwap` then `MPubAfter` (the try, the exchange under the
  mutex, the count reset and wake after the release) or `MPubBusy` (the try
  failed). `MReturn` is the return to the caller: a refused publish inside
  `MPut` drops the rest of the batch, and a refused final publish in stop
  keeps the producer for the next poll. `MStop`, `MStopQuit`, `MStopWake` are
  the steps of `portable_stop`: the final publish, then `quit`, then the
  wake. `MStopRetry` is a `portable_stopped` poll while the final producer
  waits: one more publish try, and `quit` once it succeeds. `MPollStopped` is
  `portable_stopped` after that.
- **Worker.** `WTop` is the blocking acquire. `WTake` is the exchange under
  the mutex: take the oldest ready bank, or observe empty and, in the code,
  `quit` in the same critical section. `WEmpty` exits or waits. `WWait` is the
  wait on the wake, with a timeout only when `Timeout` is TRUE. `WSend` sends
  one packet of the bank in flight. The Linux worker's retained
  `tx_consumer`/`tx_cursor` map to `inflight`/`sendpos` in this
  ownership projection: a transient
  refusal leaves the same bank and prefix in hand between attempts.
  A socket refusal without progress leaves that ownership projection
  unchanged. This is source correspondence, not a machine-checked refinement
  of socket outcomes or the runtime's retry-wake scheduling. `WSendDone` zeroes the count, and
  `WRetLock` and `WRetSwap` return the bank to the free queue.
- **Not modelled.** The packet bytes and addresses, the invalid-packet drop,
  the drop counters and `publish_refusal`, the socket's own failures, and the
  RX half. The mutex is a holder (`none`, `worker`, `main`), and the wake is a
  pending flag.

`Timeout` stands for the worker wait's 0.1 s bound. The liveness controls run
with `Timeout = FALSE`, so progress follows from the wake protocol alone.

## Properties

- `ExclusiveOwnership`: every bank is the producer, the worker's bank, or one
  position of exactly one queue.
- `ExactSend`: the packets sent, the unsent rest of the bank in flight, the
  ready banks in FIFO order, the producer and the packets dropped at stop,
  each read through the cell addresses, are exactly the packets accepted, in
  order. Nothing is lost, repeated or overwritten.
- `InBounds`: no queue position at or past `N` is read or written.
- `FreeBanksEmpty`: a bank in the free queue has count 0 when main takes it.
- `MainLockIsBrief`: main holds the mutex only inside the exchange.
- `QuitAfterFinalBank`: `quit` is set only after the final producer is
  published.
- `StopDropsNothing`: stop drops no packet; a refused final publish is tried
  again on each poll until it succeeds.
- `ExitSentAll`: when the worker exits, nothing is ready, in flight or in the
  producer.
- `EveryAcceptedSent` (liveness): every accepted packet is sent (stop drops
  none under the code's rule). This includes a batch deferred by a failed try, which the next
  `portable_send` or an idle `portable_receive` retries.
- `StopCompletes` (liveness): once stop begins, the worker exits.

The fairness assumptions are:

- weak fairness on the worker's steps and on main completing its calls;
- strong fairness on the worker's two blocking acquires, because main holds
  the mutex for an index swap only;
- strong fairness on main's try, and on main's calls to `portable_receive`.

Stop is not forced; `StopCompletes` is conditional on stop starting.

The runtime announces shutdown to the portable backend before waiting for
`send_idle`. This separate terminal-drain flag leaves disconnect-batch
admission open, while switching a retained socket suffix to its final
counted send attempt. It prevents persistent socket refusal from trapping
the outer stop before `portable_stop` can publish quit. The model's
`WSend` abstracts consumption by the socket layer; it does not prove UDP
delivery or model that layer's terminal refusal counters. Runtime controls
cover partial-prefix retention and persistent-refusal shutdown separately.

## Cases and the checks against the code

| Config | Expected | What it shows |
|---|---|---|
| `MCRNPortableTx.cfg` | pass | Three banks of two packets, calls of up to two, six packets (enough for a publish to find no free bank). All invariants and both liveness properties. |
| `MCRNPortableTxFourBanks.cfg` | pass | The same with four banks and a modulus of four. |
| `MCRNPortableTxBrokenFullOffByOne.cfg` | `ExactSend` | `FullSlack = 1` |
| `MCRNPortableTxBrokenWrapTail.cfg` | `ExclusiveOwnership` | `TailMod = 2` |
| `MCRNPortableTxBrokenNoWrap.cfg` | `InBounds` | `HeadMod = 0`, `TailMod = 0` |
| `MCRNPortableTxBrokenQuitAfterUnlock.cfg` | `ExitSentAll` | `QuitUnderLock = FALSE` |
| `MCRNPortableTxBrokenQuitFirst.cfg` | `ExitSentAll` | `PublishBeforeQuit = FALSE` |
| `MCRNPortableTxBrokenNoRetryOnIdle.cfg` | `EveryAcceptedSent` | `RetryOnIdle = FALSE` |
| `MCRNPortableTxBrokenNoWake.cfg` | `EveryAcceptedSent` | `WakeOnPublish = FALSE` |
| `MCRNPortableTxBrokenStopDrops.cfg` | `StopDropsNothing` | `PublishUntilSent = FALSE` |

Each reversed witness was read against the code by hand (2026-10-29):

- **FullOffByOne.**
  - In the trace, main copies packets 1, 2 and 3 into bank 0, whose capacity
    is 2. Packet 3 goes to cell 2, which is bank 1's first slot. Main then
    publishes bank 0 and takes bank 1 as the producer, and copies packet 4 to
    cell 2. The worker reads bank 0's third slot, which is cell 2: it sends
    packet 4 where packet 3 belongs, and packet 3 is lost.
  - In the code, `portable_send` tests `bank->count == server->tx_capacity`
    before every copy, and `portable_tx_storage_create` places bank `i + 1`'s
    array directly after bank `i`'s, so the slot at `tx_capacity` is the next
    bank's slot 0. The test as written never reaches it.
- **WrapTail.**
  - In the trace, main publishes twice. Then the worker returns bank 0 with
    `free_head = 2` and `free_count = 0`. The tail goes to position
    `(2 + 0) % 2 = 0` instead of 2. The queue reads position 2, a stale entry,
    so bank 0 is in the free queue twice and bank 1 is nowhere after the
    second return.
  - In the code, the tail is `( free_head + free_count ) % PORTABLE_TX_BANKS`
    in `portable_tx_step`, and the ready tail the same in
    `portable_tx_publish`.
- **NoWrap.**
  - In the trace, the second return computes position `2 + 1 = 3` in a
    three-entry array, and `oob` is set.
  - In the code, every head advance and every tail position is reduced
    modulo `PORTABLE_TX_BANKS`.
- **QuitAfterUnlock.**
  - In the trace, the worker takes the mutex and finds the ready queue empty.
    It releases the mutex. Main calls stop, publishes the producer (ready
    count 1), sets `quit`, and only then does the worker read `quit`, see it
    set and exit with a bank ready.
  - In the code, this is the race fixed by commit `f0ee90fe`. The worker
    stores `exit_empty = quit` under the mutex in the same critical section as
    the empty test, and `rocketnet_portable_tx_test_handoff` replays the
    exact schedule with `after_worker_empty`.
- **QuitFirst.**
  - In the trace, stop sets `quit` and the wake before it publishes. The
    worker finds empty and quit, exits, and main then publishes the producer
    into a queue nobody reads.
  - In the code, `portable_stop` stores `quit` (`portable_stop_quit`) only
    after `portable_tx_publish` has succeeded, in the stop call or in a later
    `portable_stopped` poll.
- **StopDrops.**
  - In the trace, stop's one publish try is refused (the worker holds the
    mutex, or no bank is free while it sends) and the producer is counted
    dropped.
  - In the code before 2026-10-31, `portable_stop` made that one try and
    counted the producer under `PORTABLE_TX_DROP_STOPPING`. Now a refused try
    sets `stop_publish_owed`, and `portable_stopped` tries again on each poll;
    `rocketnet_portable_tx_test_stop_publishes_across_polls` replays it with no
    free bank until the faked worker returns one.
- **NoRetryOnIdle.**
  - In the trace, main copies a packet while the worker holds the mutex. The
    call's closing try fails and the packet stays in the producer. Main then
    only calls `portable_receive`. Nothing publishes, and the worker waits.
  - In the code, `portable_receive` calls `portable_tx_publish` before its
    count and stop checks, so an idle update retries the deferred batch.
- **NoWake.**
  - In the trace, the worker finds the ready queue empty and waits. Main
    publishes a bank and the worker is never signalled. With no timeout it
    never takes the bank.
  - In the code, `portable_tx_publish` calls `rocketnet_platform_wake_set`
    after releasing the mutex, which the worker's wait returns on.

The unit cases `rocketnet_portable_tx_test_handoff`,
`rocketnet_portable_tx_test_sizing`, `rocketnet_portable_tx_test_drop_paths` and
`rocketnet_portable_tx_test_stop_publishes_across_polls`
drive these transitions on one thread with a faked worker and no socket.
