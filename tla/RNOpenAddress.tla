---------------------------- MODULE RNOpenAddress ----------------------------
(***************************************************************************)
(* The open-addressed index as the code lays it out: an array of N cells, *)
(* linear probing from a key's home with wraparound, and deletion by       *)
(* backward shift, one cell per step (sdk/rocketnet_encryption_map.cpp    *)
(* rocketnet_encryption_map_erase_index, sdk/rocketnet_token_tables.cpp,   *)
(* sdk/rocketnet_client_slots.cpp rocketnet_client_slots_erase_index,      *)
(* sdk/rocketnet_channel.cpp rn_held_erase, sdk/rocketnet_window_ack.cpp   *)
(* rn_sent_index).  The keys are the network's: a peer chooses addresses, *)
(* tokens and (channel, ordering), so it chooses the clusters.             *)
(*                                                                         *)
(* home is chosen once, at Init, from every map of keys to cells: TLC      *)
(* checks every hash function at once, clusters that wrap the array end    *)
(* included.  At most N - 1 keys are live (the code sizes every index      *)
(* above its capacity), so a probe and the erase scan always meet an       *)
(* empty cell.                                                             *)
(*                                                                         *)
(* Findable says every live key is reached by probing from its home        *)
(* before an empty cell; Unique says no key sits in two cells.             *)
(*                                                                         *)
(* Reversed witness: NoWrapTest TRUE judges "home lies in (hole, scan]"    *)
(* without the wrapped case, so a cluster crossing the array end moves an  *)
(* entry in front of its home, where no probe finds it (Findable).         *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS Keys, N, NoWrapTest

EMPTY == 0

VARIABLES
    home,       \* each key's home cell (the hash), fixed for the run
    cell,       \* the index: 0..N-1 -> key or EMPTY
    live,       \* the keys stored
    pc,         \* "idle" or "erase"
    hole,       \* erase: the cell being refilled
    scan        \* erase: the cell examined last

vars == <<home, cell, live, pc, hole, scan>>

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

Insert(k) ==
    /\ pc = "idle"
    /\ k \notin live
    /\ Cardinality(live) < N - 1
    /\ cell' = [ cell EXCEPT ![FirstEmpty(home[k], 0)] = k ]
    /\ live' = live \cup {k}
    /\ UNCHANGED <<home, pc, hole, scan>>

\* the erase clears the key's cell; the scan starts after it
EraseStart(k) ==
    /\ pc = "idle"
    /\ k \in live
    /\ LET at == CellOf(k, home[k], 0)
       IN /\ cell' = [ cell EXCEPT ![at] = EMPTY ]
          /\ hole' = at
          /\ scan' = at
    /\ live' = live \ {k}
    /\ pc' = "erase"
    /\ UNCHANGED home

\* one step of the backward shift: an empty cell ends it; an entry whose home
\* lies cyclically in (hole, scan] stays; any other moves back into the hole
EraseStep ==
    /\ pc = "erase"
    /\ LET s == Next1(scan)
           c == cell[s]
       IN IF c = EMPTY
          THEN /\ pc' = "idle"
               /\ scan' = s
               /\ UNCHANGED <<cell, hole>>
          ELSE IF InCyclic(home[c], hole, s)
          THEN /\ scan' = s
               /\ UNCHANGED <<cell, hole, pc>>
          ELSE /\ cell' = [ cell EXCEPT ![hole] = c, ![s] = EMPTY ]
               /\ hole' = s
               /\ scan' = s
               /\ UNCHANGED pc
    /\ UNCHANGED <<home, live>>

Next ==
    \/ \E k \in Keys : Insert(k) \/ EraseStart(k)
    \/ EraseStep

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ cell \in [0..(N - 1) -> Keys \cup {EMPTY}]
    /\ live \subseteq Keys
    /\ pc \in {"idle", "erase"}

\* outside an erase, every live key is found from its home
Findable == pc = "idle" => \A k \in live : ProbeFinds(k, home[k], 0)

\* the cells hold exactly the live keys, each once
Unique ==
    pc = "idle" =>
        /\ \A k \in live : Cardinality({ c \in 0..(N - 1) : cell[c] = k }) = 1
        /\ \A c \in 0..(N - 1) : cell[c] = EMPTY \/ cell[c] \in live
=============================================================================
