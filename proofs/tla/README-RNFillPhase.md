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
`MAIN_THREAD_CALL_OVERLAP` or `CLIENT_SEND_TWO_THREADS`, and asserts in a debug build.
It keeps no thread-local state: a thread is named by
`rocketnet_platform_thread_id`.

| Code | Model |
| --- | --- |
| `main_begin`: `main_calls` fetch_add | `MainAnnounce` |
| `main_begin`: load `fills_in`; refused when not 0 | `MainLook` |
| `main_end`: store the next epoch | `MainEpoch` |
| `main_end`: `main_calls` fetch_sub | `MainDrop` |
| `fill_begin`: load the epoch | `FillBegin` |
| `fill_begin`: one compare-exchange that sets the client word's in bit; refused when it is set | `FillLock` |
| `fill_begin`: refused, restoring the word, when another thread owns the client with a claim of this call's epoch; else store the owner and [epoch, depth 1, in] | `FillDecide` |
| `fill_begin`: `fills_in` fetch_add | `FillCount` |
| `fill_begin`: load `main_calls` and the epoch; refused, restoring the owner and the word, when a main operation is running or the epoch moved | `FillLook` |
| `fill_end`: clear the in bit and the depth | `FillUnclaim` |
| `fill_end`: `fills_in` fetch_sub | `FillLeave` |

`FillDecide` is one step in the model and three accesses in the code (load
the owner, store it, store the word). The in bit makes it one: only the
thread that set it writes the word or the owner until it clears it, and the
only reader that does not hold it (the nesting check) reads the owner only
when the word's depth is 1 or more, which the decide stores last.

Invariants:

- `Sound`: a main operation and a fill call are never both past their
  checks.
- `OneFiller`: in one fill phase, the calls on a client that pass the check
  are one filler's.
- `NoFalseAlarm`: every refusal is of a call or operation that broke the
  contract, or met a break. A fill call is refused justly when it overlapped
  a main operation (the ghost `fbroke`), when another filler passed on its
  client in this phase, or when another filler is inside a call on its
  client. A main operation is refused justly when a fill call is in
  progress.

Not modelled: nesting (`main_owner`, `main_depth`, a word's depth above 1),
where an inner call or operation is the outer one's own work and checks
nothing; a client index outside the server's range, which claims nothing
and checks only the overlap with a main operation (`FillCount`, `FillLook`,
`FillLeave`, the steps the model has); and a second main thread, whose
`MainAnnounce` is refused when `main_calls` was not 0. A thread id is reused
after its thread exits, so a thread that ends and a new thread that gets its
id are one filler to the check.

## Runs

| Config | Expect | Defect restored |
| --- | --- | --- |
| `MCRNFillPhase.cfg` | pass (292,929 distinct states) | none, at 2 fillers, 2 clients, 2 main operations |
| `MCRNFillPhaseN3.cfg` (bench) | pass | none, at 3 fillers, 2 clients and 3 main operations |
| `MCRNFillPhaseBrokenLookFirst.cfg` | `Sound` fails | each side looks before it announces |
| `MCRNFillPhaseBrokenLoadStore.cfg` | `OneFiller` fails | the lock is a load then a store, not a CAS |
| `MCRNFillPhaseBrokenNoReset.cfg` | `NoFalseAlarm` fails | `main_end` leaves the epoch, so the next phase's filler is refused |
| `MCRNFillPhaseBrokenNoEpochLook.cfg` | `OneFiller` fails | the look reads `main_calls` only |
| `MCRNFillPhaseBrokenNoRestore.cfg` | `NoFalseAlarm` fails | a refused look leaves its claim |
| `MCRNFillPhaseLatch.cfg` | pass | none, with the check not armed at create and armed by the game's first call that names a client |
| `MCRNFillPhaseBrokenArmedAtCreate.cfg` | `NoCheckBeforeOptIn` fails | the check is armed from create: the calls of a game that names no client enter it |

## What TLC found, checked against the code by hand

The first design (a word [epoch:32][thread:31][in:1] per client, per-thread
nesting depths in thread-local storage, a scan of every client's word in
`main_begin`):

1. **The look must read the epoch again.** f1 loads epoch 1 inside a main
   operation, and the operation ends (epoch 2). If f1's look reads only
   `main_calls` (0), f1 passes with its claim stamped 1, and f2 in epoch 2
   sees a stamp of an older phase and passes too. `fill_begin` compared
   only `main_calls`; it now also compares the epoch it loaded first
   (`MCRNFillPhaseBrokenNoEpochLook.cfg`, the same trace in this design).
2. **A stale claim must not erase the current phase's claim.** The first
   design's refused look stored its own claim, out, over the word it had
   replaced: a stale call erased the current phase's claim, and a second
   filler then passed. It was fixed first by refusing a claim of a later
   epoch. The design here restores the owner and the word instead, which
   covers every refused look, and the decide refuses only a claim of the
   caller's own epoch (a stale call that claims over a newer one holds the
   lock until its look refuses it and puts both back).
3. **The ghost counts a refused second filler.** A run failed
   `NoFalseAlarm` with no defect in the code: the ghost recorded a second
   filler only when its claim landed. The ghost is now per refusal
   (`Justified`).

The design here:

4. **A refused look leaves no claim** (`MCRNFillPhaseBrokenNoRestore.cfg`).
   main_end stores epoch 2, and before its `main_calls` fetch_sub, f1 loads
   epoch 2, locks, claims [2, f1] and counts itself; its look finds
   `main_calls` 1 and refuses. Had it left [2, out] with owner f1, f2's
   honest call in phase 2 would find another thread's claim of its own
   epoch and be refused. The code restores the owner and the word.

The check has a latch (`armed`; `rocketnet_fill_phase_arm`): the server creates
it not armed and arms it, for good, at the game's first call that names a
client (`Arm`, `ArmedStays`). Not armed, a call site's scope does nothing (one
load and a branch): the whole call is `FillSkip` or `MainSkip`, which touch no
word of the check, so a game on today's calls pays nothing and is never
refused. `NoCheckBeforeOptIn`: before the game opts in no call is inside the
check. `MCRNFillPhaseLatch.cfg` is the check as the server makes it;
`MCRNFillPhaseBrokenArmedAtCreate.cfg` arms from create and fails
`NoCheckBeforeOptIn`. The cases above run with `OptedAtStart`, the armed check.
Not in the model: a call that began before `Arm` and is still running after
it. The trace walk makes its first calls not armed, through the call sites'
scopes, then arms.

The trace check that binds the code to this model is
`TraceRNFillPhase.tla`, fed by the `rocketnet_fill_phase_set_trace` hook
(tests/tla_trace_fill_phase.cpp); its plant is the NoRestore defect.
