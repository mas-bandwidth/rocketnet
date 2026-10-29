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
(* Two erase shapes are in the code.  ClearLast FALSE clears the hole      *)
(* first (the encryption map, the token index); the scan examines at most *)
(* N cells (EraseBound, the code's step < capacity), and Findable holds    *)
(* with a full index (MaxLive = N, the encryption map) when it stops    *)
(* there.  ClearLast TRUE leaves the hole marked and copies each           *)
(* moved entry back, clearing only the final hole (client slots, the held  *)
(* index, the sent index): the scan ends only on a cell empty before the   *)
(* erase, which needs MaxLive < N; the code sizes those indexes at twice   *)
(* their capacity.  scanned counts the scan; EraseEnds says it never     *)
(* passes N.                                                               *)
(*                                                                         *)
(* Findable says every live key is reached by probing from its home        *)
(* before an empty cell; Unique says no key sits in two cells.             *)
(*                                                                         *)
(* Reversed witnesses: NoWrapTest TRUE judges "home lies in (hole, scan]"  *)
(* without the wrapped case, so a cluster crossing the array end moves an  *)
(* entry in front of its home, where no probe finds it (Findable).         *)
(* ClearLast TRUE with MaxLive = N is a clear-last index allowed to fill:  *)
(* its scan circles with no empty cell to stop it (EraseEnds).             *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS Keys, N, NoWrapTest, ClearLast, MaxLive

EMPTY == 0

VARIABLES
    home,       \* each key's home cell (the hash), fixed for the run
    cell,       \* the index: 0..N-1 -> key or EMPTY
    live,       \* the keys stored
    pc,         \* "idle" or "erase"
    hole,       \* erase: the cell being refilled
    scan,       \* erase: the cell examined last
    scanned     \* erase: cells the scan has examined

vars == <<home, cell, live, pc, hole, scan, scanned>>

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

Init ==
    /\ home \in [Keys -> 0..(N - 1)]
    /\ cell = [ c \in 0..(N - 1) |-> EMPTY ]
    /\ live = {}
    /\ pc = "idle"
    /\ hole = 0
    /\ scan = 0
    /\ scanned = 0

Insert(k) ==
    /\ pc = "idle"
    /\ k \notin live
    /\ Cardinality(live) < MaxLive
    /\ cell' = [ cell EXCEPT ![FirstEmpty(home[k], 0)] = k ]
    /\ live' = live \cup {k}
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
               /\ UNCHANGED hole
          ELSE IF InCyclic(home[c], hole, s)
          THEN /\ scan' = s
               /\ UNCHANGED <<cell, hole, pc>>
          ELSE /\ cell' = IF ClearLast THEN [ cell EXCEPT ![hole] = c ]
                           ELSE [ cell EXCEPT ![hole] = c, ![s] = EMPTY ]
               /\ hole' = s
               /\ scan' = s
               /\ UNCHANGED pc
    /\ scanned' = scanned + 1
    /\ UNCHANGED <<home, live>>

\* the clear-first scan examines at most N cells (the code's step < capacity
\* bound) and ends there whatever it found
EraseBound ==
    /\ pc = "erase"
    /\ ~ClearLast
    /\ scanned = N
    /\ pc' = "idle"
    /\ UNCHANGED <<home, cell, live, hole, scan, scanned>>

Next ==
    \/ \E k \in Keys : Insert(k) \/ EraseStart(k)
    \/ EraseStep
    \/ EraseBound

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ cell \in [0..(N - 1) -> Keys \cup {EMPTY}]
    /\ live \subseteq Keys
    /\ pc \in {"idle", "erase"}

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
