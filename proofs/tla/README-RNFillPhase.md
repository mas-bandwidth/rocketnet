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
| `fill_begin`: one compare-exchange that sets the client word's in bit; refused when it is set with depth 0 | `FillLock` |
| `fill_begin`: refused, restoring the word, when another thread owns the client with a claim of this call's epoch | `FillDecide` |
| `fill_begin`: `fills_in` fetch_add | `FillCount` |
| `fill_begin`: load `main_calls`, the epoch and the window; refused, restoring the word, when a main operation is running, the epoch moved or the window is open; else store the owner, then [epoch, depth 1, in] | `FillLook` |
| `fill_begin`: the first load found the word in with depth 1 or more | `FillPeek` |
| `fill_begin`: load the owner; this thread: store depth + 1 (refused as API misuse at `ROCKETNET_FILL_PHASE_DEPTH_MAX`); another: refused, counted | `FillNest` |
| `fill_end`: a nested call, depth - 1 | `FillUnnest` |
| `fill_end`: clear the in bit and the depth | `FillUnclaim` |
| `fill_end`: `fills_in` fetch_sub | `FillLeave` |

`FillDecide` is one step in the model and one load in the code (the owner).
`FillLook` is the three loads and, let in, two stores (the owner, then the
word). The in bit makes them one step: only the thread that set it writes
the word or the owner until it clears it, and the only reader that does not
hold it (the nested test, `FillPeek` then `FillNest`) loads the owner only
after a word with depth 1 or more, which a call stores after its owner and
only once its look has let it in. A refused call never writes the owner, so
no store of the owner follows a claim's word while the claim stands
(`ClaimHasOwner`, `NoFalseNest`).

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
- `NoFalseNest`: a fill call not inside one for its client is never taken
  as nested.
- `ClaimHasOwner`: a word that shows a claim (depth 1 or more) has as owner
  the filler inside a call for that client.

Fill calls nest at the code's granularity: a call made inside a fill call
for the same client on the same thread (`FillBegin` from `in` to `nbegun`,
`FillPeek`, `FillNest`, `FillUnnest`), with the word's depth up to
`DepthMax` (2 in the MCs, 127 in the trace check, the code's 7 bits). Not
modelled: nesting of main operations (`main_owner`, `main_depth`), where an
inner operation, or a fill call made inside one, is the outer one's own work
and checks nothing; a client index outside the server's range, which claims nothing
and checks only the overlap with a main operation (`FillCount`, `FillLook`,
`FillLeave`, the steps the model has); and a second main thread, whose
`MainAnnounce` is refused when `main_calls` was not 0. A thread id is reused
after its thread exits, so a thread that ends and a new thread that gets its
id are one filler to the check.

## Runs

| Config | Expect | Defect restored |
| --- | --- | --- |
| `MCRNFillPhase.cfg` | pass (1,423,875 distinct states) | none, at 2 fillers, 2 clients, 2 main operations |
| `MCRNFillPhaseN3.cfg` (bench) | pass, not reached | none, at 3 fillers, 2 clients and 3 main operations. Declared timeout debt: a manual run on a 16-core bench with 2 workers and a one-hour bound (2026-11-13, with nesting at `DepthMax` 2) stopped at 233,108,552 distinct states with 48,947,054 left on the queue and still growing, at depth 33, no violation |
| `MCRNFillPhaseBrokenLookFirst.cfg` | `Sound` fails | each side looks before it announces |
| `MCRNFillPhaseBrokenLoadStore.cfg` | `OneFiller` fails | the lock is a load then a store, not a CAS |
| `MCRNFillPhaseBrokenNoReset.cfg` | `NoFalseAlarm` fails | `main_end` leaves the epoch, so the next phase's filler is refused |
| `MCRNFillPhaseBrokenNoEpochLook.cfg` | `OneFiller` fails | the look reads `main_calls` only |
| `MCRNFillPhaseBrokenNoRestore.cfg` | `NoFalseAlarm` fails | a refused look leaves its claim |
| `MCRNFillPhaseLatch.cfg` | pass | none, with the check not armed at create and armed by the game's first finish_client_message, abort_client_message or update_receive |
| `MCRNFillPhaseBrokenArmedAtCreate.cfg` | `NoCheckBeforeOptIn` fails | the check is armed from create: the calls of a game that names no client enter it |
| `MCRNFillPhaseTakes.cfg` | pass | none, with takes (`update_client`) beside one fill call per filler, from not armed |
| `MCRNFillPhaseBrokenNoWindow.cfg` | `WindowShut` fails | a fill call's look does not read the window |
| `MCRNFillPhaseBrokenNoMainWindow.cfg` | `WindowShut` fails | `main_begin` does not read the window |
| `MCRNFillPhaseBrokenTakeUncounted.cfg` | `TakeSound` fails | a take does not count itself on a `fills_in` line |
| `MCRNFillPhaseBrokenOwnerAtDecide.cfg` | `NoFalseNest` fails | the decide stores the owner and the claim, and a refused look puts them back in three stores |
| `MCRNFillPhaseBrokenKickAll.cfg` | `NoKickOnOpenUpdate` fails | an update call that finds the update open finishes it and begins the next in the same call |
| `MCRNFillPhaseBrokenShutOnEarly.cfg` | `StageShut` fails | an `update_receive` that returns before the transport's receive leaves the update open and the window shut |
| `MCRNFillPhaseBrokenScanFirst.cfg` | `PlainChecked` fails | the plain finish and abort scan for the open message before any check |

Every run lets the main thread's update calls open and shut the split
update (`MainEpoch(w, s)`; `update_receive` opens it, `update_finish` and
`update` close it, `stage` is the message layer's stage and `window` the
check's word, which follows it) and its other operations meet it (`MainIn`).

The model's operations are the layer-3 API's (`sdk/rocketnet_api.cpp`). The
layer-1 and layer-2 entry points in `sdk/rocketnet_layers.h` construct the
same scopes, but only the layer-3 API arms the check
(`rocketnet_fill_phase_arm` in `sdk/rocketnet_api.cpp`; the transport's
create leaves it not armed). Used on their own, with no layer-3 server over
them, those scopes find the check not armed and do nothing, by contract;
under a layer-3 server whose check is armed they are checked as part of the
layer-3 call that reaches them.

## The window of the split update (found in review by Stella)

Between `rocketnet_server_update_receive` and `rocketnet_server_update_finish`
nothing but `update_client` and the update calls may run. Before this change
the check knew nothing of that window: `update_finish` took no main-thread
scope, `update_client` took none either, and a send, a stop or a fill call in
the window ran unrefused. The check now keeps `window` (stored by the
transport's receive and finish, inside their main operation), `main_begin`
refuses any operation but an update call while it is set (`MainIn`), a fill
call's look refuses while it is set (`FillLook`), and `update_client` is a
take: it counts itself on its `fills_in` line, then looks at `main_calls`
(`TakeCount`, `TakeLook`), so `update_finish`'s look sees a take still
running. The three witnesses' traces, checked against the code by hand:

- `BrokenTakeUncounted`: `TakeCount`, `TakeLook` (`main_calls` 0), then
  `MainAnnounce` and a `MainLook` per line that each read 0, and `MainIn` with
  the take still in. In the code `rocketnet_fill_phase_take_begin`'s
  `fetch_add` on the line is what `main_begin`'s loop reads; without it the
  loop finds every line 0.
- `BrokenNoWindow`: an update call opens the window (`MainEpoch(TRUE)`,
  `MainDrop`), then a fill call passes `FillLook` with `main_calls` 0 and the
  epoch unchanged. The code's look reads `window` beside `main_calls` and the
  epoch; without that read nothing else refuses it.
- `BrokenNoMainWindow`: an update call opens the window, then an `other`
  operation's `MainAnnounce`, looks and `MainIn` pass. The code's `main_begin`
  reads `window` after the looks for any call not begun with `update_call`.

## The update in three calls, pass 2 (found in review, 2026-11-13)

Three rules of the message layer around the window, each with a reversed
witness whose trace was checked against the old code by hand:

- `BrokenKickAll` (12 states): an `update_receive` opens the update
  (`MainEpoch(TRUE, TRUE)`, `MainDrop`), then another update call finds it
  open and its work closes it with `kicked` set. In the old
  `rn_message_object_update_begin`, an open update was finished and the
  call went on to begin the next, whose not-received check ended every
  connection the finish had just handed messages to: one refused
  `update_finish` on a late join disconnected every active client. Now the
  call counts one misuse, finishes the open update and begins none
  (`KickedBy`, `NoKickOnOpenUpdate`); `test_wide_fill_split_update_open_update_call`.
- `BrokenShutOnEarly` (11 states): an `update_receive` returns before the
  transport's receive (`MainEpoch(FALSE, TRUE)`: the update open, the window
  shut), then an `other` operation's looks and `MainIn` pass. In the old code
  the window was stored only by the transport's receive, which an
  `update_receive` with a time that is not finite never reached; a
  `send_packets` then ran inside the open update. The message server now
  stores the window from its stage as each update call's work ends
  (`UpdateWork`, `StageShut`); `test_wide_fill_split_update_window_on_early_return`.
- `BrokenScanFirst` (7 states): f1's fill call passes its look (`in`), then
  the plain finish scans for the open message (`PlainScan`) beside it, with
  no check. The old plain `finish_message` and `abort_message` scanned
  every connection before any scope, and checked only when another thread
  held the client. They now take a main scope first: a main operation of
  kind `other`, refused in the window or beside a fill call
  (`PlainChecked`); `test_wide_fill_plain_finish_checked`.

The trace walk drives a bare check with no message server, so it records
`stage` from its own update calls and never takes `PlainScan`; these three
rules are bound to the code by the native tests named above, not by the
trace (roadmap `fill-phase-trace-gaps`).

## The nested test (found in review, 2026-11-13)

The nested test was two loads, the word then the owner, and a refused look
put back the word's lock form, then the owner, then the word. A reader
reproduced the damage under stress on the unmodified file: with two
fillers on one client and a main thread, every one of 200 rounds left the
check broken, the client's word in with nobody inside (172) or a `fills_in`
line wrapped to 4294967295 (30), after which every main operation was
refused. `MCRNFillPhaseBrokenOwnerAtDecide.cfg` restores the old order. Its
trace (22 states, 413,249 distinct), checked against the old code by hand:
f1 fills c1 in epoch 1 and ends (the owner stays f1). A main operation
announces (`main_calls` 1) and, still running, moves the epoch to 2. f2 locks c1, decides (owner f2, word [2, 1, in])
and counts itself. f1 begins, loads the word [2, 1, in] (`FillPeek`). f2's
look finds `main_calls` 1 and refuses: the word to [1, 0, in]
(`FillLook`), the owner back to f1 (`FillUnwind1`). f1 loads the owner,
finds itself and takes the call as nested, storing [2, 2, in]
(`FillNest`): it counted nothing, and its end would take one off a
`fills_in` line it never counted on. The code now stores the owner and the
claim only after the look lets the call in, and a refused look writes the
word alone. `test_wide_fill_check_nested_never_torn` drives the same
interleaving through the trace hook (red on the old order);
`test_wide_fill_check_depth_bound` is the depth's top, which carried into
the epoch before it was refused.

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
   epoch. The design here restores the word instead, which covers every
   refused look, and the decide refuses only a claim of the caller's own
   epoch (a stale call that claims over a newer one holds the lock until its
   look refuses it and puts the word back; it has stored no owner, which only
   a call the look lets in stores).
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
   epoch and be refused. The code restores the word (it has stored no
   owner: the owner is stored only by a call the look lets in).

The check has a latch (`armed`; `rocketnet_fill_phase_arm`): the server transport creates
it not armed and the server arms it, for good, at the game's first
`rocketnet_server_finish_client_message`, `rocketnet_server_abort_client_message`
or `rocketnet_server_update_receive` (`Arm`, `ArmedStays`; the calls
`rocketnet.h` names: `rocketnet_server_start_message` names a client but does
not arm it), and arms each
later transport at the start that creates it, so a stop and a start keep it
armed (`test_wide_fill_check_armed_across_restart`). Not armed, a call site's scope does nothing (one
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
