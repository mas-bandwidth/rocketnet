---------------------------- MODULE RNSendStore ----------------------------
(***************************************************************************)
(* One class of the message layer's send store (docs/CHANNELS.md 4 and     *)
(* 15; sdk/rocketnet_allocator.cpp, rn_send_store_alloc and                *)
(* rn_send_store_free_checked).  The three classes share no state, so one  *)
(* class is the whole model.                                               *)
(*                                                                         *)
(* No chunk stores a reference to another. The free chunks are those whose *)
(* meta live bit is clear (plus the never-taken chunks at or past the      *)
(* lazy watermark, modelled in RNLazyPool); alloc scans the meta array     *)
(* from the class's scan cursor for one contiguous run of free chunks and  *)
(* takes that run, all or none, and a block's later chunks are addressed   *)
(* by arithmetic on its head (head plus offset). A block's chunks share    *)
(* the take's generation tag; a single chunk's tag bumps on its take.      *)
(* Freeing a block returns the live chunks of its run that still carry     *)
(* its tag (a chunk a fragment free already returned, or one since         *)
(* retaken under a new tag, is not its to free). A send holds a block of   *)
(* 1 to MaxBlock chunks, taken all or none. An allocated chunk's meta      *)
(* carries LIVE, and a block's first chunk HEAD; free refuses a handle     *)
(* whose head is not both.                                                 *)
(*                                                                         *)
(* Senders are the region's send descriptors.  A sender may free its block *)
(* twice (a caller bug); the guard is what turns the second free into a    *)
(* refusal.  The second free is modelled only while none of the block's    *)
(* chunks has been reallocated: a free through a handle whose chunk now    *)
(* heads someone else's block is indistinguishable from that owner's own   *)
(* free, and no allocator-local check can see it.  The layer above owns    *)
(* that (a descriptor's handle is dropped at its free, layer 1; the        *)
(* per-number table never names a freed descriptor, layer 3).              *)
(*                                                                         *)
(* GuardDoubleFree = FALSE removes the guard; AllOrNone = FALSE lets a     *)
(* block with no contiguous run take what there is and then report the     *)
(* refusal.  Each must fail; those are the reversed witnesses.             *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS Chunks,           \* the class's chunk count, N
          Senders,          \* the descriptors that may hold a block
          MaxBlock,         \* the most chunks one send takes
          GuardDoubleFree,  \* free_checked's LIVE | HEAD test
          AllOrNone         \* alloc's contiguous-run test

ASSUME Chunks \in Nat \ {0} /\ MaxBlock \in 1..Chunks /\ Senders # {}
ASSUME GuardDoubleFree \in BOOLEAN /\ AllOrNone \in BOOLEAN

ChunkIds == 1..Chunks

VARIABLES free,     \* the free chunks: a set of chunk ids (meta live bit clear)
          held,     \* held[s]: the block s holds, first chunk first; <<>> when none
          stale,    \* stale[s]: the block s last freed, kept for a second free
          gen,      \* gen[i]: chunk i's generation tag (1 once a take has tagged it)
          refusal,  \* the last refusal: [want |-> k, had |-> free count then]; want = 0 before any
          refFree   \* the free set at the last refusal, for RefuseOnlyWhenNoRun

vars == <<free, held, stale, gen, refusal, refFree>>

Range(seq) == { seq[i] : i \in DOMAIN seq }

InUse == UNION { Range(held[s]) : s \in Senders }

\* a contiguous run of k free chunks starting at a
RunAt(a, k, f) == a + k - 1 <= Chunks /\ \A j \in 0..(k - 1) : (a + j) \in f

HasRun(k, f) == \E a \in ChunkIds : RunAt(a, k, f)

RunSeq(a, k) == [ i \in 1..k |-> a + i - 1 ]

Init ==
    /\ free = ChunkIds
    /\ held = [ s \in Senders |-> <<>> ]
    /\ stale = [ s \in Senders |-> <<>> ]
    /\ gen = [ i \in ChunkIds |-> 0 ]
    /\ refusal = [ want |-> 0, had |-> 0 ]
    /\ refFree = ChunkIds

\* rn_send_store_alloc: take one contiguous run of k chunks, or refuse whole
Alloc(s, k) ==
    /\ held[s] = <<>>
    /\ IF HasRun(k, free)
       THEN \E a \in ChunkIds :
            /\ RunAt(a, k, free)
            /\ held' = [ held EXCEPT ![s] = RunSeq(a, k) ]
            /\ free' = free \ Range(RunSeq(a, k))
            /\ stale' = [ stale EXCEPT ![s] = <<>> ]
            /\ gen' = [ i \in ChunkIds |-> IF i \in Range(RunSeq(a, k)) THEN 1 ELSE gen[i] ]
            /\ UNCHANGED <<refusal, refFree>>
       ELSE /\ refusal' = [ want |-> k, had |-> Cardinality(free) ]
            /\ refFree' = free
            /\ IF AllOrNone
               THEN UNCHANGED <<free, held, stale, gen>>
               \* the reversed rule: take what is there, then report the refusal;
               \* the chunks taken belong to no one
               ELSE /\ free' = {}
                    /\ UNCHANGED <<held, stale, gen>>

\* rn_send_store_free_checked, through the live handle: return the block's run
Free(s) ==
    /\ held[s] # <<>>
    /\ free' = free \cup Range(held[s])
    /\ held' = [ held EXCEPT ![s] = <<>> ]
    /\ stale' = [ stale EXCEPT ![s] = held[s] ]
    /\ UNCHANGED <<gen, refusal, refFree>>

\* the same handle freed again, before any of its chunks is reallocated
FreeAgain(s) ==
    /\ held[s] = <<>>
    /\ stale[s] # <<>>
    /\ Range(stale[s]) \cap InUse = {}
    /\ IF GuardDoubleFree
       THEN UNCHANGED <<free, held>>            \* the head's LIVE is clear: refused, nothing changes
       ELSE /\ free' = free \cup Range(stale[s])
            /\ UNCHANGED held
    /\ stale' = [ stale EXCEPT ![s] = <<>> ]
    /\ UNCHANGED <<gen, refusal, refFree>>

Next ==
    \/ \E s \in Senders, k \in 1..MaxBlock : Alloc(s, k)
    \/ \E s \in Senders : Free(s)
    \/ \E s \in Senders : FreeAgain(s)

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------

TypeOK ==
    /\ free \subseteq ChunkIds
    /\ held \in [ Senders -> Seq(ChunkIds) ]
    /\ stale \in [ Senders -> Seq(ChunkIds) ]
    /\ gen \in [ ChunkIds -> 0..1 ]  \* the tag's presence (the code's 10-bit tag, bounded for the model)
    /\ refusal \in [ want : 0..MaxBlock, had : 0..Chunks ]
    /\ refFree \subseteq ChunkIds

\* no chunk is in two places: never free and held, never in two blocks,
\* never twice in one block; every held block is one contiguous run
NoDoubleAllocate ==
    /\ free \cap InUse = {}
    /\ \A s \in Senders : Len(held[s]) = Cardinality(Range(held[s]))
    /\ \A s, t \in Senders : s # t => Range(held[s]) \cap Range(held[t]) = {}
    /\ \A s \in Senders : held[s] # <<>> =>
        \A i \in 1..Len(held[s]) : held[s][i] = held[s][1] + i - 1

\* free + used = total: no chunk is lost
NoLeak ==
    Cardinality(free) + Cardinality(InUse) = Chunks

\* a refusal only when the free chunks held no contiguous run of the want
RefuseOnlyWhenNoRun ==
    refusal.want > 0 => ~HasRun(refusal.want, refFree)

\* a held block is never longer than a send may take
ChainsBounded ==
    \A s \in Senders : Len(held[s]) <= MaxBlock


\* a held chunk carries a generation its take bumped: never zero
HeldGenerationPositive ==
    \A s \in Senders : \A i \in Range(held[s]) : gen[i] > 0

=============================================================================
