# RNUringSendPhases: the io_uring send staging's phases at the code's layout

`RNUringSendPhases.tla` models the send staging of the io_uring server
transport (`sdk/rocketnet_server_uring_linux.cpp`) with the code's own index
arithmetic: two phases of slots in one array, the fill phase start_packet
reserves from, each phase's reservation index, submitted mark and pending
count, and the phase a send's completion is charged to. `RNUringDriver` is
the abstract model of the same driver; its slots are one phase, and its
`ResetIndex` is one phase's reset. The `io-uring-send-phases` row in
`COVERAGE.tsv` tracks the coverage.

## Why two phases

`send_packets` resets a phase's index only when every send of it has its
completion. Under an inbound flood more than `ROCKETNET_URING_SEND_BUDGET`
completions can arrive ahead of an update's sends, so `send_packets` spends
its budget on receives and the sends are reaped one call late, by
`receive_packets`. With one phase the index was still up when the next
update staged, `start_packet` ran out of slots, and outbound went out about
one update in three. With two, the next update fills the other phase,
complete since its sends were reaped, and the late phase is reset by the
`send_packets` after. RIO's send handoff has a third phase for its worker
thread; io_uring submits on the main thread.

## The layout

| Model | Code |
|---|---|
| `Phases`, `Size` | `ROCKETNET_URING_SEND_PHASES`, `ROCKETNET_URING_SEND_QUEUE_SIZE` |
| `fill` | `send.fill` |
| `num[p]` | `send.num_packets[p]` |
| `submitted[p]` | `send_submitted[p]` |
| `pendC[p]` | `send_pending[p]` |
| `Base(p) + i`, `SlotOf` | the staging slot `phase * ROCKETNET_URING_SEND_QUEUE_SIZE + index` in `rocketnet_server_uring_start_packet` |
| `p * Size + i`, `NewSends` | the staging slot `base + slot` that `rocketnet_server_uring_submit_phase` names in the send's tag |
| `PhaseOf(g)` | `slot / ROCKETNET_URING_SEND_QUEUE_SIZE` in the send case of `rocketnet_server_uring_reap_cqe` |
| `unposted`, `cq`, `Kern` | sends the kernel holds; their posted completions, oldest first; the slots the kernel owns |
| `written` | the slots the fill threads wrote since the last submit |

## Actions

| Action | Code |
|---|---|
| `Reserve` | `rocketnet_server_uring_start_packet`: one fetch_add on the fill phase's index, NULL past `Size` |
| `SendBegin` | `rocketnet_server_uring_send_packets` submits every staged slot not yet submitted, the committed phase's before the fill phase's (`rocketnet_server_uring_submit_phase`) |
| `KernelComplete(g)` | the kernel posts a send's completion (environment action) |
| `Reap` | a send completion reaped, in `send_packets` or `receive_packets`: its phase's pending count falls |
| `SendEnd` | the end of `send_packets`: every complete phase (every slot submitted, none pending) has its index and submitted mark reset; when the fill phase still holds staged slots and the next phase is empty, the fill moves to it |
| `RecvEnd` | the end of `receive_packets` and of the update; under `LateReap` every send submitted is reaped by now |

The model submits every staged slot at once; the code submits in chunks the
submission queue has room for, and a chunk the kernel takes nothing of ends
the call, the rest submitted by a later one in the same order.

## Properties

- `NoWriteWhileKernelOwned`: no fill thread writes a staging slot the kernel
  still owns.
- `PendingMatches`: each phase's pending count is the number of its sends
  the kernel owns, so a phase is reset only once none is.
- `NeverRefusedUnderLateReap`: when every completion is reaped by the next
  receive call, `start_packet` never runs out of slots for an update's
  sends (at most a phase's worth). Checked only by the late-reap cases.

## Cases

Every case has `Size = 2` and four updates. Group `rnuringsendphases` in
`CASES.tsv`; the records are in `RUNS.tsv`, measured on a 17-cpu
linux-amd64 bench (Java 21.0.12.1, the pinned JAR, two workers).

| Case | Change | Expected | Exit | Distinct | Seconds |
|---|---|---|---:|---:|---:|
| `MCRNUringSendPhases.cfg` | the code, any completion late any number of calls | pass | 0 | 12752 | 1.6 |
| `MCRNUringSendPhasesLateReap.cfg` | the code, every completion reaped by the next receive call | pass | 0 | 390 | 1.1 |
| `MCRNUringSendPhasesBrokenOnePhase.cfg` | `Phases = 1`, the staging before the phases | `NeverRefusedUnderLateReap` | 12 | 54 | 1.0 |
| `MCRNUringSendPhasesBrokenNoBase.cfg` | `BaseRule = FALSE`: start_packet's slot without the phase base | `NoWriteWhileKernelOwned` | 12 | 29 | 1.0 |
| `MCRNUringSendPhasesBrokenRemainder.cfg` | `QuotientRule = FALSE`: a completion's phase is its slot's remainder | `PendingMatches` | 12 | 107 | 1.0 |

## Counterexamples, checked by hand against the code

**BrokenOnePhase**: update 0 reserves slot 0 and `send_packets` submits it;
its completion is posted but not reaped in the call, so `SendEnd` finds the
phase incomplete and leaves the index at 1, and the fill cannot move (the
next phase is the same one). `receive_packets` reaps the completion. Update
1 reserves slot 1, and the next `start_packet` finds the index at `Size`:
refused. In the code before the phases, `send_packets` stored 0 into the
one `num_packets` only when `sends_pending` was 0 at its end, so the same
order returned NULL from `start_packet`;
`test_io_uring_transport_send_phases_stage_every_update_under_late_reap`
fails at its first update after the first on that code.

**BrokenNoBase**: update 0 stages and submits staging slot 0, and the
kernel keeps it; `SendEnd` moves the fill to phase 1. Update 1's
`start_packet` takes index 0 of phase 1 and, without the base, returns
staging slot 0, the slot the kernel still sends from.

**BrokenRemainder**: phase 0's slot 0 and phase 1's slot 2 are submitted
and the kernel posts slot 2's completion first. Its remainder by `Size` is
0, so the reap lowers phase 0's count while the kernel still owns slot 0,
and phase 1's count stays at 1 with nothing pending: phase 0 could be reset
under the kernel, and phase 1 never.
