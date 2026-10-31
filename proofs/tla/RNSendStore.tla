---------------------------- MODULE RNSendStore ----------------------------
(***************************************************************************)
(* One class of the message layer's send store, abstractly (docs/CHANNELS.md *)
(* 4 and 15; sdk/rocketnet_allocator.cpp, rn_send_store_take and           *)
(* rn_send_store_free_checked).  The three classes share no state, so one  *)
(* class is the whole model.  RNSendStoreLayout.tla is the same store with *)
(* the code's arithmetic: the meta array, the watermark and the cursor.    *)
(*                                                                         *)
(* A send of up to one fragment holds one chunk.  Alloc takes any free     *)
(* chunk and tags it with a generation; Free returns it.  An allocated     *)
(* chunk's meta carries LIVE, and free refuses a handle whose chunk does   *)
(* not.                                                                    *)
(*                                                                         *)
(* Senders are the region's send descriptors.  A sender may free its chunk *)
(* twice (a caller bug); the guard is what turns the second free into a    *)
(* refusal.  The second free is modelled only while the chunk has not been *)
(* reallocated: a free through a handle whose chunk now belongs to another *)
(* sender is indistinguishable from that owner's own free, and no          *)
(* allocator-local check can see it.  The layer above owns that (a         *)
(* descriptor's handle is dropped at its free, layer 1; the per-number     *)
(* table never names a freed descriptor, layer 3).                         *)
(*                                                                         *)
(* GuardDoubleFree = FALSE removes the guard; it must fail, and is the     *)
(* reversed witness.                                                       *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Chunks,           \* the class's chunk count, N
          Senders,          \* the descriptors that may hold a chunk
          GuardDoubleFree   \* free_checked's LIVE test

ASSUME Chunks \in Nat \ {0} /\ Senders # {}
ASSUME GuardDoubleFree \in BOOLEAN

ChunkIds == 1..Chunks
NONE == 0

VARIABLES free,     \* the free chunks (meta live bit clear)
          held,     \* held[s]: the chunk s holds, NONE when none
          stale,    \* stale[s]: the chunk s last freed, kept for a second free
          gen,      \* gen[i]: chunk i's generation tag (1 once a take has tagged it)
          freeCount \* the class's free_count

vars == <<free, held, stale, gen, freeCount>>

InUse == { held[s] : s \in Senders } \ {NONE}

Init ==
    /\ free = ChunkIds
    /\ held = [ s \in Senders |-> NONE ]
    /\ stale = [ s \in Senders |-> NONE ]
    /\ gen = [ i \in ChunkIds |-> 0 ]
    /\ freeCount = Chunks

\* rn_send_store_take of the chunk rn_send_store_find returned
Alloc(s) ==
    /\ held[s] = NONE
    /\ \E i \in free :
        /\ held' = [ held EXCEPT ![s] = i ]
        /\ free' = free \ {i}
        /\ stale' = [ stale EXCEPT ![s] = NONE ]
        /\ gen' = [ gen EXCEPT ![i] = 1 ]   \* the take tags the slot (the code's counter, bounded for the model)
        /\ freeCount' = freeCount - 1

\* rn_send_store_free_checked, through the live handle
Free(s) ==
    /\ held[s] # NONE
    /\ free' = free \cup {held[s]}
    /\ held' = [ held EXCEPT ![s] = NONE ]
    /\ stale' = [ stale EXCEPT ![s] = held[s] ]
    /\ freeCount' = freeCount + 1
    /\ UNCHANGED gen

\* the same handle freed again, before its chunk is reallocated
FreeAgain(s) ==
    /\ held[s] = NONE
    /\ stale[s] # NONE
    /\ stale[s] \notin InUse
    /\ IF GuardDoubleFree
       THEN UNCHANGED freeCount             \* the chunk's LIVE is clear: refused, nothing changes
       ELSE freeCount' = freeCount + 1      \* free_count counts the chunk a second time
    /\ stale' = [ stale EXCEPT ![s] = NONE ]
    /\ UNCHANGED <<free, held, gen>>

Next ==
    \/ \E s \in Senders : Alloc(s)
    \/ \E s \in Senders : Free(s)
    \/ \E s \in Senders : FreeAgain(s)

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------

TypeOK ==
    /\ free \subseteq ChunkIds
    /\ held \in [ Senders -> ChunkIds \cup {NONE} ]
    /\ stale \in [ Senders -> ChunkIds \cup {NONE} ]
    /\ gen \in [ ChunkIds -> 0..1 ]
    /\ freeCount \in Nat

\* no chunk is free and held, or held by two senders
NoDoubleAllocate ==
    /\ free \cap InUse = {}
    /\ \A s, t \in Senders : s # t /\ held[s] # NONE => held[s] # held[t]

\* free_count is the free chunks, and free plus held is the class
NoLeak ==
    /\ freeCount = Cardinality(free)
    /\ Cardinality(free) + Cardinality(InUse) = Chunks

\* a held chunk carries a generation its take bumped: never zero
HeldGenerationPositive ==
    \A i \in InUse : gen[i] > 0

=============================================================================
