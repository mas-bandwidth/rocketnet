# RNPacketLease: the packet lease table

`RNPacketLease.tla` models `rocketnet_packet_leases_t` in
`sdk/rocketnet_packet_lease.h`: `insert`, `resolve`, `take`, `retired` and
`reset`. The server and client transports use one table per transport. Game
threads call `insert` (`start_packet`), `take` and `resolve` (`finish_packet`,
`abort_packet` and the capacity query) and `retired` (a refused finish)
concurrently during the fill phase. Only the joined main thread calls `reset`.

## The abstraction

- **Entries.** `Capacity` entries, each a state word and the leased context's
  fields (`fr`, `tk`, `vl`, `bk`: frame, ticket, the other fields collapsed to
  one value, backend id). The state word is `epoch * 4 + phase`, in a word of
  `WordMod` values (2^64 in the code), with phase 0 empty, 1 writing, 2 active
  and 3 consumed. An entry is of the current epoch when `state >> 2` equals the
  epoch.
- **Probe.** `Hash(frame)` is `(frame - 1) mod HashSpread`; `HashSpread = 1`
  makes every frame collide, the worst case. The probe steps to
  `(slot + 1) mod Capacity` for at most `Capacity` entries.
- **Insert.** `StartInsert`, then `IUsed` (the `used` fetch-add, refusing at
  `UsedLimit`), `ITicket`, `ILoad` (skip entries of the current epoch), `ICas`
  (claim by compare-and-swap, or continue the probe), `IWrite` (the four fields
  in any order) and `IPublish` (the release store of active).
- **Resolve and take.** `StartResolve`, `RLoad` (another epoch ends the
  chain; writing starts the spin), `RSpinWait` and `RSpinDone`, `RCmp` (read
  the fields, continue on a frame or ticket mismatch, refuse on another
  mismatch) and `RCommit` (take swaps active to consumed; resolve tests the
  loaded state). The supplied context is a copy of a published lease, exact or
  with its value changed. `ValidCallers` limits copies to leases of the
  current fill; without it they may be stale, which the code treats as caller
  misuse.
- **Retired.** `StartRetired`, `TLoad`, `TRead` (read the fields, the
  code's relaxed loads) and `TValidate` (the `fetch_add( 0, release )` reread
  of the state: under `RetiredValidates` a changed state discards the entry).
  `AtomicFields` says the entry's fields are atomic. Without it, `TRead` of an
  entry in phase 1 or rewritten since `TLoad` is a data race, and so is a
  `TValidate` that finds the state changed: the insert that claimed the entry
  since `TLoad` stores the fields unordered with the read, whether or not the
  two overlapped in time.
- **Reset.** `Reset` runs only when every thread is idle (the main thread is
  joined). It bumps the epoch, which stops at `EpochLimit` and then is 0, and
  zeroes `used`.
- **Contract (ghost).** `issued` records each lease when `IPublish` publishes
  it, `takes` counts consumes and `gen` counts resets.
- **Not modelled.** The ticket source's exhaustion, the real hash function
  (the model takes the worst collision case), and the memory model: a step
  reads or writes one field atomically and steps interleave in any order,
  which covers every order the C++ operations can take but not their data
  races, which `NoRacyRead` states.

`Done` is the terminal stutter once every thread has used its operations and
every reset is done.

## Properties

- `NoWrongSuccess`: take, resolve and retired succeed only for a context
  exactly equal to a lease of the current fill (of an earlier fill, never
  consumed, for retired).
- `TakeOnce`, `PublishedIntact`, `ChainIntact`: a lease is consumed at most
  once, its entry is never overwritten while its epoch is current, and every
  entry between its hash and its slot is of the current epoch, so the probe
  reaches it.
- `ProbeBound`: the entries of the current epoch never exceed
  `Capacity / 2`. `InsertFindsSlot`: insert never runs out of probes.
- `NoRacyRead`: no thread's field read is a data race with another thread's
  stores. `retired`'s reads count only with plain fields (`AtomicFields =
  FALSE`); `resolve`'s reads are checked as plain: it reads only entries of the
  current epoch, which no insert rewrites.
- `SpinBounded` (liveness): a thread that is spinning stops spinning, when
  every thread is scheduled (vacuous for the code now that it does not spin).
- `ResolveCompletes` and `InsertCompletes` (liveness): thread 1's call
  completes, whatever the other threads do. `SpecSolo` schedules thread 1 only;
  thread 2 may stop between any two steps, the way a preempted thread does.

## What bounded the spin, and the fix

`resolve` used to wait (`while ( ( state & 3 ) == 1 )`) on an entry whose writer
had claimed it and not yet stored active. Three results:

1. **The wait was bounded by the writer's steps, not by the table.** The
   writer's window between the claim and the release store is four field
   writes and one store, with nothing that waits, and the spinner holds nothing
   the writer needs, so there was no deadlock.
2. **It was unbounded if the writer was paused.**
   `MCRNPacketLeaseBrokenSpinPausedWriter.cfg` (`OnWriting = {"spin"}`, the
   reversed witness) fails `ResolveCompletes` with valid callers only. The
   trace:
   - thread 2 starts an insert, takes entry 0 by compare-and-swap (state
     writing), writes all four fields, and stops before the release store;
   - thread 1 inserts its own lease: `ILoad` finds entry 0 of the current
     epoch and skips it, and the lease publishes at entry 1;
   - thread 1 resolves its own lease: the probe from the hash reaches entry 0
     first, finds it writing, and spins until thread 2 runs again.

   `insert` itself waits on no thread: `MCRNPacketLeaseInsertWaitFree.cfg`
   passes `InsertCompletes` with thread 2 free to stop anywhere.
3. **The wait was not needed.** The entry a resolve wants is never in phase 1:
   the supplied context's ticket comes from an `insert` that has returned, so
   its entry is active or consumed, and it stays so for the epoch. An entry in
   phase 1 on the way is some other lease's. The code now continues the probe
   past it (`OnWriting = {"skip"}`): `MCRNPacketLease.cfg` passes every
   invariant with changed and stale copies, and
   `MCRNPacketLeaseSkipWriting.cfg` passes `ResolveCompletes` and
   `InsertCompletes` with only thread 1 scheduled. A raised-priority spinner
   can no longer starve a preempted writer.

## `retired` and the reused entry

`retired` reads an old entry's fields after loading its state. A concurrent
`insert` that claims that same entry (it is of an earlier epoch, so `insert`
may reuse it) writes the fields at the same time. Before the fix `retired`
never reread the state:

- `MCRNPacketLeaseBrokenRetiredRace.cfg` fails `NoRacyRead`: the reader loads state
  active of epoch 1, the inserter claims the entry for epoch 2, and the reader
  then reads the fields.
- `MCRNPacketLeaseBrokenRetiredTorn.cfg` fails `NoWrongSuccess`. The trace: a lease
  (frame 1, ticket 1, value 1) is published in epoch 1 and reset. A thread
  inserts frame 1 with value 2: it claims the entry and writes only the value.
  Another thread calls `retired` with a copy of ticket 1 whose value was
  changed to 2: the frame, ticket and value now read as equal, and `retired`
  returns true for a context that is not the lease it was.
- The reread alone does not remove the race. With plain fields the read and
  a claiming insert's stores are unordered under the C++ memory model, a data
  race and undefined behavior, even when the reread discards what was read.
  Boehm's seqlock reader requires the data loads to be atomic.
  `MCRNPacketLeaseBrokenRetiredPlainFields.cfg` (the reread with plain fields)
  fails `NoRacyRead`. The trace: a lease is published in epoch 1 and reset.
  `retired` loads its state, an insert claims the entry for epoch 2, and
  `retired` reads the fields.
- `MCRNPacketLeaseRetired.cfg` (atomic fields and the reread) passes every
  invariant. The code is Boehm's seqlock reader. The entry's fields are
  `std::atomic`, stored and loaded relaxed. `retired` reads them, then rereads
  the state with a read-don't-modify-write (`fetch_add( 0, release )`) and
  compares the two states.
  - An insert that claimed the entry before the reread is seen in it.
  - An insert that claims it after the reread reads that release with its
    acquire compare-and-swap, so its stores follow the field reads.
  - ThreadSanitizer sees the plain-field race when a field read and a
    claiming insert's field stores are unordered. `test_lifecycle.cpp`
    `check_packet_lease_retired_race` drives that overlap in the tsan leg.

`retired` is reached only after `take` refused, which is caller misuse (a
finish after a reset, or a modified context), so the old effect was a wrong
classification of a misuse, not a wrong take.

## Cases and the checks against the code

| Config | Expected | What it shows |
|---|---|---|
| `MCRNPacketLease.cfg` | pass | The code's rule without `retired`, two threads (thread 1 makes two calls, thread 2 one), a four-entry table, all frames colliding, two resets, `resolve` skipping an entry being written: every invariant. |
| `MCRNPacketLeaseRetired.cfg` | pass | The code's `retired`: atomic fields (`AtomicFields`) and the state reread after the fields (`RetiredValidates`). |
| `MCRNPacketLeaseBrokenRetiredPlainFields.cfg` | `NoRacyRead` | `AtomicFields = FALSE`: the reread with plain fields, the race above. |
| `MCRNPacketLeaseBrokenRetiredRace.cfg` | `NoRacyRead` | `retired` before both repairs, plain fields and no reread: the race above. |
| `MCRNPacketLeaseBrokenRetiredTorn.cfg` | `NoWrongSuccess` | `retired` without the reread: the race above. |
| `MCRNPacketLeaseBrokenSpinPausedWriter.cfg` | `ResolveCompletes` | `OnWriting = {"spin"}`, valid callers, thread 2 paused: the former code. |
| `MCRNPacketLeaseInsertWaitFree.cfg` | pass | `insert` completes with only thread 1 scheduled. |
| `MCRNPacketLeaseSkipWriting.cfg` | pass | The code: resolve and insert complete with only thread 1 scheduled. |
| `MCRNPacketLeaseBrokenNoCas.cfg` | `PublishedIntact` | `UseCas = FALSE` |
| `MCRNPacketLeaseBrokenConsumeNoCas.cfg` | `TakeOnce` | `ConsumeCas = FALSE` |
| `MCRNPacketLeaseBrokenReadWhileWriting.cfg` | `NoRacyRead` | `OnWriting = {"read"}` |
| `MCRNPacketLeaseBrokenNoEpochBump.cfg` | `NoWrongSuccess` | `BumpEpoch = FALSE` |
| `MCRNPacketLeaseBrokenEpochUnlimited.cfg` | `PublishedIntact` | `EpochLimited = FALSE` |
| `MCRNPacketLeaseBrokenUsedOffByOne.cfg` | `ProbeBound` | `UsedLimit = 3` |

Each reversed witness was read against the code by hand (2026-10-29):

- **NoCas.** Two inserters load the same empty entry, both store writing and
  both write; the second overwrites the first's published lease. The code
  claims with `compare_exchange_strong`, so the loser continues the probe.
- **ConsumeNoCas.** Two threads load active, both store consumed, both
  succeed. The code's `take` uses `compare_exchange_strong` from active, so
  one wins (`test_lifecycle.cpp` `check_packet_leases` runs two threads on it).
- **ReadWhileWriting.** `resolve` reads `owned` while the writer is mid-copy.
  The code waits first.
- **NoEpochBump.** A lease of the previous fill is still current after reset.
  The code bumps the epoch in `reset`.
- **EpochUnlimited.** Epoch 3 shifted into a word that holds epochs to 2 reads
  back as 0, so a published entry no longer reads as current and the next
  insert claims it. The code stops at `UINT64_MAX >> 2`.
- **UsedOffByOne.** Three leases fill more than half of four entries. The code
  refuses at `used >= Capacity / 2`.
- **RetiredPlainFields** (2026-10-31). `retired` loads state active of epoch 1
  (`packet_lease.h`, the acquire load in `retired`). An insert's
  `compare_exchange_strong` then claims the entry for epoch 2, and `retired`
  reads the fields. With the fields a plain `rocketnet_packet_context_t`, the
  read and the insert's copy into it are unordered, which is a data race. The
  code reads `std::atomic` fields relaxed (`entry_t::load`).
