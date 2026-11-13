# RNMsgxSendRuns

The macOS batched server transport's send path over one taken bank,
`sdk/rocketnet_msgx_send_runs.h`, at the code's layout, with the worker loop
of `rocketnet_server_msgx_worker` (`sdk/rocketnet_server_msgx_darwin.cpp`)
that calls it. The bank hand-off is `rocketnet_buffer_handoff_t`, checked by
[RNBankFifoLayout](README-RNBankFifoLayout.md); the socket table the path
resolves through is [RNMsgxSocketTable](README-RNMsgxSocketTable.md). This
model takes the table's answers as given and checks what the send path does
with them. A trace check binds the header to it (below).

## Input

A bank of `Size` claims (`num_packets` clamped at the bank's size, so
`Claims` holds a value past it). Each packet has a client (its destination),
the stamp a fill thread gave it (table slot, `-1` the shared socket, and the
slot's generation), the fill phase it was staged in (`info.phase`, main's
update count, which only `rocketnet_server_msgx_commit` advances), and
whether it is live (`packet_bytes > 0`; an aborted or never finished packet
is not). Every slot is open at `OpenGen(s)` when the send starts (slot 0 at
generation 1, slot 1 at 0), so a stamp at the other generation is stale;
`Retire` closes a slot while the worker sends. A slot's socket is connected
to the client stamped at its open generation.

`BankOK` is what the fill threads can stage:
- fill phases rise in staged order: a fill of the next phase runs after
  main's commit, after every fill of this one (the fill-phase contract X0,
  `sdk/rocketnet_fill_phase.h`);
- a slot at a generation belongs to one client;
- within one fill phase a client has one stamp: the table changes only in
  commit.

Across phases a client's stamp may change: a bank whose publish was deferred
(the handoff's try-lock was busy or no bank was free) keeps filling across
the next commit, which may retire or open that client's socket.
`MultiStamp` FALSE keeps one stamp per client for the whole bank, and those
configs have one phase (`Phases = {0}`); TRUE configs have two.

The input domain is otherwise every stamp assignment the header accepts.
That is wider than the server reaches: a slot's generations rise in staged
order, so in the server a run whose head resolves is never followed by an
older generation of its slot. The header does not rely on that; the
generation break in the run builder keeps it true for any input.

## State

| Model | Code |
|---|---|
| `count` | `count` in `rocketnet_server_msgx_worker` (`num_packets` clamped) |
| `client`, `slot`, `gen`, `phase`, `live` | `info[i].to`, `.table_slot`, `.generation`, `.phase`, `.packet_bytes > 0` |
| `tOpen`, `tGen` | what `rocketnet_msgx_socket_table_resolve` answers |
| `order` | `worker->order` |
| `pc`, `cursor`, `hd`, `scan`, `run`, `attempts` | `rocketnet_msgx_send_runs_state_t`: `pc` (`"sort"` before the sort), `cursor`, `head`, `scan`, `run_packets[0..run)`, `attempts` |
| `quit`, `retries` | `worker->shutdown`, `retries_after_quit` |
| `left`, `lastC`, `lastS`, `bad` | history: departures per packet; the last packet sent per client and per client and stamp; a send to another client, out of order, or a dead report on the shared socket |

## Actions and the code they mirror

Each action is one call: the sort, one `rocketnet_msgx_send_runs_step`, or
the worker's check after a kept call.

- **`Sort`**: `rocketnet_msgx_send_runs_sort`. The bank is cut into runs of
  one fill phase in staged order (`RangeEnd`), and each run is sorted by
  `rocketnet_msgx_send_runs_sort_range`'s three loops over `bucket[]`
  (capacity + 2 entries) as recursive operators: count by key, prefix sums,
  place `order[lo + bucket[key]++] = i`.
- **`LoopTop`**: the step at `HEAD`: the bank done; an aborted head skipped; a
  head that resolves to no socket sent alone on the shared socket (sent,
  dropped, or full: kept); or the run built (`Build`: the head's slot and
  generation, aborted packets skipped, at most `Batch` live ones).
- **`SendAll`**, **`SendPartial`**, **`SendFull`**, **`SendRefused`**,
  **`SendDead`**, **`SendError`**: the step at `SEND`, the connected send's
  answers. A partial send walks the cursor past exactly that many live
  packets (`Advance`); nothing taken and full keep the run; a refusal goes
  again once and a second is an error; dead reports the run's stamp and
  starts the resend loop; any other error drops the run, counted.
- **`Dead`**: the step at `DEAD`, one packet of the dead run on the shared
  socket.
- **`Kept`**: `rocketnet_server_msgx_worker` after a false return
  (`rocketnet_msgx_send_runs_give_up`, `shutdown && ++retries > limit`):
  past `RetriesAfterQuit` retries after quit every live packet from the
  cursor is dropped, counted (`rocketnet_msgx_send_runs_owed`); otherwise
  the next call resumes at the same cursor (`rocketnet_msgx_send_runs_resume`).
  The retry count rises on every check after quit, the last one too, as the
  code's `++` does.

Environment: `Quit` (main sets shutdown) and `Retire(s)` (main retires a
slot). The worker reads shutdown only in `Kept` and the table only at a
resolve in `LoopTop`, so each is taken just before that read; any other
placement reaches the same reads. A run already resolved still reaches its
peer: the table's quarantine keeps that socket open and connected until the
pass ends (RNMsgxSocketTable `NoCrossClient`). The step at `SEND` uses the
descriptor resolved at the build (`state.fd`), since the table holds still
for the worker's pass.

## Properties

- `SortPermutation`: the sorted order is a permutation of the bank, fill
  phases non-decreasing, and keys non-decreasing inside a phase (the shared
  socket's packets first).
- `LeavesOnce`: a live packet leaves at most once (sent connected, sent
  shared, or dropped counted) and exactly once when the bank is done; an
  aborted packet never leaves.
- `NoCrossClient`: a connected send reaches the packet's own client.
- `ClientOrder`: per client, packets go on the wire in staged order.
- `StampOrder`: per client and stamp, packets go on the wire in staged order.
- `CursorSafe`: the cursor never passes a live packet that has not left.
- `ResumeExact`: when a call returns false, nothing at or past the cursor has
  left, so the next call resumes exactly where this one stopped.
- `DeadReportsConnected`: a dead report names a connected stamp.

## Instances

Size 4, two slots and the shared socket, generations 0 and 1, two clients,
two calls that return false, one retire, one retry after quit.

| Config | Instance | Invariants |
|---|---|---|
| `MCRNMsgxSendRuns.cfg` | batch 2, one fill phase | all |
| `MCRNMsgxSendRunsBatch3.cfg` | batch 3 (a run that can hold an aborted packet between taken ones), one phase | all |
| `MCRNMsgxSendRunsRestamp.cfg` | `MultiStamp`, two fill phases, size 3, one call that returns false | all |
| `MCRNMsgxSendRunsRestampOrder.cfg` | the same | `ClientOrder` alone, the finding below |

## The restamped client (found, then fixed)

Before the fill phase was stamped, `MCRNMsgxSendRunsRestampOrder.cfg` failed
`ClientOrder`: client 1's packet 0 is stamped on slot 0 at generation 0, main's
commit retires the slot (the table is at generation 1), and client 1's packet
1, staged in the next fill phase of the same deferred bank, is stamped for the
shared socket. A sort by slot alone puts the shared socket's packets first, so
packet 1 left before packet 0 (which then resolves to no socket and leaves
through the shared socket after it). No order of keys alone fixes both
directions (socket to shared, and shared to socket).

The fix: each packet carries the fill phase it was staged in
(`start_packet` writes `msgx->update` into `info.phase`; a plain read, safe
under X0 because main writes it only in commit and fills run strictly
between main's operations), and the sort orders by phase first and slot
second, keeping staged order inside each (phase, slot). Within one phase the
order is as before: the shared socket first, then each slot's run.
`MCRNMsgxSendRunsRestampOrder.cfg` and `MCRNMsgxSendRunsRestamp.cfg` now pass,
and `MCRNMsgxSendRunsBrokenSlotOnlySort.cfg` keeps the old sort as a witness.
The unit test `test_rocketnet_msgx_send_runs_restamp` is this bank, and is
red on the old sort (mutation 10).

## Reversed witnesses

Each switch is TRUE in the code. Each broken config sets one FALSE and lists
only `TypeOK` and the invariant it must fail. The same defects are
`ROCKETNET_MSGX_SEND_RUNS_MUTATION` 10 (`PhaseSort`) and 1 to 4 in the
header, and `tests/test_unit_msgx_send_runs.cpp` turns red on each.

| Config | Change | Fails | The counterexample |
|---|---|---|---|
| `MCRNMsgxSendRunsBrokenSlotOnlySort.cfg` | `PhaseSort = FALSE`: the whole bank sorted by slot alone | `ClientOrder` | client 1's packet 0 in phase 0 on slot 0 at generation 0 (stale), client 1's packet 1 in phase 1 on the shared socket; the sort puts packet 1 first, it leaves on the shared socket, then packet 0 resolves to no socket and leaves after it |
| `MCRNMsgxSendRunsBrokenPartialCountsAborted.cfg` | `PartialSkipsAborted = FALSE`: a partial send counts every cursor step as taken | `ResumeExact` | four packets of client 1 on slot 0 at generation 1, packet 1 aborted; the run is [0, 2, 3]; the send takes 2 (packets 0 and 2); the cursor stops at position 2, on packet 2, which has left |
| `MCRNMsgxSendRunsBrokenUnstableSort.cfg` | `StableSort = FALSE`: placement from each bucket's end | `ClientOrder` | packets 2 and 3 of client 1 on the shared socket (0 and 1 aborted); the order is reversed in the bucket and packet 3 leaves before packet 2 |
| `MCRNMsgxSendRunsBrokenRunCrossesGeneration.cfg` | `GenBreak = FALSE`: a run ends only at a change of slot | `NoCrossClient` | client 1's packet 0 on slot 0 at generation 1 (open), packets 1 and 2 aborted, client 2's packet 3 on slot 0 at generation 0 (stale); the run is [0, 3] and client 2's packet goes on client 1's socket |
| `MCRNMsgxSendRunsBrokenDeadNoResend.cfg` | `DeadResends = FALSE`: a dead run is skipped | `CursorSafe` | client 1's packet 0 alone on slot 0; the send answers dead; the cursor moves past packet 0, which never left |

Each counterexample was read state by state against the header: the steps are
the header's in the order shown, and without the change the code refuses
where the model refuses (the phase runs are sorted one at a time, so packet 0
goes first; the cursor walks past 0, the aborted 1 and 2 to position 3; the
counting sort places forward from the bucket's start; the scan stops at
packet 3, which then resolves to no socket and leaves through the shared
socket; the resend loop sends packet 0 on the shared socket before the cursor
passes it).

## The trace check

`tests/tla_trace_msgx_send_runs.cpp` (layer `msgxsendruns`,
[README-TraceCheck](README-TraceCheck.md)) binds the header to this model.
Its seeded walk stages one bank of up to four packets over two fill phases,
sorts it with the real sort, and calls the real
`rocketnet_msgx_send_runs_step` one model step at a time over a fake table
and fake sends that answer at random (all, a prefix, nothing, full, refused,
dead, error; sent, dropped, full), with the worker's real give-up check and
resume after a kept call, and the environment's `Retire` and `Quit` between
steps. Each record is every variable of this model: the bank, the table, the
code's order array and send state, and the history the fakes saw. A packet is
counted dropped when the cursor passes it live and no fake took it, and the
code's dropped counter must agree after every step, or the record is
poisoned and the trace fails. `TraceRNMsgxSendRuns.tla` checks the trace as a
behaviour of `Next`; its `TraceInit` takes the bank from the first record and
rejects one `BankOK` does not admit.

The plant (`ROCKETNET_MSGX_SEND_RUNS_PLANT`, the same change as mutation 10)
is the slot-only sort. Every walk's bank has a connected packet in phase 0
and a shared packet in phase 1, so the planted sort differs from the model's
on every seed and its trace is rejected at the sort record; the condition is
a property of the input (`quiet` False).

`MCTraceRNMsgxSendRunsSelf.tla` holds the controls: a hand-written trace of
the restamped client (sort by phase, the slot retired, both packets on the
shared socket in staged order) that must pass, and the same trace with the
slot-only sort that must fail `TraceConsistent` at the sort record
(`MCTraceRNMsgxSendRunsSelfBrokenSlotOnly.cfg`).

## Results

Pinned `tlacheck` and `tla2tools.jar`, each case its own shard (2 workers),
linux-amd64 bench (amd-1); the records are in `RUNS.tsv`.

| Config | Result | Distinct | Generated | Seconds |
|---|---|---|---|---|
| `MCRNMsgxSendRuns.cfg` | pass | 3,314,496 | 5,699,384 | 40.9 |
| `MCRNMsgxSendRunsBatch3.cfg` | pass | 3,337,696 | 5,722,504 | 40.7 |
| `MCRNMsgxSendRunsRestamp.cfg` | pass | 2,161,760 | 3,665,288 | 21.0 |
| `MCRNMsgxSendRunsRestampOrder.cfg` | pass | 2,161,760 | 3,665,288 | 15.7 |
| `MCRNMsgxSendRunsBrokenSlotOnlySort.cfg` | `ClientOrder` | 64,244 | 78,013 | 5.2 |
| `MCRNMsgxSendRunsBrokenPartialCountsAborted.cfg` | `ResumeExact` | 36,807 | 45,517 | 9.3 |
| `MCRNMsgxSendRunsBrokenUnstableSort.cfg` | `ClientOrder` | 27,832 | 32,726 | 9.3 |
| `MCRNMsgxSendRunsBrokenRunCrossesGeneration.cfg` | `NoCrossClient` | 36,750 | 45,460 | 10.2 |
| `MCRNMsgxSendRunsBrokenDeadNoResend.cfg` | `CursorSafe` | 36,712 | 45,416 | 9.3 |
| `MCTraceRNMsgxSendRunsSelf.cfg` | pass | 6 | 7 | 1.6 |
| `MCTraceRNMsgxSendRunsSelfBrokenSlotOnly.cfg` | `TraceConsistent` | 2 | 2 | 1.1 |
