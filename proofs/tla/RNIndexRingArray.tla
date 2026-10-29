-------------------------- MODULE RNIndexRingArray --------------------------
(***************************************************************************)
(* The insertion-order ring of RNIndexRing as the code lays it out: a      *)
(* fixed array of N entries, a head index and a count, positions taken     *)
(* modulo N (sdk/rocketnet_token_tables.cpp rn_token_order_push and        *)
(* rn_token_order_oldest; sdk/rocketnet_endpoint.cpp rn_unreliable_push    *)
(* and rn_unreliable_sync).  RNIndexRing checks the FIFO as a sequence;    *)
(* this module checks the mapping from that sequence to the array, which  *)
(* the sequence model cannot see.                                          *)
(*                                                                         *)
(* A push into a full ring compacts it first, in place, as the code's     *)
(* loop does: one read of logical entry i at physical (head + i) % N per  *)
(* step, and, when the entry is current, one write of it at its           *)
(* destination.  Nothing else runs while the loop does (one thread owns   *)
(* the ring), so the steps are an atomic pass broken into its reads and   *)
(* writes, which is where an overwrite can hide.                          *)
(*                                                                         *)
(* order is the abstract FIFO: the live slots in insertion order.         *)
(* LiveIsOrder says that, outside a compaction, the ring's current         *)
(* entries read from head in order are exactly that FIFO.                  *)
(*                                                                         *)
(* Reversed witness: CompactToZero TRUE writes survivor k at physical k   *)
(* and resets head to 0, the code before the fix (PR #616).  On a wrapped *)
(* ring that overwrites unread entries, losing a live one (LiveIsOrder).  *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS Slots, N, MaxTicket, CompactToZero

NONE == [slot |-> 0, t |-> 0]

VARIABLES
    ticket,     \* each slot's latest insertion ticket, 0 when not live
    next,       \* the next ticket
    arr,        \* the ring: 0..N-1 -> entry
    head,       \* physical index of logical entry 0
    count,      \* entries in the ring
    order,      \* ghost: the live slots in insertion order
    pc,         \* "idle" or "compact"
    i,          \* compaction: the logical entry read next
    kept,       \* compaction: survivors written so far
    pending     \* compaction: the entry to push once the pass ends

vars == <<ticket, next, arr, head, count, order, pc, i, kept, pending>>

Live(s) == ticket[s] # 0
Current(e) == e.slot # 0 /\ ticket[e.slot] = e.t
Phys(k) == (head + k) % N
Logical == [ k \in 1..count |-> arr[Phys(k - 1)] ]
Remove(seq, s) == SelectSeq( seq, LAMBDA x : x # s )
SlotsOf(seq) == [ k \in 1..Len(seq) |-> seq[k].slot ]

Init ==
    /\ ticket = [ s \in Slots |-> 0 ]
    /\ next = 1
    /\ arr = [ k \in 0..(N - 1) |-> NONE ]
    /\ head = 0
    /\ count = 0
    /\ order = << >>
    /\ pc = "idle"
    /\ i = 0
    /\ kept = 0
    /\ pending = NONE

\* append at the tail, or start the in-place compaction when full
Push(e) ==
    IF count < N
    THEN /\ arr' = [ arr EXCEPT ![Phys(count)] = e ]
         /\ count' = count + 1
         /\ UNCHANGED <<head, pc, i, kept, pending>>
    ELSE /\ pc' = "compact"
         /\ i' = 0
         /\ kept' = 0
         /\ pending' = e
         /\ UNCHANGED <<arr, head, count>>

\* a free slot is claimed: the newest entry
Insert(s) ==
    /\ pc = "idle"
    /\ ~Live(s)
    /\ next <= MaxTicket
    /\ ticket' = [ ticket EXCEPT ![s] = next ]
    /\ next' = next + 1
    /\ order' = Append( order, s )
    /\ Push( [slot |-> s, t |-> next] )

\* a live slot is claimed again: it becomes the newest, its old entry stale
Reinsert(s) ==
    /\ pc = "idle"
    /\ Live(s)
    /\ next <= MaxTicket
    /\ ticket' = [ ticket EXCEPT ![s] = next ]
    /\ next' = next + 1
    /\ order' = Append( Remove( order, s ), s )
    /\ Push( [slot |-> s, t |-> next] )

\* a release from anywhere: the ring is not touched
Release(s) ==
    /\ pc = "idle"
    /\ Live(s)
    /\ ticket' = [ ticket EXCEPT ![s] = 0 ]
    /\ order' = Remove( order, s )
    /\ UNCHANGED <<next, arr, head, count, pc, i, kept, pending>>

\* the head skips a stale entry
PopStale ==
    /\ pc = "idle"
    /\ count > 0
    /\ ~Current( arr[head] )
    /\ head' = (head + 1) % N
    /\ count' = count - 1
    /\ UNCHANGED <<ticket, next, arr, order, pc, i, kept, pending>>

\* one step of the compaction loop: read logical entry i, write it to its
\* destination when it is current
CompactStep ==
    /\ pc = "compact"
    /\ i < count
    /\ LET e == arr[Phys(i)]
           dest == IF CompactToZero THEN kept ELSE Phys(kept)
       IN IF Current(e)
          THEN /\ arr' = [ arr EXCEPT ![dest] = e ]
               /\ kept' = kept + 1
          ELSE UNCHANGED <<arr, kept>>
    /\ i' = i + 1
    /\ UNCHANGED <<ticket, next, head, count, order, pc, pending>>

\* the pass ends: the survivors are the ring, then the pending entry appends
CompactDone ==
    /\ pc = "compact"
    /\ i = count
    /\ LET h == IF CompactToZero THEN 0 ELSE head
       IN /\ arr' = [ arr EXCEPT ![(h + kept) % N] = pending ]
          /\ head' = h
    /\ count' = kept + 1
    /\ pc' = "idle"
    /\ pending' = NONE
    /\ UNCHANGED <<ticket, next, order, i, kept>>

Next ==
    \/ \E s \in Slots : Insert(s) \/ Reinsert(s) \/ Release(s)
    \/ PopStale
    \/ CompactStep
    \/ CompactDone

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ ticket \in [ Slots -> 0..MaxTicket ]
    /\ next \in 1..(MaxTicket + 1)
    /\ head \in 0..(N - 1)
    /\ count \in 0..N
    /\ pc \in {"idle", "compact"}

\* outside a compaction, the current entries from head in order are the FIFO
LiveIsOrder == pc = "idle" => SlotsOf( SelectSeq( Logical, Current ) ) = order

\* a full ring always has room once compacted: at most Cardinality(Slots)
\* entries are live, so the pass frees one
CompactionFrees == pc = "compact" /\ i = count => kept < N
=============================================================================
