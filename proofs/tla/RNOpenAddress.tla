---------------------------- MODULE RNOpenAddress ----------------------------
(***************************************************************************)
(* The open-addressed index as the code lays it out: an array of N cells, *)
(* linear probing from a key's home with wraparound, and deletion by       *)
(* backward shift, one cell per step (sdk/rocketnet_encryption_map.cpp    *)
(* rocketnet_encryption_map_erase_index, sdk/rocketnet_token_tables.cpp,   *)
(* sdk/rocketnet_client_slots.cpp rocketnet_client_slots_erase_index,      *)
(* sdk/rocketnet_channel.cpp rn_held_erase, sdk/rocketnet_window_ack.cpp   *)
(* rn_sent_index).  The keys are the network's: the sender chooses addresses,*)
(* tokens and (channel, ordering), so it chooses the clusters.             *)
(*                                                                         *)
(* home is chosen once, at Init, from every map of keys to cells: TLC      *)
(* checks every hash function at once, clusters that wrap the array end    *)
(* included.  At most MaxLive keys are live.                               *)
(*                                                                         *)
(* N is the index's cell count, which in every index here is twice the    *)
(* table's capacity (the encryption map's index_cells, the token tables'   *)
(* index_capacity), so at most N \div 2 keys are live in the code.         *)
(*                                                                         *)
(* Two erase shapes are in the code.  ClearLast FALSE clears the hole      *)
(* first (the encryption map, the token index); the scan examines at most *)
(* N cells (EraseBound, the code's step < index_cells), and Findable      *)
(* holds even with a full index (MaxLive = N, MCRNOpenAddress.cfg), which  *)
(* no index in the code reaches; MCRNOpenAddressTwiceCapacity.cfg is the   *)
(* code's load factor (MaxLive = N \div 2).  ClearLast TRUE leaves the    *)
(* hole marked and copies each                                             *)
(* moved entry back, clearing only the final hole (client slots, the held  *)
(* index, the sent index): the scan ends only on a cell empty before the   *)
(* erase, which needs MaxLive < N; the code sizes those indexes at twice   *)
(* their capacity.  scanned counts the scan; EraseEnds says it never     *)
(* passes N.  Each clear-last site in the code also bounds its scan, for   *)
(* an index corrupted full: client slots at num_buckets cells, the held    *)
(* index at N - 1 cells and then an invariant failure that ends the        *)
(* connection (RNHeldOwnership.tla EraseBound models it), the sent index   *)
(* at cap cells and then a protocol violation.  Those exits are not here:  *)
(* with MaxLive < N the scan meets an empty cell first, so they are never  *)
(* taken in the configurations that model the code.                       *)
(*                                                                         *)
(* Findable says every live key is reached by probing from its home        *)
(* before an empty cell; Unique says no key sits in two cells.             *)
(*                                                                         *)
(* Reversed witnesses: NoWrapTest TRUE judges "home lies in (hole, scan]"  *)
(* without the wrapped case, so a cluster crossing the array end moves an  *)
(* entry in front of its home, where no probe finds it (Findable).         *)
(* ClearLast TRUE with MaxLive = N is a clear-last index allowed to fill:  *)
(* its scan circles with no empty cell to stop it (EraseEnds).  That       *)
(* witness is historical: it is the clear-last algorithm without a bound,  *)
(* the erasers before their bounds were added.  No current site can        *)
(* circle (each is bounded, above); what it still shows is why a           *)
(* clear-last index is sized so that it never fills.                       *)
(*                                                                         *)
(* Insert of a live key: the sent index's insert stops at the first cell   *)
(* that is empty or already holds the key, and writes there               *)
(* (rn_sent_index_insert), so a live key is overwritten in place, cells    *)
(* unchanged, as long as Findable holds.  Insert carries that branch for   *)
(* every index (the other indexes never insert a live key).                *)
(*                                                                         *)
(* The separate counter.  The held index (sdk/rocketnet_channel.cpp) keeps *)
(* its own count of used cells, region->held_count, beside the cells, and  *)
(* its bound reads the counter, not the cells: rn_held_insert refuses at   *)
(* held_count >= W and increments on success; rn_held_erase decrements    *)
(* once, after the final hole clears, under held_count > 0.  count is that *)
(* counter, and Insert is refused at count >= MaxLive (MaxLive is W).  The *)
(* other indexes here keep no such counter (their bound is their table's   *)
(* capacity); for them count is the model's own tally, checked the same   *)
(* way.  It falls when a cell is cleared: at EraseStart for the           *)
(* clear-first erase, at the final hole for the clear-last one, so         *)
(* CountExact (count is the number of used cells) holds in every state,    *)
(* mid-erase included,                                                     *)
(* and LiveBounded (at most MaxLive live keys) follows from the counted    *)
(* refusal.  MCRNOpenAddressHeldCount.cfg is the held index's own shape:  *)
(* W = 2 in 2W = 4 cells, clear-last.  CountFault names a fault in the     *)
(* counter, for the reversed witnesses: "insert_keeps_count" (the          *)
(* increment deleted, mutant 3085 of the 2026-11-11 hunt) and              *)
(* "erase_floor_one" (the guard held_count > 0 read as > 1, mutant 3119);  *)
(* both fail CountExact.  "none" is the code.                              *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS Keys, N, NoWrapTest, ClearLast, MaxLive, CountFault

EMPTY == 0

VARIABLES
    home,       \* each key's home cell (the hash), fixed for the run
    cell,       \* the index: 0..N-1 -> key or EMPTY
    live,       \* the keys stored
    pc,         \* "idle" or "erase"
    hole,       \* erase: the cell being refilled
    scan,       \* erase: the cell examined last
    scanned,    \* erase: cells the scan has examined
    count       \* the index's separate counter (the held index's held_count)

vars == <<home, cell, live, pc, hole, scan, scanned, count>>

\* the counter on a successful insert, and at the one clear that ends an erase
CountUp == IF CountFault = "insert_keeps_count" THEN count ELSE count + 1
CountDown == IF count > (IF CountFault = "erase_floor_one" THEN 1 ELSE 0) THEN count - 1 ELSE count

Next1(x) == (x + 1) % N

\* home lies cyclically in (lo, hi]
InCyclic(h, lo, hi) ==
    IF lo <= hi THEN lo < h /\ h <= hi
    ELSE IF NoWrapTest THEN lo < h /\ h <= hi
    ELSE lo < h \/ h <= hi

\* the cells a probe for k visits before it stops at an empty cell
RECURSIVE ProbeFinds(_, _, _)
ProbeFinds(k, at, steps) ==
    IF steps = N THEN FALSE
    ELSE IF cell[at] = k THEN TRUE
    ELSE IF cell[at] = EMPTY THEN FALSE
    ELSE ProbeFinds(k, Next1(at), steps + 1)

RECURSIVE FirstEmpty(_, _)
FirstEmpty(at, steps) == IF cell[at] = EMPTY \/ steps = N THEN at ELSE FirstEmpty(Next1(at), steps + 1)

RECURSIVE CellOf(_, _, _)
CellOf(k, at, steps) == IF steps = N \/ cell[at] = k THEN at ELSE CellOf(k, Next1(at), steps + 1)

\* rn_sent_index_insert's probe: the first cell empty or holding k
RECURSIVE EmptyOrKey(_, _, _)
EmptyOrKey(k, at, steps) ==
    IF cell[at] = EMPTY \/ cell[at] = k \/ steps = N THEN at ELSE EmptyOrKey(k, Next1(at), steps + 1)

Init ==
    /\ home \in [Keys -> 0..(N - 1)]
    /\ cell = [ c \in 0..(N - 1) |-> EMPTY ]
    /\ live = {}
    /\ pc = "idle"
    /\ hole = 0
    /\ scan = 0
    /\ scanned = 0
    /\ count = 0

\* refused at count >= MaxLive: the code's bound reads the counter
\* (rn_held_insert: held_count >= W), never the cells.  A live key is
\* written at the first cell empty or holding it (rn_sent_index_insert's
\* overwrite in place): its own cell while Findable holds, so the cells do
\* not change; an empty cell met first would hold it twice (Unique).
Insert(k) ==
    /\ pc = "idle"
    /\ IF k \notin live
       THEN /\ count < MaxLive
            /\ cell' = [ cell EXCEPT ![FirstEmpty(home[k], 0)] = k ]
            /\ live' = live \cup {k}
            /\ count' = CountUp
       ELSE LET at == EmptyOrKey(k, home[k], 0)
            IN /\ cell' = [ cell EXCEPT ![at] = k ]
               /\ count' = IF cell[at] = EMPTY THEN CountUp ELSE count
               /\ UNCHANGED live
    /\ UNCHANGED <<home, pc, hole, scan, scanned>>

\* the erase finds the key's cell, clearing it first unless ClearLast; the
\* scan starts after it
EraseStart(k) ==
    /\ pc = "idle"
    /\ k \in live
    /\ LET at == CellOf(k, home[k], 0)
       IN /\ cell' = IF ClearLast THEN cell ELSE [ cell EXCEPT ![at] = EMPTY ]
          /\ hole' = at
          /\ scan' = at
          /\ scanned' = 0
    /\ count' = IF ClearLast THEN count ELSE CountDown
    /\ live' = live \ {k}
    /\ pc' = "erase"
    /\ UNCHANGED home

\* one step of the backward shift: an empty cell ends it (ClearLast clears
\* the final hole then); an entry whose home lies cyclically in (hole, scan]
\* stays; any other moves back into the hole (ClearLast leaves its copy
\* behind, to be overwritten or cleared as the final hole)
EraseStep ==
    /\ pc = "erase"
    /\ IF ClearLast THEN scanned <= N ELSE scanned < N
    /\ LET s == Next1(scan)
           c == cell[s]
       IN IF c = EMPTY
          THEN /\ pc' = "idle"
               /\ scan' = s
               /\ cell' = IF ClearLast THEN [ cell EXCEPT ![hole] = EMPTY ] ELSE cell
               /\ count' = IF ClearLast THEN CountDown ELSE count
               /\ UNCHANGED hole
          ELSE IF InCyclic(home[c], hole, s)
          THEN /\ scan' = s
               /\ UNCHANGED <<cell, hole, pc, count>>
          ELSE /\ cell' = IF ClearLast THEN [ cell EXCEPT ![hole] = c ]
                           ELSE [ cell EXCEPT ![hole] = c, ![s] = EMPTY ]
               /\ hole' = s
               /\ scan' = s
               /\ UNCHANGED <<pc, count>>
    /\ scanned' = scanned + 1
    /\ UNCHANGED <<home, live>>

\* the clear-first scan examines at most N cells (the code's step < capacity
\* bound) and ends there whatever it found
EraseBound ==
    /\ pc = "erase"
    /\ ~ClearLast
    /\ scanned = N
    /\ pc' = "idle"
    /\ UNCHANGED <<home, cell, live, hole, scan, scanned, count>>

Next ==
    \/ \E k \in Keys : Insert(k) \/ EraseStart(k)
    \/ EraseStep
    \/ EraseBound

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ cell \in [0..(N - 1) -> Keys \cup {EMPTY}]
    /\ live \subseteq Keys
    /\ pc \in {"idle", "erase"}
    /\ count \in Nat
    /\ CountFault \in {"none", "insert_keeps_count", "erase_floor_one"}

\* the counter is the number of used cells, in every state, mid-erase included
\* (rn_held_insert's increment, rn_held_erase's one decrement after the final
\* hole clears)
CountExact == count = Cardinality({ c \in 0..(N - 1) : cell[c] # EMPTY })

\* the refusal at count >= MaxLive keeps the live keys within the bound the
\* cells are sized for (RNHeldOwnership HeldBound, here on the counter)
LiveBounded == Cardinality(live) <= MaxLive

\* the scan meets an empty cell within one pass of the array
EraseEnds == scanned <= N

\* outside an erase, every live key is found from its home
Findable == pc = "idle" => \A k \in live : ProbeFinds(k, home[k], 0)

\* the cells hold exactly the live keys, each once
Unique ==
    pc = "idle" =>
        /\ \A k \in live : Cardinality({ c \in 0..(N - 1) : cell[c] = k }) = 1
        /\ \A c \in 0..(N - 1) : cell[c] = EMPTY \/ cell[c] \in live
=============================================================================
