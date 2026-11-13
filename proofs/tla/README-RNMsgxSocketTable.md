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
model's kernel maps each descriptor to its remote, so that send is the event
`crossed`.

## State

| Model | Code |
|---|---|
| `word[s]`, `<<gen, fd + 1>>` | `slots[s].word`: generation high 32 bits, fd + 1 low |
| `deadRep[s]` | `slots[s].dead_report`: 0, or the reported generation + 1 |
| `state`, `fd`, `gen`, `addr` | `slots[s].state`, `.fd`, `.generation`, `.address` |
| `snap[s][h]` | `slots[s].passcopy[h]` |
| `pass[h]` | `pass[h]`: even quiescent, odd inside a pass |
| `deadReports` | `dead_reports` |
| `freeSlots`, `numFree` | `free_slots[]`, `num_free` (a stack) |
| `retired`, `numRetired` | `retired[]`, `num_retired` (compacted in place by the sweep) |
| `numOpen` | `num_open` |
| `index[e]` | `index[e]`, for the entries in use: the model's index is a function on the entries holding a slot, where the code holds -1 in an empty entry (a record of the table names the entries in use, not all 2048) |
| `coolAddr`, `coolUntil` | `cooldown_address[]`, `cooldown_until[]` |
| `counters`, `update`, `opensLeft` | `counters[]`, `update`, `opens_left` |
| `mpc`, `ms`, `mi`, `mk` | main's place inside a call; `"idle"` between calls |
| `kfd[f]`, `kdead[f]` | the kernel: closed, another descriptor of the process, or the remote; disconnected |
| `poller` | the caller's receive poller: `<<fd, slot, generation, address>>` |
| `pkts`, `cur`, `rep`, `rxEv`, `rxFd` | the fill threads' stamped packets and the holders' work |
| `crossed`, `onClosed`, `attempts` | history: a send or read reached another remote, or a closed descriptor; open calls |

## Actions and the code they mirror

- **`Mark(a)`**: `rocketnet_msgx_socket_table_mark`. Refused by the
  cooldown, a full table or the open budget (each counted); otherwise the
  open (`table->open`), refused by the kernel for a remote that already has a
  connected socket (EADDRINUSE: a retired socket still holds it) or when no
  descriptor is free (counted `OPEN_FAILED`, no slot taken), or the slot
  popped, the word stored, the address inserted and the poller registered.
- **`Forget(a)`**: `rocketnet_msgx_socket_table_forget`, the lookup and the
  first step of `rocketnet_msgx_socket_table_retire`: index remove, generation
  advance, word store.
- **`Retire`**: the retire's second step: the passcopy of every holder's
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
kernel delivers to the descriptor's current remote; a disconnected socket
answers ENOTCONN), `RxPoll` (the poller hands out a registered event),
`ForgetDereg` (the caller's deregistration after forget returns
`retired_fd`), `EnvOpen` and `EnvClose` (other descriptors of the process)
and `Disconnect` (ICMP).

- **`ForgetCall(a)`**, **`SweepCall`**: a whole call of main's with no holder
  step inside it: `rocketnet_msgx_socket_table_forget` with the caller's
  deregistration, and `rocketnet_msgx_socket_table_sweep`. The model splits
  those calls into steps so holders interleave inside them (the reversed
  witness `PassCopyBeforeWord` fails only because a holder runs between the
  word store and the passcopy); the trace check sees the table between calls.
  Each step is a function of a record of main's state (`ForgetF`, `RetireF`,
  `RetireWordLateF`, `DeregF`, `SweepBeginF`, `RetireDeadF`, `CloseF`); the
  fine-grained actions apply one of them and the call-level actions apply
  them in the code's order (`RunF`) with no holder step between, so each
  step has one definition. The fine-grained actions are listed untraced in
  `tools/tla_trace_check.py` with that reason, and liveness (`SpecFair`) is
  checked over `NextFine`, the fine-grained actions alone.

The index's empty entries are outside its domain here (the code holds -1), so
the walk's records name the entries in use. `Home` and `CoolOf` are read from
the constants `HomeAt` and `CoolAt`, the sets of `a * IS + home` and
`a * CoolEntries + entry` (a cfg carries a set of integers, not a function).

The retire's passcopy loop is one step in the model. Each holder's passcopy
only decides that holder's quiescence, and the code reads each pass after the
word store, so any order of the per-holder reads is one of the model's
interleavings for that holder.

## Properties

- `NoCrossClient`: no send or read reached a remote other than the one the
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
- `OpenWatched`: between calls every open socket is registered with the
  receive poller, or reported dead at its own generation (the next sweep
  retires it). `Mark` lets the poller refuse the registration
  (`RegisterMayFail`), and main then reports the socket dead, as
  `rocketnet_server_msgx_mark` does.
- `FreeHasNoReport`: between calls a free slot holds no dead report.
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
| `MCRNMsgxSocketTable.cfg` | 2 slots, 2 addresses, 2 descriptors, TX holders {1, 2} and RX holder 3, one pass each, one packet, 2 opens, no `BeginUpdate` | every holder against reuse of a slot and a descriptor |
| `MCRNMsgxSocketTableUpdate.cfg` | the same with TX holder 1 and RX holder 2, and one `BeginUpdate` (with two TX holders and an update it is over the 110 s budget) | marks, forgets and sweeps after the update at the control's budget and cooldown |
| `MCRNMsgxSocketTableLayout.cfg` | 3 slots, 4 addresses (homes 6, 7, 7, 0 of 8: collision and wrap), 3 descriptors, one other descriptor of the process, 2 opens per budget and two `BeginUpdate`s; every registration succeeds (`RegisterMayFail <- MCRegisterAlways`: refused registrations take it past the budget, and the other configs check them) | the index, free stack, EADDRINUSE and EMFILE, budget, counters |
| `MCRNMsgxSocketTableQuarantine.cfg` | 3 slots, 3 addresses, 3 descriptors, TX holder 1 and RX holder 2, one pass each, 3 opens | the retired list compacted around kept entries |
| `MCRNMsgxSocketTableDead.cfg` | 2 slots, 2 addresses, TX 1 and RX 2, ICMP, cooldown 1, 1 open per budget and one `BeginUpdate` | dead reports, stale reports, cooldown |
| `MCRNMsgxSocketTableLive.cfg` | 2 slots, TX 1 and RX 2, no packets, `SpecFair`, no VIEW | `RetiredClosed` |
| `MCRNMsgxSocketTableLivePacket.cfg` | 1 slot, TX 1 with a packet and RX 2, `SpecFair`, no VIEW | `RetiredClosed` with a packet in flight |

The safety configs use `View`, which replaces every value no later step
reads by a constant (a free slot's address, the free stack above `numFree`, a
cooldown entry already expired, a finished packet's detail), keeps the
counters only as the differences `Accounting` reads, and keeps a live
passcopy only as what `Quiescent` reads: whether the holder is still inside
the pass copied (a pass only grows, so once it moves on it never equals
the passcopy again). The liveness configs run without the VIEW and override
`RefusedMarks` to FALSE: a refused mark changes only its counter, which no
guard reads, and leaving it out makes the state space finite.

Generations wrap at `GenWrap` (8 in the passing configs; 2^32 in the code, 2^30 in the trace cfgs: TLC's integers stop at 2^31 and the walk's generations stay far below its steps).

## Reversed witnesses

Each switch is TRUE in the code (`PassCopyFirst` FALSE). Each broken config
sets one and lists only `TypeOK` and the invariant it must fail.

| Config | Change | Fails | The counterexample |
|---|---|---|---|
| `MCRNMsgxSocketTableBrokenCloseAtRetire.cfg` | `Quarantine = FALSE`: the retire closes at once | `NoCrossClient` | client 1 on slot 0, fd 0; TX holder 1 begins, resolves fd 0; forget closes fd 0 at the retire; mark client 2 opens the lowest free descriptor, fd 0; the send reaches client 2 |
| `MCRNMsgxSocketTableBrokenNoGenerationCheck.cfg` | `GenCheck = FALSE`: resolve ignores the stamp's generation | `NoCrossClient` | client 1 on slot 0; forget retires it (passcopy all quiescent); the RX holder begins and takes the event (fd 0, slot 0, generation 0) before the caller deregisters; the sweep closes fd 0; client 2 gets slot 0, generation 1, fd 0; the event resolves to fd 0 and the read is client 2's |
| `MCRNMsgxSocketTableBrokenPassCopyBeforeWord.cfg` | `PassCopyFirst = TRUE`: the passes are copied before the word store | `NoCrossClient` | forget passcopys (all quiescent); the RX holder begins and resolves the old word, fd 0; the word store; the sweep closes fd 0 (the passcopy showed no pass); client 2 gets fd 0; the read is client 2's |
| `MCRNMsgxSocketTableBrokenExhaustionTakesSlot.cfg` | `RefusedOpenFree = FALSE`: a refused open takes a slot, uncounted | `NoLeak` | client 1 opened, forgotten, in quarantine; mark client 1 again: the kernel refuses (EADDRINUSE, the retired socket is still connected) and the slot is open with fd -1 |
| `MCRNMsgxSocketTableBrokenExhaustionTakesSlotAccounting.cfg` | the same | `Accounting` | the same six states: an open call neither opened nor counted refused |
| `MCRNMsgxSocketTableBrokenGenWrap2.cfg` | `GenWrap = 2`, one slot | `NoCrossClient` | a packet stamped for client 1 at generation 0 stays queued while slot 0 is retired twice (generation 1, then 0 again) and reopened for client 2; the stale stamp resolves to client 2's descriptor. In the code the wrap is 2^32 retires of one slot while one stamp is queued |
| `MCRNMsgxSocketTableBrokenStaleReport.cfg` | `ClearReportAtFree <- MCKeepStaleReport` (FALSE): the close loop leaves a dead report in the slot it frees | `OpenWatched` | mark client 1 on slot 0: the poller refuses it and main reports generation 0; TX holder 1 stamps, begins and resolves fd 0; the sweep retires slot 0 on the report (generation 1, cooled down), kept while holder 1 is inside; the next sweep's dead scan finds nothing; fd 0 is disconnected, holder 1's send answers ENOTCONN and it reports generation 0 into the retired slot, ends its pass, and the close loop frees slot 0 with the report in it; after `BeginUpdate` client 2 takes slot 0 (generation 1), the poller refuses it, and main's report fails against the stale one: the socket is open, unwatched and unreported |
| `MCRNMsgxAddressIndexBrokenDeleteWithoutShift.cfg` | `DeleteShift = FALSE`: the delete only clears the entry | `LookupFindsOpen` | keys 0 and 1 both home at 0; 0 at entry 0, 1 at entry 1; remove 0 clears entry 0; the lookup of 1 stops at the empty entry 0 |

Each counterexample was read state by state against the header: the steps
are the code's calls in the order shown, and in the code without the change
each is refused where the model refuses it (the quarantine keeps fd 0 open
while holder 1 is in its pass; the generation check drops the event; the
word store before the passcopy makes the RX holder's resolve return -1 or
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
| `MCRNMsgxSocketTable.cfg` | pass | 615,957 | 3,556,507 | 19.4 |
| `MCRNMsgxSocketTableLayout.cfg` | pass | 742,825 | 3,797,376 | 24.1 |
| `MCRNMsgxSocketTableQuarantine.cfg` | pass | 567,505 | 2,794,642 | 17.3 |
| `MCRNMsgxSocketTableDead.cfg` | pass | 629,277 | 3,226,529 | 18.6 |
| `MCRNMsgxSocketTableLive.cfg` | pass | 274,389 | 798,188 | 51.4 |
| `MCRNMsgxSocketTableLivePacket.cfg` | pass | 31,205 | 101,360 | 4.9 |
| `MCRNMsgxSocketTableBrokenCloseAtRetire.cfg` | `NoCrossClient` | 21,935 | 79,772 | 2.2 |
| `MCRNMsgxSocketTableBrokenNoGenerationCheck.cfg` | `NoCrossClient` | 39,347 | 123,144 | 2.6 |
| `MCRNMsgxSocketTableBrokenPassCopyBeforeWord.cfg` | `NoCrossClient` | 364,295 | 1,538,635 | 16.0 |
| `MCRNMsgxSocketTableBrokenExhaustionTakesSlot.cfg` | `NoLeak` | 120 | 216 | 0.7 |
| `MCRNMsgxSocketTableBrokenExhaustionTakesSlotAccounting.cfg` | `Accounting` | 120 | 216 | 0.7 |
| `MCRNMsgxSocketTableBrokenGenWrap2.cfg` | `NoCrossClient` | 20,162 | 108,117 | 3.1 |
| `MCRNMsgxSocketTableBrokenStaleReport.cfg` | `OpenWatched` | 256,208 | 1,315,385 | 13.1 |
| `MCRNMsgxAddressIndex.cfg` | pass | 640 | 1,984 | 0.7 |
| `MCRNMsgxAddressIndexBrokenDeleteWithoutShift.cfg` | `LookupFindsOpen` | 497 | 833 | 0.7 |
| `MCTraceRNMsgxSocketTableSelf.cfg` | pass | 10 | 19 | 1.0 |
| `MCTraceRNMsgxSocketTableSelfBrokenClose.cfg` | `TraceConsistent` | 7 | 7 | 0.8 |

## The trace check

`tests/tla_trace_msgx.cpp` (`tla_trace_msgx`, and `tla_trace_plant_msgx` built
with `ROCKETNET_TLA_PLANT`, which includes `sdk/rocketnet_msgx_socket_table.cpp`
with `ROCKETNET_MSGX_SOCKET_TABLE_PLANT`) walks the real table for 500 steps
per seed over a fake kernel of three descriptors, two transmit holders, one
receive holder, a budget of two opens and one other descriptor of the process,
with four addresses whose real hashes give homes 2046, 2046, 2047 and 0 in the
code's 2048-entry index (a collision and a wrap). Each record carries the op,
the model action it took, its arguments and every model variable;
`TraceRNMsgxSocketTable.tla` requires the records to be a behaviour of `Spec`
one action at a time ([README-TraceCheck.md](README-TraceCheck.md)). A forget
is `ForgetCall` and a sweep `SweepCall`; the row lists the fine-grained
actions untraced with that reason. The row's reach predicates (`IndexProbed`,
`IndexWrapped`, `Held`, `DeadRetired`) require some clean trace to show the
index probing and wrapping, a socket held in quarantine by an open pass, and a
socket retired on a dead report.

- The self-test (`MCTraceRNMsgxSocketTableSelf.tla`) accepts its hand-written
  good trace and rejects the one with a single wrong step (a sweep that
  closes while holder 1 is still inside its pass).
- Seeds 1 to 4, `--jobs 8` (hetzner1, under other pieces' load): every clean
  trace passes (501 distinct states, 38 to 42 s of TLC each), every plant
  trace fails `TraceConsistent` at the first record the plant changed (seed 1
  at record 13, seed 2 at 26, seed 3 at 19, seed 4 at 65), a record the walk's
  `PLANT-AT` lists (15 to 18 closes of a slot still open per seed); the clean
  build never made one. Every action not listed untraced was taken, and the
  four reach predicates were reached.
- The template's `ASSUME` reads `TraceLog` once per record and TLC builds the
  log again at each read, so a trace costs records times the log's size: the
  walk is 500 steps, with the index and the cooldown table logged as the
  entries in use, to stay inside the 100 s budget.

The plant is the close at the retire, the same change as
`MCRNMsgxSocketTableBrokenCloseAtRetire.cfg`: it sits in
`rocketnet_msgx_socket_table_retire` (`sdk/rocketnet_msgx_socket_table.cpp`,
out of line so the plant is library code) behind the flag and is never defined
outside the plant build. `PLANT-FIRED` counts the closes of a slot still open;
the clean build never makes one.
