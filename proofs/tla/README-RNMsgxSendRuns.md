# RNMsgxSendRuns

The macOS batched server transport's send path over one taken bank,
`sdk/rocketnet_msgx_send_runs.h`, at the code's layout, with the worker loop
of `rocketnet_server_msgx_worker` (`sdk/rocketnet_server_msgx_apple.cpp`)
that calls it. The bank hand-off is `rocketnet_buffer_handoff_t`, checked by
[RNBankFifoLayout](README-RNBankFifoLayout.md); the socket table the path
resolves through is [RNMsgxSocketTable](README-RNMsgxSocketTable.md). This
model takes the table's answers as given and checks what the send path does
with them.

## Input

A bank of `Size` claims (`num_packets` clamped at the bank's size, so
`Claims` holds a value past it). Each packet has a client (its destination),
the stamp a fill thread gave it (table slot, `-1` the shared socket, and the
slot's generation) and whether it is live (`packet_bytes > 0`; an aborted or
never finished packet is not). Every slot is open at `OpenGen(s)` (from the set
`OpenGenAt` of `s * GenSpan + g`) when the
send starts (slot 0 at generation 1, slot 1 at 0 in the checked configs), so a stamp at the other
generation is stale; `Retire` closes a slot while the worker sends. A slot's
socket is connected to the client stamped at its open generation.

The input domain is every stamp assignment the header accepts. That is wider
than the server reaches: inside one fill phase a client has one stamp, and a
slot's generations rise in staged order, so in the server a run whose head
resolves is never followed by an older generation of its slot. The header's
contract does not rely on that, and the generation break in the run builder
is what keeps it true for any input.

`MultiStamp` FALSE keeps one stamp per client (the fill-phase contract).
TRUE lets a client hold two: a bank whose publish was deferred (the
handoff's try-lock was busy or no bank was free) keeps filling across the
next update's replay walk, which may retire or open that client's socket.

## State

| Model | Code |
|---|---|
| `count` | `count` in `rocketnet_server_msgx_worker` (`num_packets` clamped) |
| `client`, `slot`, `gen`, `live` | `info[i].to`, `.table_slot`, `.generation`, `.packet_bytes > 0` |
| `tOpen`, `tGen` | what `rocketnet_msgx_socket_table_resolve` answers |
| `order` | `worker->order` |
| `cursor`, `hd`, `scan`, `run`, `attempts` | `*cursor`, `head`, `scan`, `run_packets[0..run)`, `attempts` |
| `quit`, `retries` | `worker->shutdown`, `retries_after_quit` |
| `left`, `lastC`, `lastS`, `bad` | history: departures per packet; the last packet sent per client and per client and stamp; a send to another client, out of order, or a dead report on the shared socket |

## Actions and the code they mirror

- **`Sort`**: `rocketnet_msgx_send_runs_sort`, its three loops over
  `bucket[]` (capacity + 2 entries) as recursive operators: count by key,
  prefix sums, place `order[bucket[key]++] = i`.
- **`HeadDone`**, **`HeadSkip`**, **`HeadShared`**, **`HeadDropped`**,
  **`HeadKept`**, **`HeadRun`**: the top of the send's loop, one action per
  outcome: the bank done; an aborted head skipped; a head that resolves to no
  socket sent alone on the shared socket (sent, dropped, or full: kept); or
  the run built (`Build`: the head's slot and generation, aborted packets
  skipped, at most `Batch` live ones).
- **`SendAll`**, **`SendPartial(k)`**, **`SendFull`**, **`SendRefused`**,
  **`SendDead`**, **`SendError`**: the connected send's answers. A partial
  send of `k` walks the cursor past exactly that many live packets
  (`Advance`); nothing taken and full keep the run; a refusal goes again once
  and a second is an error; dead reports the run's stamp and starts the
  resend loop; any other error drops the run, counted.
- **`DeadDone`**, **`DeadSkip`**, **`DeadShared`**, **`DeadDropped`**,
  **`DeadKept`**: the dead run's resend, one packet per step, on the shared
  socket.
- **`GiveUp`**, **`Resume`**: `rocketnet_msgx_send_runs_give_up` after a
  false return: past `RetriesAfterQuit` retries after quit (the count rises
  on every check after quit, the last one too) every live packet from the
  cursor is dropped, counted; otherwise the next call starts at the same
  cursor.

Environment: `Quit` (main sets shutdown) and `Retire(s)` (main retires a
slot). The worker reads shutdown only in `GiveUp` and `Resume` and the table only
at a resolve at `HeadDone` .. `HeadRun`, so each is taken just before that read; any other
placement reaches the same reads. A run already resolved still reaches its
peer: the table's quarantine keeps that socket open and connected until the
pass ends (RNMsgxSocketTable `NoCrossClient`).

## Properties

- `SortPermutation`: the sorted order is a permutation of the bank, keys
  non-decreasing (the shared socket's packets first).
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
| `MCRNMsgxSendRuns.cfg` | batch 2 | all |
| `MCRNMsgxSendRunsBatch3.cfg` | batch 3 (a run that can hold an aborted packet between taken ones) | all |
| `MCRNMsgxSendRunsRestamp.cfg` | `MultiStamp`, size 3, one call that returns false | all but `ClientOrder` |
| `MCRNMsgxSendRunsRestampOrder.cfg` | the same | `ClientOrder`, which fails |

## The restamped client (a finding, not fixed)

`MCRNMsgxSendRunsRestampOrder.cfg` fails `ClientOrder` with the code as it
is: client 1's packet 0 is stamped on slot 0 at generation 0, the slot is
retired (the table is at generation 1), and client 1's packet 1, staged later,
is stamped for the shared socket. The sort puts the shared socket's packets
first, so packet 1 leaves before packet 0 (which now resolves to no socket
and leaves through the shared socket after it). In the server this needs a
bank whose publish was deferred across a replay walk that retired, or opened,
that client's socket; within one fill phase it cannot happen
(`MCRNMsgxSendRuns.cfg` passes `ClientOrder`). No send-path order of keys
fixes both directions (socket to shared and shared to socket); a fix needs
the stamp to carry the fill phase, which is outside the send path.
`StampOrder` holds in every case.

## Reversed witnesses

Each switch is TRUE in the code. Each broken config sets one FALSE and lists
only `TypeOK` and the invariant it must fail. The same four defects are
`ROCKETNET_MSGX_SEND_RUNS_MUTATION` 1 to 4 in the header, and
`tests/test_unit_msgx_send_runs.cpp` turns red on each.

| Config | Change | Fails | The counterexample |
|---|---|---|---|
| `MCRNMsgxSendRunsBrokenPartialCountsAborted.cfg` | `PartialSkipsAborted = FALSE`: a partial send counts every cursor step as taken | `ResumeExact` | four packets of client 1 on slot 0 at generation 1, packet 1 aborted; the run is [0, 2, 3]; the send takes 2 (packets 0 and 2); the cursor stops at position 2, on packet 2, which has left |
| `MCRNMsgxSendRunsBrokenUnstableSort.cfg` | `StableSort = FALSE`: placement from each bucket's end | `ClientOrder` | packets 2 and 3 of client 1 on the shared socket (0 and 1 aborted); the order is reversed in the bucket and packet 3 leaves before packet 2 |
| `MCRNMsgxSendRunsBrokenRunCrossesGeneration.cfg` | `GenBreak = FALSE`: a run ends only at a change of slot | `NoCrossClient` | client 1's packet 0 on slot 0 at generation 1 (open), packets 1 and 2 aborted, client 2's packet 3 on slot 0 at generation 0 (stale); the run is [0, 3] and client 2's packet goes on client 1's socket |
| `MCRNMsgxSendRunsBrokenDeadNoResend.cfg` | `DeadResends = FALSE`: a dead run is skipped | `CursorSafe` | client 1's packet 0 alone on slot 0; the send answers dead; the cursor moves past packet 0, which never left |

Each counterexample was read state by state against the header: the steps are
the header's loop in the order shown, and without the change the code
refuses where the model refuses (the cursor walks past 0, the aborted 1 and 2
to position 3; the counting sort places forward from the bucket's start;
the scan stops at packet 3, which then resolves to no socket and leaves
through the shared socket; the resend loop sends packet 0 on the shared
socket before the cursor passes it).

## Results

Pinned `tlacheck` and `tla2tools.jar`, each case its own shard (2 workers),
linux-amd64 bench (amd-1); the records are in `RUNS.tsv`.

| Config | Result | Distinct | Generated | Seconds |
|---|---|---|---|---|
| `MCRNMsgxSendRuns.cfg` | pass | 3,314,496 | 5,699,384 | 36.9 |
| `MCRNMsgxSendRunsBatch3.cfg` | pass | 3,337,696 | 5,722,504 | 47.0 |
| `MCRNMsgxSendRunsRestamp.cfg` | pass | 1,494,692 | 2,531,864 | 15.5 |
| `MCRNMsgxSendRunsRestampOrder.cfg` | `ClientOrder` | 43,320 | 52,611 | 5.0 |
| `MCRNMsgxSendRunsBrokenPartialCountsAborted.cfg` | `ResumeExact` | 36,807 | 45,517 | 21.8 |
| `MCRNMsgxSendRunsBrokenUnstableSort.cfg` | `ClientOrder` | 27,832 | 32,726 | 15.4 |
| `MCRNMsgxSendRunsBrokenRunCrossesGeneration.cfg` | `NoCrossClient` | 36,750 | 45,460 | 8.5 |
| `MCRNMsgxSendRunsBrokenDeadNoResend.cfg` | `CursorSafe` | 36,712 | 45,416 | 19.5 |

## Bound to the code

`TraceRNMsgxSendRuns.tla` is the trace check of this module (layer
`msgxsendruns`, `proofs/tla/README-TraceCheck.md`): the seeded walk of
`tests/tla_trace_msgx_send_runs.cpp` stages one bank of 18 to 24 packets and
drives the real sort and `rocketnet_msgx_send_runs_step` over a fake table
and fake sends, one record per step with the action the call's own answer
names. The bank is the walk's input, given to the cfg as `Bank` (`{}` in every
model config: every bank `Init` admits), so a trace's `Init` is that one
state. The plant, a run builder that crosses a generation
(`rocketnet_msgx_send_runs_same_generation`, `sdk/rocketnet_msgx_send_runs.cpp`),
must be rejected at the first record it changes; `MCTraceRNMsgxSendRunsSelf.cfg`
and `MCTraceRNMsgxSendRunsSelfBrokenSortBlind.cfg` are the checker's own
controls. The unit tests (`tests/test_unit_msgx_send_runs.cpp`) also run the
real header over a fake, including a seeded walk of 2,000 random banks against
these invariants.
