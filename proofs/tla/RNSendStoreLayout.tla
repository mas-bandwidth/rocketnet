------------------------- MODULE RNSendStoreLayout -------------------------
(***************************************************************************)
(* The send store and the send descriptor pool of one connection region    *)
(* as laid out in sdk/rocketnet_allocator.cpp, with the code's own         *)
(* arithmetic.  One store class is the whole store: the three classes      *)
(* share no state.                                                         *)
(*                                                                         *)
(* The class: N chunks, one meta word per chunk (a live bit and a          *)
(* generation), the lazy watermark fresh (chunks at or past it were never  *)
(* taken and their meta is whatever bytes the memory held), the scan       *)
(* cursor free_head and free_count.  rn_send_store_find scans from         *)
(* free_head up for the first chunk that is at or past fresh or whose live *)
(* bit is clear; rn_send_store_take marks it live under its next           *)
(* generation (1 on a first take, skipping 0 on a wrap) and moves fresh    *)
(* and the cursor past it; rn_send_store_free_checked clears the live bit  *)
(* and lowers the cursor to the freed chunk.                               *)
(*                                                                         *)
(* The descriptors: Q of them, a LIFO free stack stored in the             *)
(* descriptors' own free_slot fields (the entry at stack depth k is        *)
(* desc[k].slot), descriptors_free_top, descriptors_free and the lazy      *)
(* watermark descriptors_fresh.  rn_region_descriptor_take pops the stack  *)
(* or takes the first fresh descriptor, clears the descriptor whole and    *)
(* keeps its free_slot when the descriptor sits below the stack's new top  *)
(* (keep_free_slot), because that field is the stack entry at that depth.  *)
(* rn_region_descriptor_release pushes the index into desc[top].slot.      *)
(*                                                                         *)
(* Garbage: the region's memory is not cleared, so Init gives every meta   *)
(* word an arbitrary live bit and every descriptor an adversarial value    *)
(* (in use, generation 0, slot 0, chunk 0), and Carve leaves them as the   *)
(* previous connection left them.  The ghost touchedPast records any read  *)
(* or write of a meta word at or past fresh or of a descriptor at or past  *)
(* descriptors_fresh; the code must do neither.                            *)
(*                                                                         *)
(* Generations run modulo GenMod (2^31 for a chunk's tag and 2^32 for a    *)
(* descriptor's in the code).  Sends are the region's: Alloc is            *)
(* rn_region_send_alloc (a chunk and a descriptor), AllocSpan the          *)
(* descriptor take of rn_region_send_alloc_span (the span itself is        *)
(* RNSendSpans.tla's), Refuse a refused allocation (sticky exhaustion),    *)
(* Free rn_region_send_free, Carve rn_region_carve.                        *)
(*                                                                         *)
(* Reversed witnesses (each flag FALSE is a real bug class):               *)
(*   FreshGuard    the free test reads meta without the i >= fresh guard   *)
(*   LowerCursor   a free that does not lower the cursor                   *)
(*   ChunkSkipZero a chunk generation that wraps to 0                      *)
(*   DescSkipZero  a descriptor generation that wraps to 0      *)
(*   KeepFreeSlot  the take clears a free_slot the stack still uses        *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    N,              \* chunks in the class
    Q,              \* send descriptors
    GenMod,         \* generations run 0..GenMod-1
    FreshGuard,     \* rn_store_chunk_is_free tests i >= fresh before the meta read
    LowerCursor,    \* rn_send_store_free_checked lowers free_head to the freed chunk
    ChunkSkipZero,  \* a chunk's generation skips 0 when it wraps
    DescSkipZero,   \* a descriptor's generation skips 0 when it wraps
    KeepFreeSlot    \* rn_region_descriptor_take keeps a free_slot below the new top

ASSUME N \in Nat \ {0} /\ Q \in Nat \ {0} /\ GenMod \in Nat /\ GenMod >= 3
ASSUME \A f \in {FreshGuard, LowerCursor, ChunkSkipZero, DescSkipZero, KeepFreeSlot} : f \in BOOLEAN

Chunks == 0..(N - 1)
Descs == 0..(Q - 1)
Gens == 0..(GenMod - 1)
NONE == N                       \* RN_CHUNK_NONE in a descriptor's chunk field

Garbage == [ inUse |-> TRUE, gen |-> 0, slot |-> 0, chunk |-> 0 ]

VARIABLES
    meta,           \* meta[i]: chunk i's meta word, [live, gen]
    freeHead,       \* the class's free_head
    freeCount,      \* the class's free_count
    fresh,          \* the class's fresh
    desc,           \* desc[d]: descriptor d's in_use, generation, free_slot and chunk
    dTop,           \* descriptors_free_top
    dFree,          \* descriptors_free
    dFresh,         \* descriptors_fresh
    exhausted,      \* the region's sticky exhausted flag
    touchedPast,    \* ghost: meta at or past fresh, or a descriptor at or past dFresh, read or written
    takenTwice,     \* ghost: a take handed out a chunk or a descriptor that was live
    badRefusal      \* ghost: the store refused while a chunk was free

vars == <<meta, freeHead, freeCount, fresh, desc, dTop, dFree, dFresh, exhausted, touchedPast, takenTwice, badRefusal>>

Min(S) == CHOOSE x \in S : \A y \in S : x <= y

\* the chunks that are free in truth: never taken, or taken with the live bit clear
FreeChunks == { i \in Chunks : i >= fresh \/ ~meta[i].live }

\* rn_store_chunk_is_free, and whether that test reads the meta word
IsFree(i) == IF FreshGuard THEN i >= fresh \/ ~meta[i].live ELSE ~meta[i].live
TestReads(i) == ~FreshGuard \/ i < fresh

\* rn_send_store_find: from free_head up, the first chunk the test calls free; N when none
Found == IF \E i \in freeHead..(N - 1) : IsFree(i)
         THEN Min({ i \in freeHead..(N - 1) : IsFree(i) })
         ELSE N
FindOK == freeCount > 0 /\ Found < N

\* the chunks the scan examined, and whether it read meta at or past fresh
Scanned == IF freeCount = 0 THEN {} ELSE freeHead..Min({Found, N - 1})
ScanTouchesPast == \E i \in Scanned : TestReads(i) /\ i >= fresh

NextGen(g, skip) == IF skip THEN (IF g = GenMod - 1 THEN 1 ELSE g + 1) ELSE (g + 1) % GenMod

\* rn_region_descriptor_take, read off the current state
DFreshTake == dTop = 0
DTaken == IF DFreshTake THEN dFresh ELSE desc[dTop - 1].slot
DNewTop == IF DFreshTake THEN 0 ELSE dTop - 1
DGen == IF DFreshTake THEN 1
        ELSE IF DescSkipZero THEN NextGen(desc[DTaken].gen, TRUE)
        ELSE (desc[DTaken].gen + 1) % GenMod
DKeep == ~DFreshTake /\ DTaken < DNewTop
DTakeTouchesPast == ~DFreshTake /\ (dTop - 1 >= dFresh \/ DTaken >= dFresh)
DTakeTwice == ~DFreshTake /\ desc[DTaken].inUse

\* the descriptor the take initializes: cleared whole, its generation, its kept free_slot, its chunk
DTakeRecord(chunk) ==
    [ inUse |-> TRUE,
      gen |-> DGen,
      slot |-> IF DKeep /\ KeepFreeSlot THEN desc[DTaken].slot ELSE 0,
      chunk |-> chunk ]

DTakeSteps(chunk) ==
    /\ desc' = [ desc EXCEPT ![DTaken] = DTakeRecord(chunk) ]
    /\ dTop' = DNewTop
    /\ dFresh' = IF DFreshTake THEN dFresh + 1 ELSE dFresh
    /\ dFree' = dFree - 1

Init ==
    /\ meta \in [ Chunks -> [ live : BOOLEAN, gen : {0} ] ]
    /\ freeHead = 0
    /\ freeCount = N
    /\ fresh = 0
    /\ desc = [ d \in Descs |-> Garbage ]
    /\ dTop = 0
    /\ dFree = Q
    /\ dFresh = 0
    /\ exhausted = FALSE
    /\ touchedPast = FALSE
    /\ takenTwice = FALSE
    /\ badRefusal = FALSE

\* rn_region_send_alloc with room: rn_region_send_fit_store's checks, then
\* rn_send_store_take of the chunk rn_send_store_find returned, then
\* rn_region_descriptor_take
Alloc ==
    /\ ~exhausted
    /\ dFree > 0
    /\ FindOK
    /\ DTaken < Q
    /\ LET i == Found
           g == IF i < fresh THEN NextGen(meta[i].gen, ChunkSkipZero) ELSE 1
       IN /\ meta' = [ meta EXCEPT ![i] = [ live |-> TRUE, gen |-> g ] ]
          /\ fresh' = IF i < fresh THEN fresh ELSE i + 1
          /\ freeCount' = freeCount - 1
          /\ freeHead' = i + 1
          /\ DTakeSteps(i)
          /\ touchedPast' = (touchedPast \/ ScanTouchesPast \/ DTakeTouchesPast)
          /\ takenTwice' = (takenTwice \/ i \notin FreeChunks \/ DTakeTwice)
    /\ UNCHANGED <<exhausted, badRefusal>>

\* rn_region_send_alloc_span with room: the descriptor take alone
AllocSpan ==
    /\ ~exhausted
    /\ dFree > 0
    /\ DTaken < Q
    /\ DTakeSteps(NONE)
    /\ touchedPast' = (touchedPast \/ DTakeTouchesPast)
    /\ takenTwice' = (takenTwice \/ DTakeTwice)
    /\ UNCHANGED <<meta, freeHead, freeCount, fresh, exhausted, badRefusal>>

\* rn_region_send_fit_store refuses (no descriptor, or the store's find
\* fails) and rn_region_send_refuse exhausts the region
Refuse ==
    /\ ~exhausted
    /\ dFree = 0 \/ ~FindOK
    /\ exhausted' = TRUE
    /\ badRefusal' = (badRefusal \/ (dFree > 0 /\ FreeChunks # {}))
    /\ touchedPast' = (touchedPast \/ (dFree > 0 /\ ScanTouchesPast))
    /\ UNCHANGED <<meta, freeHead, freeCount, fresh, desc, dTop, dFree, dFresh, takenTwice>>

\* rn_region_send_free: rn_send_store_free_checked of the descriptor's chunk,
\* then rn_region_descriptor_release pushes the descriptor
Free(d) ==
    /\ d < dFresh
    /\ desc[d].inUse
    /\ dTop < Q
    /\ LET c == desc[d].chunk
           frees == c # NONE /\ c < fresh /\ meta[c].live
       IN /\ meta' = IF frees THEN [ meta EXCEPT ![c].live = FALSE ] ELSE meta
          /\ freeCount' = IF frees THEN freeCount + 1 ELSE freeCount
          /\ freeHead' = IF frees /\ LowerCursor /\ c < freeHead THEN c ELSE freeHead
    /\ desc' = [ desc EXCEPT ![d].inUse = FALSE, ![d].chunk = NONE, ![dTop].slot = d ]
    /\ dTop' = dTop + 1
    /\ dFree' = dFree + 1
    /\ touchedPast' = (touchedPast \/ dTop >= dFresh)
    /\ UNCHANGED <<fresh, dFresh, exhausted, takenTwice, badRefusal>>

\* rn_region_carve (rn_send_store_init_counts for the class): both pools
\* empty and both watermarks at zero; modeled meta/descriptor fields unchanged.
\* SDK carve separately clears enqueue_ticket/schedule_flags below the old
\* descriptor watermark before lowering it; these scheduling fields are omitted.
Carve ==
    /\ freeHead' = 0
    /\ freeCount' = N
    /\ fresh' = 0
    /\ dTop' = 0
    /\ dFree' = Q
    /\ dFresh' = 0
    /\ exhausted' = FALSE
    /\ UNCHANGED <<meta, desc, touchedPast, takenTwice, badRefusal>>

Next ==
    \/ Alloc
    \/ AllocSpan
    \/ Refuse
    \/ \E d \in Descs : Free(d)
    \/ Carve

Spec == Init /\ [][Next]_vars

\* The fingerprint view for TLC: a meta word at or past fresh, a descriptor at
\* or past descriptors_fresh and a free_slot at or above the stack's top are
\* dead values, written before they are next read, so states that differ only
\* in them are one state.  A read of one before its write is the violation
\* NeverTouchPastFresh records (the ghosts stay in the view); a free_slot at
\* or above the top is read by no action.
View ==
    << [ i \in Chunks |-> IF i < fresh THEN meta[i] ELSE "dead" ],
       freeHead, freeCount, fresh,
       [ d \in Descs |-> IF d < dFresh
                         THEN [ desc[d] EXCEPT !.slot = IF d < dTop THEN @ ELSE 0 ]
                         ELSE "dead" ],
       dTop, dFree, dFresh, exhausted, touchedPast, takenTwice, badRefusal >>

----------------------------------------------------------------------------

TypeOK ==
    /\ meta \in [ Chunks -> [ live : BOOLEAN, gen : Gens ] ]
    /\ freeHead \in 0..N
    /\ freeCount \in 0..N
    /\ fresh \in 0..N
    /\ desc \in [ Descs -> [ inUse : BOOLEAN, gen : Gens, slot : Descs, chunk : 0..N ] ]
    /\ dTop \in 0..Q
    /\ dFree \in 0..Q
    /\ dFresh \in 0..Q
    /\ exhausted \in BOOLEAN
    /\ touchedPast \in BOOLEAN /\ takenTwice \in BOOLEAN /\ badRefusal \in BOOLEAN

LiveDescs == { d \in 0..(dFresh - 1) : desc[d].inUse }
Stack == [ k \in 0..(dTop - 1) |-> desc[k].slot ]

\* no meta word at or past fresh, and no descriptor at or past descriptors_fresh, is read or written
NeverTouchPastFresh == ~touchedPast

\* no chunk and no descriptor is handed out while it is live
NoTakeWhileLive == ~takenTwice

\* the store refuses only when no chunk is free
RefuseOnlyWhenNoneFree == ~badRefusal

\* free_count is the free chunks
FreeCountExact == freeCount = Cardinality(FreeChunks)

\* free_head is at or below fresh and below every free chunk: the scan from it finds the lowest
CursorBelowFree == freeHead <= fresh /\ \A i \in FreeChunks : freeHead <= i

\* a chunk below fresh is live exactly when one live descriptor names it
LiveIsHeld ==
    /\ \A i \in 0..(fresh - 1) : meta[i].live <=> \E d \in LiveDescs : desc[d].chunk = i
    /\ \A d1, d2 \in LiveDescs : d1 # d2 /\ desc[d1].chunk # NONE => desc[d1].chunk # desc[d2].chunk
    /\ \A d \in LiveDescs : desc[d].chunk = NONE \/ desc[d].chunk < fresh

\* the stack holds exactly the released descriptors, each once, below the watermark
StackExact ==
    /\ dTop <= dFresh
    /\ \A j, k \in 0..(dTop - 1) : j # k => Stack[j] # Stack[k]
    /\ { Stack[k] : k \in 0..(dTop - 1) } = { d \in 0..(dFresh - 1) : ~desc[d].inUse }

\* descriptors_free is the stack plus the never-taken descriptors
DescCountExact == dFree = dTop + (Q - dFresh) /\ dFree = Q - Cardinality(LiveDescs)

\* a live chunk's or descriptor's generation is never 0
HeldGenerationPositive ==
    /\ \A i \in 0..(fresh - 1) : meta[i].live => meta[i].gen # 0
    /\ \A d \in LiveDescs : desc[d].gen # 0

=============================================================================
