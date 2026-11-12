# RNFillPhase: the fill-phase check

`RNFillPhase.tla` models the check in `sdk/rocketnet_fill_phase.cpp` that
the server transport (`sdk/rocketnet_server_transport.cpp`) and the message
server (`sdk/rocketnet_message_server.cpp`) wrap around every main-thread
operation and every fill call.

## The contract and the check

Main-thread operations (update, send_packets, receive, stop, disconnect)
run on one thread. Fill calls (start, finish and abort of a packet or a
message) may run on any number of game threads between an update and the
next send_packets. Each client has one filler per fill phase. Breaking the
contract is caller misuse. The check refuses the call, counts
`FILL_PHASE_OVERLAP` or `FILL_TWO_FILLERS`, and asserts in a debug build.

| Code | Model |
| --- | --- |
| `main_calls` fetch_add in `main_begin` | `MainAnnounce` |
| the scan of every client's in bit | `MainLook(c)`, then `MainIn` |
| `main_end`: epoch advance, then `main_calls` fetch_sub | `MainLeave` |
| `fill_begin`: load the epoch | `FillBegin` |
| `fill_begin`: one compare-exchange of `claims[c]` to [epoch, thread, in] | `FillClaim` |
| `fill_begin`: load `main_calls` and the epoch again | `FillLook` |
| `fill_end`: clear the in bit | `FillLeave` |

The claim refuses a word held by another thread when its in bit is set,
or when it is stamped with the caller's epoch or a later one (a signed
32-bit difference in the code, so the wrap is safe). The look refuses when
a main operation is running or the epoch has moved since `FillBegin`.

Invariants:

- `Sound`: a main operation and a fill call are never both past their
  checks unless one was flagged.
- `OneFiller`: two fillers never claim one client in one phase unless one
  was flagged.
- `NoFalseAlarm`: nothing is flagged unless the contract was broken.

The per-thread nesting depths are not modelled. A fill call made inside a
main operation or inside another fill call is the outer call's own work,
and it skips the check.

## Runs

| Config | Expect | Defect restored |
| --- | --- | --- |
| `MCRNFillPhase.cfg` | pass (4,114 distinct states) | none |
| `MCRNFillPhaseN3.cfg` (bench) | pass (8,758,413 distinct states) | none, at 3 fillers, 2 clients and 3 main operations |
| `MCRNFillPhaseBrokenLookFirst.cfg` | `Sound` fails | each side looks before it announces |
| `MCRNFillPhaseBrokenLoadStore.cfg` | `OneFiller` fails | the claim is a load then a store, not a CAS |
| `MCRNFillPhaseBrokenNoReset.cfg` | `NoFalseAlarm` fails | `main_end` leaves the epoch, so the next phase's filler is flagged |
| `MCRNFillPhaseBrokenStaleClaim.cfg` | `OneFiller` fails | the claim refuses only the caller's own epoch |
| `MCRNFillPhaseBrokenNoEpochLook.cfg` | `OneFiller` fails | the look reads `main_calls` only |

## What TLC found, checked against the code by hand

1. **The look must read the epoch again.** f1 loads epoch 0 inside a main
   operation, and the operation ends (epoch 1). If f1's claim is stamped 0
   and its look reads only `main_calls` (0), f1 goes in. f2 in epoch 1 then
   sees a word stamped 0 and passes. The code had this hole: `fill_begin`
   compared only `main_calls`. It now also compares `epoch.load()` with the
   epoch it loaded first (`MCRNFillPhaseBrokenNoEpochLook.cfg`).
2. **A stale claim must not overwrite the current phase.** f1 loads epoch 0
   and a main operation ends. f2 claims, fills and leaves in epoch 1 (word
   [1, f2, out]). f1's compare-exchange then saw a word of another thread,
   not in and not of f1's epoch, and overwrote it with [0, f1, in]. f1's
   look refused it but stored [0, f1, out]. That erased f2's claim, so f1
   could fill again in epoch 1 next to f2, unflagged. The code compared the
   word's epoch for equality. It now refuses a word of the caller's epoch
   or a later one (`MCRNFillPhaseBrokenStaleClaim.cfg`).
3. **The ghost counts a refused second filler.** The first committed run
   failed `NoFalseAlarm` with no defect in the code. f2 claims c1, then
   f1's claim on c1 is refused and flagged, but the ghost recorded a second
   filler only when its claim landed. `Second(f)` now marks both cases.

The trace check that binds the code to this model is
`TraceRNFillPhase.tla`, fed by the `rocketnet_fill_phase_set_trace` hook.
