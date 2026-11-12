# RNMsgxSocketTable

The macOS batched server transport's per-client socket table,
`sdk/rocketnet_msgx_socket_table.h`, at the code's layout. Two modules:

- `RNMsgxSocketTable.tla`: the slots, their words, the holders' passes, the
  retire and its quarantine, the sweep, the open budget, the cooldown table and
  the counters, over a kernel that hands descriptors out lowest free first.
- `RNMsgxAddressIndex.tla`: the address index alone (linear probing with the
  backward-shift delete), with every assignment of homes to its keys.

`TraceRNMsgxSocketTable.tla` binds the code to the first module: the walk in
`tests/tla_trace_msgx.cpp` runs the real header over a fake kernel and its
records must be a behaviour of the spec ([README-TraceCheck.md](README-TraceCheck.md)
has the mechanism).

## Why a close too early is a cross-client send

Every client the replay walk sees connected gets its own UDP socket, bound to
the server's port and connected to the client. POSIX hands out the lowest free
descriptor, so a descriptor closed now is the next one opened. A transmit
worker that resolved a client's descriptor and has not yet sent, or the
receive worker holding a poller event, would then send to or read from the
next client. The table keeps a retired socket open until every holder that
was inside a pass at the retire has left that pass, and stamps packets with
the slot's generation so a stale stamp resolves to the shared socket. The
model's kernel maps each descriptor to its peer, so that send is the event
`crossed`.

## State

| Model | Code |
|---|---|
| `word[s]`, `<<gen, fd + 1>>` | `slots[s].word`: generation high 32 bits, fd + 1 low |
| `deadRep[s]` | `slots[s].dead_report`: 0, or the reported generation + 1 |
| `state`, `fd`, `gen`, `addr` | `slots[s].state`, `.fd`, `.generation`, `.address` |
| `snap[s][h]` | `slots[s].snapshot[h]` |
| `pass[h]` | `pass[h]`: even quiescent, odd inside a pass |
| `deadReports` | `dead_reports` |
| `freeSlots`, `numFree` | `free_slots[]`, `num_free` (a stack) |
| `retired`, `numRetired` | `retired[]`, `num_retired` (compacted in place by the sweep) |
| `numOpen`, `index` | `num_open`, `index[]` |
| `coolAddr`, `coolUntil` | `cooldown_address[]`, `cooldown_until[]` |
| `counters`, `update`, `opensLeft` | `counters[]`, `update`, `opens_left` |
| `mpc`, `ms`, `mi`, `mk` | main's place inside a call; `"idle"` between calls |
| `kfd[f]`, `kdead[f]` | the kernel: closed, another descriptor of the process, or the peer; disconnected |
| `poller` | the caller's receive poller: `<<fd, slot, generation, address>>` |
| `pkts`, `cur`, `rep`, `rxEv`, `rxFd` | the fill threads' stamped packets and the holders' work |
| `crossed`, `onClosed`, `attempts` | history: a send or read reached another peer, or a closed descriptor; open calls |

## Actions and the code they mirror

- **`Mark(a)`**: `rocketnet_msgx_socket_table_mark`. Refused by the
  cooldown, a full table or the open budget (each counted); otherwise the
  open (`table->open`), refused by the kernel for a peer that already has a
  connected socket (EADDRINUSE: a retired socket still holds it) or when no
  descriptor is free (counted `OPEN_FAILED`, no slot taken), or the slot
  popped, the word stored, the address inserted and the poller registered.
- **`Forget(a)`**: `rocketnet_msgx_socket_table_forget`, the lookup and the
  first step of `rocketnet_msgx_socket_table_retire`: index remove, generation
  advance, word store.
- **`Retire`**: the retire's second step: the snapshot of every holder's
  pass, the slot in quarantine (the retired list), the counter. On the forget
  path the caller then deregisters (`ForgetDereg`); on the sweep's path its
  dead-report loop goes on.
- **`BeginUpdate`**: `rocketnet_msgx_socket_table_begin_update`.
- **`SweepBegin`**: `rocketnet_msgx_socket_table_sweep`, the exchange of
  `dead_reports`.
- **`RetireDead`**: one slot of the sweep's dead-report loop: the report
  exchanged; one about the open generation cools the address down, calls the
  retire hook (the caller deregisters) and runs the retire's first step.
- **`Close`**: one entry of the sweep's close loop: closed and freed once
  `rocketnet_msgx_socket_table_quiescent`, else kept at `retired[kept++]`.
- **`Stamp(a, h)`**: `rocketnet_msgx_socket_table_stamp` on a fill thread,
  between main's calls.
- **`HolderBegin(h)`**, **`HolderEnd(h)`**: `holder_begin`, `holder_end`.
- **`TxResolve(h, i)`**, **`RxResolve`**: `rocketnet_msgx_socket_table_resolve`
  on a stamped packet or a poller event.
- **`ReportDead(h)`**: `rocketnet_msgx_socket_table_report_dead`.

Environment actions, not code of this header: `TxSend` and `RxRead` (the
kernel delivers to the descriptor's current peer; a disconnected socket
answers ENOTCONN), `RxPoll` (the poller hands out a registered event),
`ForgetDereg` (the caller's deregistration after forget returns
`retired_fd`), `EnvOpen` and `EnvClose` (other descriptors of the process)
and `Disconnect` (ICMP).

The retire's snapshot loop is one step in the model. Each holder's snapshot
only decides that holder's quiescence, and the code reads each pass after the
word store, so any order of the per-holder reads is one of the model's
interleavings for that holder.

## Properties

- `NoCrossClient`: no send or read reached a peer other than the one the
  packet or event names.
- `NoSendOnClosed`: no send or read on a closed descriptor.
- `Accounting`: every open call is opened or counted refused; between calls,
  opened = closed + open + retired, and retires = closed + retired.
- `NoLeak`: between calls every slot is in exactly one of the free stack,
  open, or the retired list; every slot not free holds the socket connected
  to its address; every connected socket is in exactly one slot.
- `IndexMatchesOpen`: the index finds exactly the open slots.
- `WordMatchesSlot`: an open slot's word is its generation and descriptor,
  any other slot's its generation and no descriptor.
- `RetiredClosed` (liveness, `SpecFair`): a slot in quarantine is eventually
  closed and freed, with main sweeping and every holder finishing its pass.

`RNMsgxAddressIndex`: `LookupFindsOpen` (every inserted key is found, nothing
else is) and `NoEntryLost` (each key is in exactly one entry).

## Instances

The state space grows fastest with holders. No single instance holds three
slots, four addresses, three descriptors and two transmit holders with the
receive holder inside the 110 s budget, so each passing config holds one
part at full size:

| Config | Instance | What it holds |
|---|---|---|
| `MCRNMsgxSocketTable.cfg` | 2 slots, 2 addresses, 2 descriptors, TX holders {1, 2} and RX holder 3, one pass each, one packet, 2 opens | every holder against reuse of a slot and a descriptor |
| `MCRNMsgxSocketTableLayout.cfg` | 3 slots, 4 addresses (homes 6, 7, 7, 0 of 8: collision and wrap), 3 descriptors, one other descriptor of the process, 2 opens per budget and two `BeginUpdate`s | the index, free stack, EADDRINUSE and EMFILE, budget, counters |
| `MCRNMsgxSocketTableQuarantine.cfg` | 3 slots, 3 addresses, 3 descriptors, TX holder 1 and RX holder 2, one pass each, 3 opens | the retired list compacted around kept entries |
| `MCRNMsgxSocketTableDead.cfg` | 2 slots, 2 addresses, TX 1 and RX 2, ICMP, cooldown 1, 1 open per budget and one `BeginUpdate` | dead reports, stale reports, cooldown |
| `MCRNMsgxSocketTableLive.cfg` | 2 slots, TX 1 and RX 2, no packets, `SpecFair`, no VIEW | `RetiredClosed` |
| `MCRNMsgxSocketTableLivePacket.cfg` | 1 slot, TX 1 with a packet and RX 2, `SpecFair`, no VIEW | `RetiredClosed` with a packet in flight |

The safety configs use `SafetyView`, which replaces every value no later step
reads by a constant (a free slot's address, the free stack above `numFree`, a
cooldown entry already expired, a finished packet's detail), keeps the
counters only as the differences `Accounting` reads, and keeps a live
snapshot only as what `Quiescent` reads: whether the holder is still inside
the pass snapshotted (a pass only grows, so once it moves on it never equals
the snapshot again). The liveness configs run without the VIEW and override
`RefusedMarks` to FALSE: a refused mark changes only its counter, which no
guard reads, and leaving it out makes the state space finite.

Generations wrap at `GenWrap` (8 in the passing configs; 2^32 in the code).

## Reversed witnesses

Each switch is TRUE in the code (`SnapshotFirst` FALSE). Each broken config
sets one and lists only `TypeOK` and the invariant it must fail.

| Config | Change | Fails | The counterexample |
|---|---|---|---|
| `MCRNMsgxSocketTableBrokenCloseAtRetire.cfg` | `Quarantine = FALSE`: the retire closes at once | `NoCrossClient` | client 1 on slot 0, fd 0; TX holder 1 begins, resolves fd 0; forget closes fd 0 at the retire; mark client 2 opens the lowest free descriptor, fd 0; the send reaches client 2 |
| `MCRNMsgxSocketTableBrokenNoGenerationCheck.cfg` | `GenCheck = FALSE`: resolve ignores the stamp's generation | `NoCrossClient` | client 1 on slot 0; forget retires it (snapshot all quiescent); the RX holder begins and takes the event (fd 0, slot 0, generation 0) before the caller deregisters; the sweep closes fd 0; client 2 gets slot 0, generation 1, fd 0; the event resolves to fd 0 and the read is client 2's |
| `MCRNMsgxSocketTableBrokenSnapshotBeforeWord.cfg` | `SnapshotFirst = TRUE`: the passes are snapshotted before the word store | `NoCrossClient` | forget snapshots (all quiescent); the RX holder begins and resolves the old word, fd 0; the word store; the sweep closes fd 0 (the snapshot showed no pass); client 2 gets fd 0; the read is client 2's |
| `MCRNMsgxSocketTableBrokenExhaustionTakesSlot.cfg` | `RefusedOpenFree = FALSE`: a refused open takes a slot, uncounted | `NoLeak` | client 1 opened, forgotten, in quarantine; mark client 1 again: the kernel refuses (EADDRINUSE, the retired socket is still connected) and the slot is open with fd -1 |
| `MCRNMsgxSocketTableBrokenExhaustionTakesSlotAccounting.cfg` | the same | `Accounting` | the same six states: an open call neither opened nor counted refused |
| `MCRNMsgxSocketTableBrokenGenWrap2.cfg` | `GenWrap = 2`, one slot | `NoCrossClient` | a packet stamped for client 1 at generation 0 stays queued while slot 0 is retired twice (generation 1, then 0 again) and reopened for client 2; the stale stamp resolves to client 2's descriptor. In the code the wrap is 2^32 retires of one slot while one stamp is queued |
| `MCRNMsgxAddressIndexBrokenDeleteWithoutShift.cfg` | `DeleteShift = FALSE`: the delete only clears the entry | `LookupFindsOpen` | keys 0 and 1 both home at 0; 0 at entry 0, 1 at entry 1; remove 0 clears entry 0; the lookup of 1 stops at the empty entry 0 |

Each counterexample was read state by state against the header: the steps
are the code's calls in the order shown, and in the code without the change
each is refused where the model refuses it (the quarantine keeps fd 0 open
while holder 1 is in its pass; the generation check drops the event; the
word store before the snapshot makes the RX holder's resolve return -1 or
its pass block the close; `mark` returns with `OPEN_FAILED` counted and no
slot taken; the backward shift moves key 1 into entry 0).

`RetiredClosed` was also run under `Spec` without fairness on the
`LivePacket` instance: TLC reports the temporal property violated
(stuttering), so the passing liveness configs are not vacuous.

## Results

Pinned `tlacheck` and `tla2tools.jar`, each case its own run (2 workers for a
passing case, 1 for a counterexample), linux-amd64 bench; the records are in
`RUNS.tsv`.

| Config | Result | Distinct | Generated | Seconds |
|---|---|---|---|---|
| `MCRNMsgxSocketTable.cfg` | pass | 169,376 | 870,815 | 18.4 |
| `MCRNMsgxSocketTableLayout.cfg` | pass | 742,825 | 2,810,771 | 52.9 |
| `MCRNMsgxSocketTableQuarantine.cfg` | pass | 100,575 | 428,452 | 11.4 |
| `MCRNMsgxSocketTableDead.cfg` | pass | 334,768 | 1,422,882 | 26.6 |
| `MCRNMsgxSocketTableLive.cfg` | pass | 77,531 | 242,620 | 29.8 |
| `MCRNMsgxSocketTableLivePacket.cfg` | pass | 9,951 | 34,698 | 5.0 |
| `MCRNMsgxSocketTableBrokenCloseAtRetire.cfg` | `NoCrossClient` | 9,427 | 35,730 | 3.3 |
| `MCRNMsgxSocketTableBrokenNoGenerationCheck.cfg` | `NoCrossClient` | 37,964 | 139,007 | 7.9 |
| `MCRNMsgxSocketTableBrokenSnapshotBeforeWord.cfg` | `NoCrossClient` | 52,564 | 183,138 | 9.6 |
| `MCRNMsgxSocketTableBrokenExhaustionTakesSlot.cfg` | `NoLeak` | 655 | 1,606 | 3.2 |
| `MCRNMsgxSocketTableBrokenExhaustionTakesSlotAccounting.cfg` | `Accounting` | 655 | 1,606 | 1.7 |
| `MCRNMsgxSocketTableBrokenGenWrap2.cfg` | `NoCrossClient` | 7,950 | 41,353 | 4.8 |
| `MCRNMsgxAddressIndex.cfg` | pass | 640 | 1,984 | 1.6 |
| `MCRNMsgxAddressIndexBrokenDeleteWithoutShift.cfg` | `LookupFindsOpen` | 497 | 833 | 1.3 |
| `MCTraceRNMsgxSocketTableSelf.cfg` | pass | 14 | 15 | 1.4 |
| `MCTraceRNMsgxSocketTableSelfBrokenClose.cfg` | `TraceConsistent` | 10 | 10 | 1.5 |

## The trace check

`tests/tla_trace_msgx.cpp` (`tla_trace_msgx`, and `tla_trace_msgx_plant`
built with `ROCKETNET_MSGX_SOCKET_TABLE_PLANT`) walks the real header for 400
calls per seed over a fake kernel of three descriptors, two transmit holders,
one receive holder, a budget of two opens and one other descriptor of the
process, with four addresses whose real hashes give homes 2046, 2046, 2047
and 0 in the code's 2048-entry index (a collision and a wrap). Each record
carries every model variable and the call made; `TraceRNMsgxSocketTable.tla`
requires the records to be a behaviour of `Spec`.

- The self-test (`MCTraceRNMsgxSocketTableSelf.tla`) accepts its hand-written
  good trace and rejects the one with a single wrong step (a sweep that
  closes while holder 1 is still inside its pass).
- Seeds 1 to 16: every clean trace passes (453 to 546 distinct states, 4 to 13
  seconds each), every plant trace fails `TraceConsistent` (8 to 76 distinct
  states). The plant fired 8 to 19 times per seed (`EARLY-CLOSES`); the clean
  build never did.

The plant is the close at the retire, the same change as
`MCRNMsgxSocketTableBrokenCloseAtRetire.cfg`: it sits in
`rocketnet_msgx_socket_table_retire` behind the flag and is never defined
outside the plant build.
