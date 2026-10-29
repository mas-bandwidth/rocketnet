---------------------------- MODULE RNIndexRing ----------------------------
(***************************************************************************)
(* The insertion-order ring of slot indices with tombstones: the FIFO the  *)
(* claims table keeps (sdk/rocketnet_token_tables.cpp, rn_token_order_push *)
(* and rn_token_order_oldest), and the shape the message layer's queues    *)
(* move to.  Each slot carries the ticket of its latest insertion (0 when *)
(* not live); each ring entry names a slot and the ticket it was pushed    *)
(* with.  An entry whose ticket no longer matches its slot's is a          *)
(* tombstone: a release zeroes the slot's ticket and a re-insertion gives  *)
(* it a new one, so neither touches the ring.  The head skips tombstones   *)
(* (PopStale), and a push that finds the ring full first drops every       *)
(* tombstone in one pass.                                                  *)
(*                                                                         *)
(* order is the abstract FIFO the ring implements: the live slots in      *)
(* insertion order.  LiveIsOrder says the ring's live entries, in order,  *)
(* are exactly that FIFO; HeadIsOldest says a live head is its oldest;    *)
(* Bounded says the ring never overflows.                                  *)
(*                                                                         *)
(* Reversed witnesses: StaleBySlot TRUE judges an entry by its slot being  *)
(* live, not by its ticket, so an entry left by a re-insertion still looks *)
(* live (LiveIsOrder).  RingCap = Cardinality(Slots) leaves no room after  *)
(* a compaction of an all-live ring (Bounded).                             *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS Slots, MaxTicket, RingCap, StaleBySlot

VARIABLES ticket, next, ring, order

vars == <<ticket, next, ring, order>>

Live(s) == ticket[s] # 0

\* an entry is current when its ticket is its slot's (the code); the broken
\* witness asks only whether the slot is live
Current(e) == IF StaleBySlot THEN Live(e[1]) ELSE ticket[e[1]] = e[2]

LiveEntries(r) == SelectSeq( r, Current )

Slotsof(r) == [ i \in 1..Len(r) |-> r[i][1] ]

Remove(seq, s) == SelectSeq( seq, LAMBDA x : x # s )

\* the push: compact a full ring, then append
Pushed(r, s, t) == Append( IF Len(r) = RingCap THEN LiveEntries(r) ELSE r, <<s, t>> )

Init ==
    /\ ticket = [ s \in Slots |-> 0 ]
    /\ next = 1
    /\ ring = << >>
    /\ order = << >>

\* a free slot is claimed: the newest entry
Insert(s) ==
    /\ ~Live(s)
    /\ next <= MaxTicket
    /\ ticket' = [ ticket EXCEPT ![s] = next ]
    /\ next' = next + 1
    /\ ring' = Pushed( ring, s, next )
    /\ order' = Append( order, s )

\* a live slot is claimed again (an expired claim of the same token, or the
\* oldest taken for a new one): it becomes the newest, its old entry a tombstone
Reinsert(s) ==
    /\ Live(s)
    /\ next <= MaxTicket
    /\ ticket' = [ ticket EXCEPT ![s] = next ]
    /\ next' = next + 1
    /\ ring' = Pushed( ring, s, next )
    /\ order' = Append( Remove( order, s ), s )

\* a release from anywhere in the order: the ring is not touched
Release(s) ==
    /\ Live(s)
    /\ ticket' = [ ticket EXCEPT ![s] = 0 ]
    /\ order' = Remove( order, s )
    /\ UNCHANGED <<next, ring>>

\* the head skips a tombstone
PopStale ==
    /\ Len(ring) > 0
    /\ ~Current( Head(ring) )
    /\ ring' = Tail(ring)
    /\ UNCHANGED <<ticket, next, order>>

Next ==
    \/ \E s \in Slots : Insert(s) \/ Reinsert(s) \/ Release(s)
    \/ PopStale

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ ticket \in [ Slots -> 0..MaxTicket ]
    /\ next \in 1..(MaxTicket + 1)
    /\ \A i \in 1..Len(ring) : ring[i] \in Slots \X (1..MaxTicket)
    /\ \A i \in 1..Len(order) : order[i] \in Slots

LiveIsOrder == Slotsof( LiveEntries(ring) ) = order

HeadIsOldest == ( Len(ring) > 0 /\ Current( Head(ring) ) ) => Head(ring)[1] = Head(order)

Bounded == Len(ring) <= RingCap

=============================================================================
