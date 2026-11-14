# Notes for package R from M13 loopback-link (models/v1.1.x/loopback-link)

Rows: MM192-MM202 and the MM218 RNSimulatorStoreLayout instance. The record rows of this
package's own modules (COVERAGE.tsv rows link, loopback-queue, loopback-hub-registration,
loopback-hub-layout; CASES.tsv and RUNS.tsv rows of groups link, loopbackqueue, loopbackhub,
loopbackhublayout) are already edited on this branch. Everything below is for R to apply.

## Roadmap (docs/roadmap.sexp)

- Remove, as done on this branch, the item fix/v1.1.x/loopback 85704ab03 added:
  `loopback-queue-model-full-drop` ("LoopbackQueue cannot reach its full drop, and its Drop
  does not count"). MCLoopbackQueue.cfg now reaches the full drop and the wrap, the Drop
  counts, and MCLoopbackQueueBrokenUncountedDrop.cfg is the reversed witness.
- MM194's code half is a roadmap entry (Rowan's decision: caller misuse, low). The entry
  already exists on main as `loopback-hub-destroy-check-and-free` (group review-v1-2, area
  sdk), so no new entry is added and it is not duplicated. Keep it. Its last sentence asks to "make
  LoopbackHub.tla's Destroy (atomic today) two steps with a reversed witness": that model half
  is done here (Destroy has the two steps; MCLoopbackHubBrokenDestroyRace.cfg). When the code
  fix lands, the fix keeps CallerSerializesDestroy = FALSE passing for a create that takes the
  mutex after the check. The entry as it should read (same id, so it replaces the existing
  item in place; only the line numbers and the model sentence change):

```
  (item "loopback-hub-destroy-check-and-free" :group "review-v1-2" :area "sdk"
   :title "Loopback hub destroy checks for endpoints, then frees the hub outside the lock"
   :text "rocketnet_loopback_hub_destroy (sdk/rocketnet_driver_loopback.cpp about 572-587) reads registered under the hub mutex, releases it, then destroys the mutex and frees the block. A create on another thread in that window registers into memory about to be freed. Creating while destroying is misuse (low), but the in-use guard reads as if it catches it. Make the check and the retirement one step under the mutex (a create after it refused with the hub's in-use or mismatch status) and show it red with two threads through a deterministic seam. LoopbackHub.tla already has Destroy's two steps, and MCLoopbackHubBrokenDestroyRace.cfg (CallerSerializesDestroy = FALSE) shows the window; with the fix, a create that takes the mutex after the check is refused, so the model's retiring hub refuses it and that case passes. A create that is already waiting on the mutex when the hub is freed stays misuse that no flag can catch: say so in rocketnet.h."
   :date "2026-11-13" :origin "Review before v1.2.0, second wave, o3 finding W2-11 (read, not run); models fix MM194")
```

No other new roadmap entries: no other row of this package was a missing model or binding.

## proofs/tla/README.md, group table

```
| `link` | Shared lossy-link abstraction and loss patterns; the simulator's slot-store order and store-full drop (MCLinkPatterns SpecSlotStore) and its live setting hand-over (MCLinkReconfigure). |
| `loopbackqueue` | The loopback endpoint ring: FIFO, the full drop, the restart at empty and the slots it bounds (LoopbackQueue.tla). |
| `loopbackhublayout` | The loopback hub's queue layout, ring arithmetic, record reset and the carve with red zones at capacity 3 (LoopbackHubLayout.tla). |
```

(`loopbackqueue` has no row in that table today; add it beside `loopbackhub`.)

## docs/TLA-INVENTORY.md

- Line 72 (link simulator store), append to the guarantees cell: "; the packets due at a receive
  leave in slot order, not send order (MCLinkSlotStoreReorders), so a model over the simulator
  needs Reordering = TRUE; a send to a full store is dropped and counted (StoreAccounts), which
  Link covers only under the arbitrary pattern or a store that never fills".
- Line 69 (loopback bind table), append: "; each endpoint ring FIFO with the full drop counted,
  the slots written bounded by the longest run of sends between empties, a record reset never
  leaving a stale cell readable, and the queues' carves with red zones disjoint inside the block".

## Cross-family text held here (needs a serial R step)

- `proofs/tla/Link.tla` (header, lines 37-40) still say the simulator's store-full drop "is a harness
  artefact at 4096 slots and is not modelled", and lines 42-47 do not say that a model over
  the simulator needs Reordering = TRUE. Editing Link.tla stales every group that instantiates
  Link (clientconnection, rnblocks, rnchannel, rnendpoint, rnslots, rnwindow, serverconnection,
  sessionacks, with their own packages' records), so it was not edited here; README-Link.md
  says the truth meanwhile. Text for the next serial records pass:

```
(* Capacity bounds the copies in flight.  Send and Duplicate are disabled  *)
(* at the bound rather than dropping, so Capacity is a model-size knob and *)
(* never a hidden loss source.  The simulator's own store-full drop is     *)
(* counted (store_refused) and modelled in MCLinkPatterns SpecSlotStore.   *)
...
(* of jitter to a copy in flight; Reorder swaps two adjacent copies.  With *)
(* Reordering = FALSE the link is in-order; with MaxDelay = 0 it has no    *)
(* latency and Age is a no-op.  The simulator is not in-order even with no *)
(* jitter: it delivers the due packets in slot order (MCLinkPatterns       *)
(* SpecSlotStore), so a model over it needs Reordering = TRUE.             *)
```

- `proofs/tla/RNSimulatorStoreLayout.tla` (the comment on SendTakesLowest, line 375): "drops it only when
  none is free" is true of the model only; the code also refuses at an exhausted generation
  (README-RNSimulatorStoreLayout.md now says so). Suggested: "the send places a packet in the
  lowest free slot, and drops it only when none is free (the code also drops at an exhausted
  generation, which is not modelled)". Editing it stales group rnsimulatorstorelayout.

## For package C: comment-only re-citations in sdk/

This branch edited one sdk file, comments only: sdk/rocketnet_driver_loopback.cpp, two comments
"proofs/tla/LoopbackHubLayout.tla Reset: ..." in loopback_create (before `ep->head = 0`) and in
loopback_destroy, because check_tla_coverage --actions-strict needs the new action Reset cited,
and the loopback_queue_pop comment, whose "only the slots its deepest backlog reached" was wrong
(MM192): it now cites LoopbackQueue.tla TouchedWithinRun. The rest is C's:

- sdk/rocketnet_network_simulator.cpp (rocketnet_network_simulator_reconfigure, lines 439-441): cites
  "MCSend", which does not exist, and says the setting in force at the send holds the packet.
  New text: "proofs/tla/MCLinkReconfigure.tla models the change: MCReconfigure writes the pending
  record, Take is the owner's uncontended try-lock that puts it in force, and a send acts on the
  setting last asked for unless a change still waits (SendUsesForce); the change removes nothing
  already in flight (NoInFlightDrop)."
- sdk/rocketnet_network_simulator.cpp (rocketnet_network_simulator_take_setting, lines 459-469):
  add "proofs/tla/MCLinkReconfigure.tla Take".
- sdk/rocketnet_network_simulator.h (the comment above rocketnet_network_simulator_take_setting, lines 178-183): same correction as reconfigure (a busy try-lock
  keeps the old setting in force until the next uncontended pass).
- sdk/rocketnet_server_transport.cpp (the comment above rocketnet_server_link_active, lines 1626-1632): "the setting in force at the send is the setting
  the caller last asked for: the change takes effect at once" is not what the code does when the
  try-lock is busy, and it cites Link.tla. New text: "Taking the setting applies the one the
  caller handed over when the simulator's mutex is free; when it is busy the old setting stays in
  force for this send and the next uncontended pass applies it
  (proofs/tla/MCLinkReconfigure.tla Take, SendUsesForce). A change removes nothing already in
  flight (NoInFlightDrop)."
- sdk/rocketnet_network_simulator.cpp (the Link.tla citation above ROCKETNET_SIMULATOR_NO_SLOT, lines 63-65): add after the Link action list "; the slot store
  delivers the due packets in slot order, not send order, and counts the store-full drop
  (proofs/tla/MCLinkPatterns.tla SpecSlotStore)".
- sdk/rocketnet_network_simulator.cpp (rocketnet_network_simulator_flush_window): may cite
  "proofs/tla/MCLinkPatterns.tla FlushWindow: any positive count, a short window reversed too".
