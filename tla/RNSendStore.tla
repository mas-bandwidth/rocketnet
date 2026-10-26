---------------------------- MODULE RNSendStore ----------------------------
(***************************************************************************)
(* One class of the message layer's send store (docs/CHANNELS.md 4 and     *)
(* 15; sdk/rocketnet_allocator.cpp, rn_send_store_alloc and                *)
(* rn_send_store_free_checked).  The three classes share no state, so one  *)
(* class is the whole model.                                               *)
(*                                                                         *)
(* The free list is a stack threaded through the chunks' links: alloc pops *)
(* from the head, free pushes each chunk of a chain back onto the head.  A *)
(* send holds a chain of 1 to MaxBlock chunks (a block's fragments), taken *)
(* all or none.  An allocated chunk's link carries IN_USE, and a chain's   *)
(* first chunk HEAD; free refuses a handle whose head is not both.         *)
(*                                                                         *)
(* Senders are the region's send descriptors.  A sender may free its chain *)
(* twice (a caller bug); the guard is what turns the second free into a    *)
(* refusal.  The second free is modelled only while none of the chain's    *)
(* chunks has been reallocated: a free through a handle whose chunk now    *)
(* heads someone else's chain is indistinguishable from that owner's own   *)
(* free, and no allocator-local check can see it.  The layer above owns    *)
(* that (a descriptor's handle is dropped at its free, layer 1; the        *)
(* per-number table never names a freed descriptor, layer 3).              *)
(*                                                                         *)
(* GuardDoubleFree = FALSE removes the guard; AllOrNone = FALSE lets a     *)
(* block too large for the free chunks take what there is and then report *)
(* the refusal.  Each must fail; those are the reversed witnesses.         *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS Chunks,           \* the class's chunk count, N
          Senders,          \* the descriptors that may hold a chain
          MaxBlock,         \* the most chunks one send takes
          GuardDoubleFree,  \* free_checked's IN_USE | HEAD test
          AllOrNone         \* alloc's free_count >= needed test

ASSUME Chunks \in Nat \ {0} /\ MaxBlock \in 1..Chunks /\ Senders # {}
ASSUME GuardDoubleFree \in BOOLEAN /\ AllOrNone \in BOOLEAN

ChunkIds == 1..Chunks

VARIABLES free,     \* the free chain, head first: a sequence of chunk ids
          held,     \* held[s]: the chain s holds, first chunk first; <<>> when none
          stale,    \* stale[s]: the chain s last freed, kept for a second free
          gen,      \* gen[i]: chunk i's generation tag (1 once a take has tagged it)
          refusal   \* the last refusal: [want |-> k, had |-> Len(free) then]; want = 0 before any

vars == <<free, held, stale, gen, refusal>>

Range(seq) == { seq[i] : i \in DOMAIN seq }

Reverse(seq) == [ i \in 1..Len(seq) |-> seq[Len(seq) - i + 1] ]

InUse == UNION { Range(held[s]) : s \in Senders }

Init ==
    /\ free = [ i \in 1..Chunks |-> i ]
    /\ held = [ s \in Senders |-> <<>> ]
    /\ stale = [ s \in Senders |-> <<>> ]
    /\ gen = [ i \in ChunkIds |-> 0 ]
    /\ refusal = [ want |-> 0, had |-> 0 ]

\* rn_send_store_alloc: pop k chunks, chained, or refuse whole
Alloc(s, k) ==
    /\ held[s] = <<>>
    /\ IF Len(free) >= k
       THEN /\ held' = [ held EXCEPT ![s] = SubSeq(free, 1, k) ]
            /\ free' = SubSeq(free, k + 1, Len(free))
            /\ stale' = [ stale EXCEPT ![s] = <<>> ]
            /\ gen' = [ i \in ChunkIds |-> IF i \in Range(SubSeq(free, 1, k)) THEN 1 ELSE gen[i] ]
            /\ UNCHANGED refusal
       ELSE /\ refusal' = [ want |-> k, had |-> Len(free) ]
            /\ IF AllOrNone
               THEN UNCHANGED <<free, held, stale, gen>>
               \* the reversed rule: take what is there, then report the refusal;
               \* the chunks taken belong to no one
               ELSE /\ free' = <<>>
                    /\ UNCHANGED <<held, stale, gen>>

\* rn_send_store_free_checked, through the live handle: push the chain back
Free(s) ==
    /\ held[s] # <<>>
    /\ free' = Reverse(held[s]) \o free
    /\ held' = [ held EXCEPT ![s] = <<>> ]
    /\ stale' = [ stale EXCEPT ![s] = held[s] ]
    /\ UNCHANGED <<gen, refusal>>

\* the same handle freed again, before any of its chunks is reallocated
FreeAgain(s) ==
    /\ held[s] = <<>>
    /\ stale[s] # <<>>
    /\ Range(stale[s]) \cap InUse = {}
    /\ IF GuardDoubleFree
       THEN UNCHANGED <<free, held>>            \* the head's IN_USE is clear: refused, nothing changes
       ELSE /\ free' = Reverse(stale[s]) \o free
            /\ UNCHANGED held
    /\ stale' = [ stale EXCEPT ![s] = <<>> ]
    /\ UNCHANGED <<gen, refusal>>

Next ==
    \/ \E s \in Senders, k \in 1..MaxBlock : Alloc(s, k)
    \/ \E s \in Senders : Free(s)
    \/ \E s \in Senders : FreeAgain(s)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ free \in Seq(ChunkIds)
    /\ held \in [ Senders -> Seq(ChunkIds) ]
    /\ stale \in [ Senders -> Seq(ChunkIds) ]
    /\ gen \in [ ChunkIds -> 0..1 ]  \* the tag's presence (the code's 10-bit tag, bounded for the model)
    /\ refusal \in [ want : 0..MaxBlock, had : 0..Chunks ]

\* no chunk is in two places: never twice on the free list, never both free
\* and held, never in two chains, never twice in one chain
NoDoubleAllocate ==
    /\ Len(free) = Cardinality(Range(free))
    /\ Range(free) \cap InUse = {}
    /\ \A s \in Senders : Len(held[s]) = Cardinality(Range(held[s]))
    /\ \A s, t \in Senders : s # t => Range(held[s]) \cap Range(held[t]) = {}

\* free + used = total: no chunk is lost
NoLeak ==
    Len(free) + Cardinality(InUse) = Chunks

\* a refusal only when the free chunks could not hold the send whole
RefuseOnlyWhenFull ==
    refusal.want > 0 => refusal.had < refusal.want

\* a held chain is never longer than a send may take
ChainsBounded ==
    \A s \in Senders : Len(held[s]) <= MaxBlock


\* a held chunk carries a generation its take bumped: never zero
HeldGenerationPositive ==
    \A s \in Senders : \A i \in Range(held[s]) : gen[i] > 0

=============================================================================
