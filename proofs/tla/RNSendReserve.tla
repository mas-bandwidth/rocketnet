---------------------------- MODULE RNSendReserve ----------------------------
(***************************************************************************)
(* The protocol send reserve of one send queue                             *)
(* (sdk/rocketnet_server_transport.cpp: open_fill_budget, fill_claim,      *)
(* queue_budget_now, fill_started, claim_frames; README-RNSendReserve.md)  *)
(*                                                                         *)
(* The game's fillers and the protocol share the queue's staging.  Main    *)
(* opens a fill phase with a budget, the grants the fill may take: the     *)
(* room above the reserve the next update's protocol frames need.          *)
(*                                                                         *)
(* A filler's frame goes one of two ways.  A client's own frame (its first *)
(* frame this phase, once its keep-alive is acknowledged) takes the frame  *)
(* the reserve holds for that client: it is staged, then counted in the    *)
(* own frames used, which lowers the reserve the budget keeps back.  Any   *)
(* other frame belongs to a claim made with one atomic add on the claims.  *)
(* A claim is a chunk of Chunk frames while the budget less the claims is  *)
(* at least Chunk x GuardWidth (the guard, two loads before the add), else *)
(* one frame.  It is granted when the whole claim fits under the budget;   *)
(* else the budget is recomputed from the queue as it reads now (the       *)
(* grants staged and the own frames used, then the room, then those two    *)
(* again, starting over if either moved), raised to that, never lowered,   *)
(* and the claims outstanding tested against it.  The test and the take    *)
(* back of a refused claim are two steps: another filler's claim may be    *)
(* the one that put the claims past the budget, and it may be taken back   *)
(* in between.  A refused chunk is taken back whole and claimed again as   *)
(* one frame; a refused single claim is taken back, and then the claims    *)
(* and the budget are read again: a frame now under the budget is claimed  *)
(* again (the retest), else the claim is refused.  Two fillers refused     *)
(* together by each other's claims would otherwise both be refused with a  *)
(* frame left under the budget (witness NoRetry).                          *)
(* A granted claim is the filler's tail: each frame of it is               *)
(* staged (it leaves the room) and then counted in the grants staged; the  *)
(* run ends at any frame, and what is left of the tail goes back with one  *)
(* add.  The transport may retire staged frames at any time (an            *)
(* asynchronous send), which returns room.  The two counts are one atomic  *)
(* each per queue, added per frame.                                        *)
(*                                                                         *)
(* Properties.  ReserveKept: once the fill holds a frame, the room left    *)
(* after every frame in flight has staged still covers the reserve (less   *)
(* the own frames taken).  NeverStarved: no single claim is refused while  *)
(* the room its recompute read had a frame for it past the reserve and     *)
(* past every other filler in flight, a filler holding a tail counting as  *)
(* that tail (with Chunk = 1, exactly one frame per filler in flight: the  *)
(* property as it stood before chunks).  TailsNamed: a refusal with room   *)
(* for the frame past every filler in flight comes only while another      *)
(* filler holds a tail, so the code counts it under its own name           *)
(* (SEND_RESERVE_REFUSED_TAILS_HELD), apart from a true full.              *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Fillers,        \* the fill threads
    Reserve,        \* the queue's protocol reserve, in frames
    MaxFree,        \* the queue's staging, in frames
    MaxOwn,         \* own frames main may hand the fill in one phase
    MaxClaims,      \* claims per filler in a behaviour
    MaxRetire,      \* frames the transport may retire in a behaviour
    MaxPhases,      \* fill phases main opens in a behaviour
    Chunk,          \* frames per chunk claim (1: every claim is one frame)
    GuardWidth,     \* the guard's filler bound (0: no guard)
    \* reversed witnesses, each TRUE breaks one rule of the code
    RoomOnly,       \* the budget is the whole room, no reserve           (ReserveKept)
    SplitClaim,     \* the claim is a load then a store, not one add      (ReserveKept)
    RoomFirst,      \* the recompute reads the room before the counts     (ReserveKept)
    OwnCountFirst,  \* an own frame is counted before it is staged        (ReserveKept)
    NoRefresh,      \* the budget stays as main opened it                 (NeverStarved)
    NoReread,       \* the counts are read once, not again after the room (NeverStarved)
    IndexTest,      \* the recompute tests the claim's index, not the claims outstanding (NeverStarved)
    NoFallback,     \* a refused chunk is not claimed again as one frame  (NeverStarved)
    BatchCount,     \* staged frames are counted at the run's end         (NeverStarved)
    NoTailReturn,   \* a run's unused tail is never given back            (TailsNamed)
    NoRetry         \* a single claim taken back is refused, no retest    (NeverStarved)

ASSUME MaxOwn <= Reserve
ASSUME Chunk >= 1

VARIABLES
    free,           \* the queue's free frames, as the transport reports them
    started,        \* fill frames staged and counted this phase (fill_started)
    ownUsed,        \* own frames staged and counted this phase (fill_own_used)
    ownTaken,       \* own frames taken this phase (fill_own_frame cleared)
    ownLeft,        \* own frames main handed the fill and not yet taken
    claims,         \* the claims counter (fill_claims)
    budget,         \* the grant cap (fill_budget)
    pc,             \* each filler's step
    size,           \* the frames a filler's claim asks for
    claim,          \* the claim index a filler holds
    tail,           \* granted frames a filler holds, not yet staged
    uncounted,      \* staged frames a filler has not counted (BatchCount only)
    seenS,          \* the recompute's reads: grants staged,
    seenO,          \*   own frames used,
    seenR,          \*   room
    starved,        \* a refusal came with room past every filler and tail (history)
    unnamed,        \* a refusal came with room and no tail held by another (history)
    claimsDone,
    retired,
    phases

vars == <<free, started, ownUsed, ownTaken, ownLeft, claims, budget, pc, size,
          claim, tail, uncounted, seenS, seenO, seenR, starved, unnamed,
          claimsDone, retired, phases>>

Steps == {"idle", "sized", "loaded", "claimed", "read1", "read2", "read3",
          "refused", "retest", "granted", "counting", "next", "ownio", "owncount"}

Max(a, b) == IF a > b THEN a ELSE b

RECURSIVE SumOver(_, _)
SumOver(fn, S) == IF S = {} THEN 0
                  ELSE LET x == CHOOSE x \in S : TRUE IN fn[x] + SumOver(fn, S \ {x})

\* the room the fill may take above the reserve still held back
Above(r, o) == LET held == IF Reserve > o THEN Reserve - o ELSE 0
               IN IF RoomOnly THEN r ELSE IF r > held THEN r - held ELSE 0

InStep(s) == Cardinality({f \in Fillers : pc[f] = s})
Pending == SumOver(tail, Fillers)               \* granted, not yet staged
Unstaged == SumOver(uncounted, Fillers)         \* staged, not yet counted (BatchCount)
OwnUnstaged == InStep("ownio")                  \* own frame taken, not yet staged
Busy(f) == Cardinality({g \in Fillers \ {f} : pc[g] /= "idle"})
\* a filler's chunk hold: a chunk claim from its add until it is granted or
\* taken back, then the tail of it not yet staged (the code's tail_held:
\* raised by the chunk at its add, lowered at a take back, per frame
\* staged, and by the rest at the tail's return)
Checking == {"claimed", "read1", "read2", "read3", "refused"}
TailHeld(g) == tail[g] + (IF pc[g] \in Checking /\ size[g] > 1 THEN size[g] ELSE 0)
OthersTail(f) == SumOver([g \in Fillers |-> IF g = f THEN 0 ELSE TailHeld(g)], Fillers)
\* another filler's hold on the room: its chunk hold, plus the frame it has
\* staged and not yet counted; at least one frame while it is in flight
Held(g) == IF pc[g] = "idle" THEN 0
           ELSE Max(1, TailHeld(g) + (IF pc[g] = "counting" THEN 1 ELSE 0))
OthersHeld(f) == SumOver([g \in Fillers |-> IF g = f THEN 0 ELSE Held(g)], Fillers)

\* a room of r frames is above what the protocol still needs, with a frame
\* for f and for every other filler in flight now
RoomFor(f, r) == r + ownTaken > Reserve + Busy(f)
\* the same, past every tail the other fillers hold
RoomPastTails(f, r) == r + ownTaken > Reserve + OthersHeld(f)

TypeOK ==
    /\ free \in 0..MaxFree
    /\ started \in Nat
    /\ ownUsed \in Nat
    /\ ownTaken \in 0..MaxOwn
    /\ ownLeft \in 0..MaxOwn
    /\ claims \in Nat
    /\ budget \in Nat
    /\ pc \in [Fillers -> Steps]
    /\ size \in [Fillers -> 1..Chunk]
    /\ claim \in [Fillers -> Nat]
    /\ tail \in [Fillers -> 0..Chunk]
    /\ uncounted \in [Fillers -> 0..Chunk]
    /\ seenS \in [Fillers -> Nat]
    /\ seenO \in [Fillers -> Nat]
    /\ seenR \in [Fillers -> Nat]
    /\ starved \in BOOLEAN
    /\ unnamed \in BOOLEAN
    /\ claimsDone \in [Fillers -> 0..MaxClaims]
    /\ retired \in 0..MaxRetire
    /\ phases \in 0..MaxPhases

\* the state just after main opens the first phase
Init ==
    /\ free \in 0..MaxFree
    /\ ownLeft \in 0..MaxOwn
    /\ started = 0
    /\ ownUsed = 0
    /\ ownTaken = 0
    /\ claims = 0
    /\ budget = Above(free, 0)
    /\ pc = [f \in Fillers |-> "idle"]
    /\ size = [f \in Fillers |-> 1]
    /\ claim = [f \in Fillers |-> 0]
    /\ tail = [f \in Fillers |-> 0]
    /\ uncounted = [f \in Fillers |-> 0]
    /\ seenS = [f \in Fillers |-> 0]
    /\ seenO = [f \in Fillers |-> 0]
    /\ seenR = [f \in Fillers |-> 0]
    /\ starved = FALSE
    /\ unnamed = FALSE
    /\ claimsDone = [f \in Fillers |-> 0]
    /\ retired = 0
    /\ phases = 1

(***************************************************************************)
(* Main, with no fill running: the update's protocol frames take what      *)
(* they need of the room (at most the reserve), then the next phase opens. *)
(***************************************************************************)
OpenFill ==
    /\ phases < MaxPhases
    /\ \A f \in Fillers : pc[f] = "idle"
    /\ \E used \in 0..(IF free < Reserve THEN free ELSE Reserve), own \in 0..MaxOwn :
          /\ free' = free - used
          /\ budget' = Above(free - used, 0)
          /\ ownLeft' = own
    /\ started' = 0
    /\ ownUsed' = 0
    /\ ownTaken' = 0
    /\ claims' = 0
    /\ phases' = phases + 1
    /\ claimsDone' = [f \in Fillers |-> 0]
    /\ UNCHANGED <<pc, size, claim, tail, uncounted, seenS, seenO, seenR, starved,
                   unnamed, retired>>

(***************************************************************************)
(* A client's own frame: taken, staged, counted.                           *)
(***************************************************************************)
OwnClaim(f) ==
    /\ pc[f] = "idle"
    /\ claimsDone[f] < MaxClaims
    /\ ownLeft > 0
    /\ ownLeft' = ownLeft - 1
    /\ ownTaken' = ownTaken + 1
    /\ ownUsed' = IF OwnCountFirst THEN ownUsed + 1 ELSE ownUsed
    /\ pc' = [pc EXCEPT ![f] = "ownio"]
    /\ claimsDone' = [claimsDone EXCEPT ![f] = @ + 1]
    /\ UNCHANGED <<free, started, claims, budget, size, claim, tail, uncounted,
                   seenS, seenO, seenR, starved, unnamed, retired, phases>>

\* staged; a queue with no room refuses it, the client's keep-alive stays due
\* and its frame stays in the reserve
OwnIo(f) ==
    /\ pc[f] = "ownio"
    /\ IF free > 0
          THEN /\ free' = free - 1
               /\ pc' = [pc EXCEPT ![f] = IF OwnCountFirst THEN "idle" ELSE "owncount"]
               /\ UNCHANGED ownTaken
          ELSE /\ pc' = [pc EXCEPT ![f] = "idle"]
               /\ ownTaken' = ownTaken - 1
               /\ UNCHANGED free
    /\ UNCHANGED <<started, ownUsed, ownLeft, claims, budget, size, claim, tail,
                   uncounted, seenS, seenO, seenR, starved, unnamed, claimsDone,
                   retired, phases>>

OwnCount(f) ==
    /\ pc[f] = "owncount"
    /\ ownUsed' = ownUsed + 1
    /\ pc' = [pc EXCEPT ![f] = "idle"]
    /\ UNCHANGED <<free, started, ownTaken, ownLeft, claims, budget, size, claim,
                   tail, uncounted, seenS, seenO, seenR, starved, unnamed,
                   claimsDone, retired, phases>>

(***************************************************************************)
(* Any other frame: size, claim, check, recompute; then the run stages     *)
(* and counts its tail, and gives back what is left.                       *)
(***************************************************************************)

\* the guard: a chunk while the budget less the claims leaves Chunk frames
\* for every filler the guard allows for, else one frame (two loads)
Size(f) ==
    /\ pc[f] = "idle"
    /\ claimsDone[f] < MaxClaims
    /\ size' = [size EXCEPT ![f] =
                  IF Chunk > 1 /\ budget >= claims + Chunk * GuardWidth THEN Chunk ELSE 1]
    /\ pc' = [pc EXCEPT ![f] = "sized"]
    /\ claimsDone' = [claimsDone EXCEPT ![f] = @ + 1]
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, claims, budget, claim,
                   tail, uncounted, seenS, seenO, seenR, starved, unnamed, retired,
                   phases>>

Claim(f) ==
    /\ pc[f] = "sized"
    /\ claim' = [claim EXCEPT ![f] = claims]
    /\ IF SplitClaim
          THEN /\ pc' = [pc EXCEPT ![f] = "loaded"]
               /\ UNCHANGED claims
          ELSE /\ pc' = [pc EXCEPT ![f] = "claimed"]
               /\ claims' = claims + size[f]
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, budget, size, tail,
                   uncounted, seenS, seenO, seenR, starved, unnamed, claimsDone,
                   retired, phases>>

ClaimStore(f) ==
    /\ pc[f] = "loaded"
    /\ pc' = [pc EXCEPT ![f] = "claimed"]
    /\ claims' = claim[f] + size[f]
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, budget, size, claim,
                   tail, uncounted, seenS, seenO, seenR, starved, unnamed,
                   claimsDone, retired, phases>>

\* the refusal decided: the claim is still added until its take back, a
\* step of its own.  A single claim refused is starved if the room the
\* decision read had a frame for f past the reserve and past every frame and
\* tail the other fillers hold; unnamed if that room had a frame for f past
\* every filler in flight while no other filler held a tail.  Room that came
\* back after the read, or that a filler claiming after the read takes, is
\* no starvation.
Refuse(f, r) ==
    /\ pc' = [pc EXCEPT ![f] = "refused"]
    /\ IF size[f] > 1 /\ ~NoFallback
          THEN UNCHANGED <<starved, unnamed>>
          ELSE /\ starved' = (starved \/ RoomPastTails(f, r))
               /\ unnamed' = (unnamed \/ (RoomFor(f, r) /\ OthersTail(f) = 0))

\* a claim refused for good, the claims c outstanding after its take back:
\* starved too if a frame was left under the budget (the claims that
\* refused it were taken back before it gave up)
FinalRefuse(f, c) ==
    /\ pc' = [pc EXCEPT ![f] = "idle"]
    /\ starved' = (starved \/ c + size[f] <= budget)
    /\ UNCHANGED unnamed

\* the take back: the claim leaves the claims whole.  A chunk is claimed
\* again as one frame; a single claim is retested (NoRetry: refused)
TakeBack(f) ==
    /\ pc[f] = "refused"
    /\ claims' = claims - size[f]
    /\ IF size[f] > 1 /\ ~NoFallback
          THEN /\ pc' = [pc EXCEPT ![f] = "sized"]
               /\ size' = [size EXCEPT ![f] = 1]
               /\ UNCHANGED <<starved, unnamed>>
          ELSE IF NoRetry
                  THEN /\ FinalRefuse(f, claims - size[f])
                       /\ UNCHANGED size
                  ELSE /\ pc' = [pc EXCEPT ![f] = "retest"]
                       /\ UNCHANGED <<starved, unnamed, size>>
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, budget, claim, tail,
                   uncounted, seenS, seenO, seenR, claimsDone, retired, phases>>

\* the retest: the claims and the budget read again; a frame under the
\* budget now is claimed again (the claim counts as the same one), else
\* the claim is refused
Retest(f) ==
    /\ pc[f] = "retest"
    /\ IF claims + size[f] <= budget
          THEN /\ pc' = [pc EXCEPT ![f] = "sized"]
               /\ UNCHANGED <<starved, unnamed>>
          ELSE FinalRefuse(f, claims)
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, claims, budget, size,
                   claim, tail, uncounted, seenS, seenO, seenR, claimsDone, retired,
                   phases>>

Grant(f) ==
    /\ pc' = [pc EXCEPT ![f] = "granted"]
    /\ tail' = [tail EXCEPT ![f] = size[f]]

\* the raise (an atomic max) and the test after it
Decide(f, s, o, r) ==
    LET b2 == Max(budget, s + Above(r, o))
        ok == IF IndexTest THEN claim[f] + size[f] <= b2 ELSE claims <= b2
    IN /\ budget' = b2
       /\ IF ok
             THEN /\ Grant(f)
                  /\ UNCHANGED <<claims, starved, unnamed, size>>
             ELSE /\ Refuse(f, r)
                  /\ UNCHANGED <<claims, size, tail>>

\* the whole claim under the budget: granted; else the recompute begins
Check(f) ==
    /\ pc[f] = "claimed"
    /\ IF claim[f] + size[f] <= budget
          THEN /\ Grant(f)
               /\ UNCHANGED <<claims, starved, unnamed, size>>
          ELSE IF NoRefresh
                  THEN /\ Refuse(f, free)
                       /\ UNCHANGED <<claims, size, tail>>
                  ELSE /\ pc' = [pc EXCEPT ![f] = "read1"]
                       /\ UNCHANGED <<claims, starved, unnamed, size, tail>>
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, budget, claim,
                   uncounted, seenS, seenO, seenR, claimsDone, retired, phases>>

\* the first read: the two counts (RoomFirst: the room)
Read1(f) ==
    /\ pc[f] = "read1"
    /\ pc' = [pc EXCEPT ![f] = "read2"]
    /\ IF RoomFirst
          THEN /\ seenR' = [seenR EXCEPT ![f] = free]
               /\ UNCHANGED <<seenS, seenO>>
          ELSE /\ seenS' = [seenS EXCEPT ![f] = started]
               /\ seenO' = [seenO EXCEPT ![f] = ownUsed]
               /\ UNCHANGED <<seenR>>
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, claims, budget, size,
                   claim, tail, uncounted, starved, unnamed, claimsDone, retired,
                   phases>>

\* the second read: the room (RoomFirst: the counts, then decide)
Read2(f) ==
    /\ pc[f] = "read2"
    /\ IF RoomFirst
          THEN /\ Decide(f, started, ownUsed, seenR[f])
               /\ UNCHANGED <<seenS, seenO, seenR>>
          ELSE /\ seenR' = [seenR EXCEPT ![f] = free]
               /\ UNCHANGED <<seenS, seenO>>
               /\ IF NoReread
                     THEN Decide(f, seenS[f], seenO[f], free)
                     ELSE /\ pc' = [pc EXCEPT ![f] = "read3"]
                          /\ UNCHANGED <<claims, budget, starved, unnamed, size, tail>>
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, claim, uncounted,
                   claimsDone, retired, phases>>

\* the counts again: moved, start over; else decide on the three reads
Read3(f) ==
    /\ pc[f] = "read3"
    /\ IF started /= seenS[f] \/ ownUsed /= seenO[f]
          THEN /\ pc' = [pc EXCEPT ![f] = "read1"]
               /\ UNCHANGED <<claims, budget, starved, unnamed, size, tail>>
          ELSE Decide(f, seenS[f], seenO[f], seenR[f])
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, claim, uncounted,
                   seenS, seenO, seenR, claimsDone, retired, phases>>

\* the run ends: what is left of the tail goes back with one add, and
\* (BatchCount) the frames staged are counted only now
EndRun(f) ==
    /\ claims' = IF NoTailReturn THEN claims ELSE claims - tail[f]
    /\ tail' = [tail EXCEPT ![f] = 0]
    /\ started' = started + uncounted[f]
    /\ uncounted' = [uncounted EXCEPT ![f] = 0]
    /\ pc' = [pc EXCEPT ![f] = "idle"]

\* a granted frame is staged: it leaves the room.  A queue with no room
\* refuses it and the run ends, its tail given back.
Stage(f) ==
    /\ pc[f] = "granted"
    /\ IF free > 0
          THEN /\ free' = free - 1
               /\ tail' = [tail EXCEPT ![f] = @ - 1]
               /\ IF BatchCount
                     THEN /\ uncounted' = [uncounted EXCEPT ![f] = @ + 1]
                          /\ pc' = [pc EXCEPT ![f] = "next"]
                     ELSE /\ pc' = [pc EXCEPT ![f] = "counting"]
                          /\ UNCHANGED uncounted
               /\ UNCHANGED <<claims, started>>
          ELSE /\ EndRun(f)
               /\ UNCHANGED free
    /\ UNCHANGED <<ownUsed, ownTaken, ownLeft, budget, size, claim, seenS, seenO,
                   seenR, starved, unnamed, claimsDone, retired, phases>>

Count(f) ==
    /\ pc[f] = "counting"
    /\ started' = started + 1
    /\ pc' = [pc EXCEPT ![f] = "next"]
    /\ UNCHANGED <<free, ownUsed, ownTaken, ownLeft, claims, budget, size, claim,
                   tail, uncounted, seenS, seenO, seenR, starved, unnamed,
                   claimsDone, retired, phases>>

\* the run stages its next frame from the tail, or ends at any frame
Continue(f) ==
    /\ pc[f] = "next"
    /\ tail[f] > 0
    /\ pc' = [pc EXCEPT ![f] = "granted"]
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, claims, budget, size,
                   claim, tail, uncounted, seenS, seenO, seenR, starved, unnamed,
                   claimsDone, retired, phases>>

End(f) ==
    /\ pc[f] = "next"
    /\ EndRun(f)
    /\ UNCHANGED <<free, ownUsed, ownTaken, ownLeft, budget, size, claim, seenS,
                   seenO, seenR, starved, unnamed, claimsDone, retired, phases>>

\* the transport retires a staged frame (an asynchronous send completes)
Retire ==
    /\ retired < MaxRetire
    /\ free < MaxFree
    /\ free' = free + 1
    /\ retired' = retired + 1
    /\ UNCHANGED <<started, ownUsed, ownTaken, ownLeft, claims, budget, pc, size,
                   claim, tail, uncounted, seenS, seenO, seenR, starved, unnamed,
                   claimsDone, phases>>

Done ==
    /\ \A f \in Fillers : pc[f] = "idle"
    /\ UNCHANGED vars

Next ==
    \/ OpenFill \/ Retire \/ Done
    \/ \E f \in Fillers :
          \/ OwnClaim(f) \/ OwnIo(f) \/ OwnCount(f)
          \/ Size(f) \/ Claim(f) \/ ClaimStore(f) \/ Check(f)
          \/ Read1(f) \/ Read2(f) \/ Read3(f) \/ TakeBack(f) \/ Retest(f)
          \/ Stage(f) \/ Count(f) \/ Continue(f) \/ End(f)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Properties                                                              *)
(***************************************************************************)

\* once the fill holds a frame, the room left after every frame in flight
\* has staged covers the reserve, less the own frames taken (their clients'
\* keep-alives are not due): the next update's protocol frames are never
\* refused for the fill's sake
ReserveKept ==
    (started + Pending + Unstaged + InStep("counting") > 0) =>
        free + ownTaken >= Reserve + OwnUnstaged + Pending

\* the fill is never refused while the room it read had a frame for it
\* past the reserve and every filler in flight (each filler holding a
\* tail counting as its tail), nor refused for good while the claims left
\* after its take back leave a frame for it under the budget
NeverStarved == ~starved

\* a refusal with room for the frame past every filler in flight comes only
\* while another filler holds a tail: the code names it apart from a full
TailsNamed == ~unnamed

=============================================================================
