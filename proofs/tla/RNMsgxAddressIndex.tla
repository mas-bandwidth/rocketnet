--------------------------- MODULE RNMsgxAddressIndex ---------------------------
(***************************************************************************)
(* The address index of sdk/rocketnet_msgx_socket_table.h at its layout:   *)
(* open addressing over IS entries (a power of two), linear probing from   *)
(* a key's home, -1 an empty entry, and the backward-shift delete of        *)
(* rocketnet_msgx_socket_table_index_remove (no tombstones: a lookup ends   *)
(* at the first empty entry, so a delete must move every later entry of    *)
(* the run whose home is not cyclically inside (hole, entry] back into the  *)
(* hole).  Every assignment of homes to the keys is checked: the homes are  *)
(* chosen in Init and never change, so collisions, runs that wrap past the  *)
(* last entry and runs that hold several homes are all in one instance.     *)
(*                                                                         *)
(* Insert is rocketnet_msgx_socket_table_index_insert, Remove is           *)
(* rocketnet_msgx_socket_table_index_remove, Lookup is                     *)
(* rocketnet_msgx_socket_table_lookup.  DeleteShift FALSE is the broken    *)
(* delete that only clears the entry (MCRNMsgxAddressIndexBroken           *)
(* DeleteWithoutShift.cfg) and must fail LookupFindsOpen.                  *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    IS,             \* index entries
    Keys,           \* the keys (slots of open addresses), at most IS - 1 of them
    DeleteShift     \* TRUE: the code's backward-shift delete

Entries == 0..(IS - 1)

VARIABLES
    ix,             \* ix[entry]: a key, or -1
    open,           \* the keys inserted and not removed
    home            \* home[key]: the entry its probe starts at

vars == <<ix, open, home>>

RECURSIVE LookupFrom(_, _, _)
LookupFrom(k, e, n) ==
    IF n = IS \/ ix[e] < 0 THEN -1
    ELSE IF ix[e] = k THEN e
    ELSE LookupFrom(k, (e + 1) % IS, n + 1)

\* the entry holding k, or -1
Lookup(k) == LookupFrom(k, home[k], 0)

RECURSIVE EmptyFrom(_)
EmptyFrom(e) == IF ix[e] < 0 THEN e ELSE EmptyFrom((e + 1) % IS)

RECURSIVE SlotFrom(_, _)
SlotFrom(k, e) == IF ix[e] = k THEN e ELSE SlotFrom(k, (e + 1) % IS)

\* the shift loop of the code: hole is the entry to fill, entry the last
\* examined; an entry moves into the hole when its home is not cyclically
\* inside (hole, entry]
RECURSIVE Shift(_, _, _)
Shift(x, hole, entry) ==
    LET next == (entry + 1) % IS
        moving == x[next]
    IN IF moving < 0 THEN [x EXCEPT ![hole] = -1]
       ELSE LET h == home[moving]
                inRange == IF hole <= next THEN h > hole /\ h <= next
                                           ELSE h > hole \/ h <= next
            IN IF inRange THEN Shift(x, hole, next)
               ELSE Shift([x EXCEPT ![hole] = moving], next, next)

Init ==
    /\ home \in [Keys -> Entries]
    /\ ix = [e \in Entries |-> -1]
    /\ open = {}

Insert(k) ==
    /\ k \notin open
    /\ ix' = [ix EXCEPT ![EmptyFrom(home[k])] = k]
    /\ open' = open \cup {k}
    /\ UNCHANGED home

Remove(k) ==
    /\ k \in open
    /\ LET hole == SlotFrom(k, home[k]) IN
       ix' = IF DeleteShift THEN Shift(ix, hole, hole) ELSE [ix EXCEPT ![hole] = -1]
    /\ open' = open \ {k}
    /\ UNCHANGED home

Next == \E k \in Keys : Insert(k) \/ Remove(k)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ ix \in [Entries -> {-1} \cup Keys]
    /\ open \subseteq Keys
    /\ home \in [Keys -> Entries]

\* every open key is found by the probe from its home, and no other is
LookupFindsOpen ==
    \A k \in Keys : (Lookup(k) >= 0) = (k \in open)

\* the entries hold exactly the open keys, each once: no entry is lost or
\* duplicated
NoEntryLost ==
    /\ {ix[e] : e \in {x \in Entries : ix[x] >= 0}} = open
    /\ Cardinality({x \in Entries : ix[x] >= 0}) = Cardinality(open)

=============================================================================
