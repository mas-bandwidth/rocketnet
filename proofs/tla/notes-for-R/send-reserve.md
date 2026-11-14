# Notes for package R from package M5c (send-reserve)

Branch `models/v1.1.x/send-reserve`. Rows MM074-MM078. The records this
package owns (COVERAGE rows `send-reserve`, `send-reserve-calls`; the
`rnsendreserve` rows of CASES.tsv, RUNS.tsv and BUDGETS.tsv) are edited on the
branch. Everything below is for R to apply.

## docs/roadmap.sexp

Two items the branch settles; R closes or removes them as the roadmap's
rules say:

- `send-reserve-tails-named-implied` (MM076): settled. `TailsNamed` is
  restated on the code's own counter read (every line's word, one line a
  step, after the take back and the retest), to the counter meaning settled
  for MM076 (branch fix/v1.1.x/reserve-refusal-counter-wording): a refusal
  counted SEND_RESERVE_REFUSED_TAILS_HELD had another filler holding frames
  while the read ran, judged by the fillers' own state; the tails row is a
  lower bound and the two rows together exact. It no longer follows from
  `NeverStarved`; the code passes it (`MCRNSendReserveCodeTailsNamed.cfg`
  and every passing control) and its reversed witness
  `MCRNSendReserveBrokenNameAll.cfg` fails it. `MCRNSendReserveBrokenNoTailReturn.cfg`
  now fails `NeverStarved`.
- `send-reserve-multi-queue-capacity` (MM074): closed by send-queue-capacity
  17f32a979 (`CapacityTo`, `CapacityAll`, `NQ` queues, the Queues instance and
  `NoHiddenRoom`, trace seeds 3 and 4 on two queues).

One new item:

```
  (item "send-reserve-calls-refinement-checked" :group "proof-debt" :area "proofs"
   :title "Check the refinement RNSendReserveCalls to RNSendReserve"
   :text "RNSendReserveCalls (the module the trace check binds the code to) is stated to be RNSendReserve run one thread at a time, each action a run of its steps with no other thread's step between them (the mapping is in its header). That is argued, not checked: a change to one module can leave the other behind with every case green. Check it with TLC: an instance of RNSendReserve with one filler and one line, its variables mapped to the calls module's (ownTaken to ownUsed between calls, tail to the one line's tail), and the calls module's Spec as the property, at the sizes of MCRNSendReserveCalls.cfg and MCRNSendReserveCallsChunk.cfg; then a reversed witness (a calls action changed so no step run makes it) that fails the check."
   :date "2026-11-13" :origin "models round, MM078 (third part) and MM075 (its alternative)")
```

## docs/TLA-INVENTORY.md

Neither module is in the table. A line for it, after "server send loop and
budgets":

```
| protocol send reserve | `RNSendReserve`, `RNSendReserveCalls` | the game's packets never take the room the next update's protocol frames need (`ReserveKept`); a game start is never refused while the room it read had a frame for it past the reserve and every frame held (`NeverStarved`); several threads claim at the code's layout (fill lines, chunk claims, tails taken by compare-and-swap); per send queue, a client's capacity is its own queue's (`NoHiddenRoom`) and a reserve cut to half a small room grows back (`ReserveRestored`); the code is bound through `RNSendReserveCalls` (trace layer `sendreserve`); a refusal counted on the tails row had another thread holding frames when it read the lines, so that row is a lower bound and the two refusal rows together exact (`TailsNamed`) |
```

## proofs/tla/README.md (the groups table)

`rnsendreserve` has no row. One, in the table's order:

```
| `rnsendreserve` | The protocol send reserve of a staged send queue: RNSendReserve, several threads at the code's layout, and RNSendReserveCalls, one call at a time on several queues, the module the trace check binds (README-RNSendReserve.md). |
```

## MM217 (missing READMEs)

`RNSendReserveCalls` is documented in `README-RNSendReserve.md` (its own
section "RNSendReserveCalls: one action per call", its configs table and
the trace check). R may list that README as the calls module's.

## Cross-family rows

- RNSendReserve and RNFillPhase share the fill lines
  (`rocketnet_server_fill_line_t`, `rocketnet_fill_phase_line_of`): RNFillPhase
  owns the phase and the line a thread hashes to, RNSendReserve the `started`
  and `tails` words on each line. Neither model has the other's words; a
  cross-family row names the line hash as the shared assumption
  (`SharedLine` in RNSendReserve: every filler on one line, or each on its
  own).

## Package C: comment re-citations

Each is a comment only; the line numbers are at this branch's head and are given here only to find the comment.

- `sdk/rocketnet_server_transport.cpp lines 239-240`: "tail + held, summed over
  the lines, is the code's tail_held (proofs/tla/RNSendReserve.tla
  TailHeld)." becomes "tail + held, summed over the lines, is what
  rocketnet_server_transport_tails_held reads to pick a refusal's counter
  (proofs/tla/RNSendReserve.tla TailHeld, read one line a step in
  Retest)." (TailHeld is defined again in the model; only the function's
  name and where it is read change.)
- `sdk/rocketnet_server_transport.cpp lines 241-242`: "or by the filler when the
  transport refuses a frame (RNSendReserve End)." becomes "or by the filler
  when the transport refuses a frame (RNSendReserve Stage refused, then
  End)."
- `sdk/rocketnet_server_transport.cpp lines 1277-1278`: "Refused (Refuse): taken
  back whole (TakeBack) and false." becomes "Refused (Decide's test): taken
  back whole (TakeBack) and false." (there is no action Refuse.)
- `sdk/rocketnet_server_transport.cpp lines 1308-1309`: "OwnClaim, Continue, Size,
  Claim, Check, Read1-3, Decide, Refuse, TakeBack, Retest" becomes "OwnClaim,
  Continue, Size, Claim, Check, Read1-3, Decide, TakeBack, Retest".
- `sdk/rocketnet_server_transport.cpp lines 1317-1319`: "A single claim refused is
  counted SEND_RESERVE_REFUSED_TAILS_HELD while any line holds a tail or a
  chunk claim (TailsNamed), else SEND_RESERVE_REFUSED." becomes "A single
  claim refused is counted SEND_RESERVE_REFUSED_TAILS_HELD when any line's
  word shows a tail or a chunk claim when it reads them, after the retest,
  else SEND_RESERVE_REFUSED. Frames held at the decision may by then be on
  no line word or staged, and that refusal is counted plain: the tails row
  is a lower bound, the two rows together exact (RNSendReserve
  TailsNamed)." Unless fix/v1.1.x/reserve-refusal-counter-wording words
  this comment itself; then its text.
- `sdk/rocketnet_server_transport.cpp lines 1424-1428`: "proofs/tla/RNSendReserve.tla
  Stage, a queue with no room, and End: the transport refused the granted
  frame, so the run ends: the frame's claim and the line's tail go back in
  one add" becomes "proofs/tla/RNSendReserve.tla Stage refused (room or not)
  and End: the transport refused the granted frame, so the run ends: the
  line's tail is taken off its word with one compare-and-swap, then it and
  the frame's claim go back to the claims in one add".
- `sdk/rocketnet_server_transport.cpp lines 1458`: "(RNSendReserve End at the
  phase's end)" becomes "(RNSendReserve OpenFill)".
- `tests/test_unit_server_send_reserve.cpp lines 384`: "RNSendReserve Stage, a
  queue that refuses:" becomes "RNSendReserve Stage refused, then End:".
