---------------------------- MODULE RNSendReserve ----------------------------
(***************************************************************************)
(* The protocol send reserve of one send queue                             *)
(* (sdk/rocketnet_server_transport.cpp: open_fill_budget, fill_claim,      *)
(* queue_budget_now, fill_started, fill_take_back; README-RNSendReserve.md)*)
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
(* at least Chunk x GuardWidth (the guard), else one frame.  It is granted *)
(* when it fits under the budget; else the budget is recomputed from the   *)
(* queue as it reads now (the grants staged and the own frames used, then  *)
(* the room, then those two again, starting over if either moved), raised  *)
(* to that, never lowered, and the claims outstanding tested against it.   *)
(* A refused chunk is taken back whole and claimed again as one frame; a   *)
(* refused single claim is taken back.  A granted claim is the filler's    *)
(* tail: each frame of it is staged (it leaves the room) and then counted  *)
(* in the grants staged; the run ends at any frame, and what is left of    *)
(* the tail goes back with one add.  The transport may retire staged       *)
(* frames at any time (an asynchronous send), which returns room.          *)
(*                                                                         *)
(* The two counts are kept per line: Slots lines, each filler counting on  *)
(* the line SlotOf picks for it (a hash of its thread id, so two fillers   *)
(* may share one), with an atomic add.  The recompute reads the lines one  *)
(* at a time, in any order, and sums them.                                 *)
(*                                                                         *)
(* Properties.  ReserveKept: once the fill holds a frame, the room left    *)
(* after every frame in flight has staged still covers the reserve (less   *)
(* the own frames taken).  NeverStarved: no single claim is refused while  *)
(* the room its recompute read had a frame for it past the reserve and     *)
(* past every other filler's frame in flight and tail held.  TailsNamed:   *)
(* a refusal that would starve but for the tails held always comes with a  *)
(* tail held by another filler, so the code counts it under its own name   *)
(* (SEND_RESERVE_REFUSED_TAILS_HELD), apart from a true full.              *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Fillers,        \* the fill threads
    Slots,          \* the count lines
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
    NoTailReturn    \* a run's unused tail is never given back            (TailsNamed)

ASSUME MaxOwn <= Reserve
ASSUME Chunk >= 1

VARIABLES
    free,           \* the queue's free frames, as the transport reports them
    startedBy,      \* fill frames staged and counted this phase, per line (fill_started)
    ownBy,          \* own frames staged and counted this phase, per line (fill_own_used)
    slotOf,         \* each filler's line, fixed for the behaviour
    ownTaken,       \* own frames taken this phase (fill_own_frame cleared)
    ownLeft,        \* own frames main handed the fill and not yet taken
    claims,         \* the claims counter (fill_claims)
    budget,         \* the grant cap (fill_budget)
    pc,             \* each filler's step
    size,           \* the frames a filler's claim asks for
    claim,          \* the claim index a filler holds
    tail,           \* granted frames a filler holds, not yet staged
    uncounted,      \* staged frames a filler has not counted (BatchCount only)
    toRead,         \* the lines the recompute has still to read
    seenS,          \* the recompute's reads: grants staged per line,
    seenO,          \*   own frames used per line,
    seenR,          \*   room
    seenBusy,       \*   and, at the room read, the other fillers in flight
    seenHeld,       \*   and the frames and tails they held
    seenOwn,        \*   and the own frames taken
    starved,        \* a refusal came with room past every tail held (history)
    unnamed,        \* a refusal came with room and no tail held by another (history)
    claimsDone,
    retired,
    phases

vars == <<free, startedBy, ownBy, slotOf, ownTaken, ownLeft, claims, budget, pc,
          size, claim, tail, uncounted, toRead, seenS, seenO, seenR, seenBusy,
          seenHeld, seenOwn, starved, unnamed, claimsDone, retired, phases>>

Steps == {"idle", "sized", "loaded", "claimed", "read1", "read2", "read3",
          "granted", "counting", "next", "ownio", "owncount"}

Max(a, b) == IF a > b THEN a ELSE b

RECURSIVE SumOver(_, _)
SumOver(fn, S) == IF S = {} THEN 0
                  ELSE LET x == CHOOSE x \in S : TRUE IN fn[x] + SumOver(fn, S \ {x})

Started == SumOver(startedBy, Slots)
OwnUsed == SumOver(ownBy, Slots)

\* the room the fill may take above the reserve still held back
Above(r, o) == LET held == IF Reserve > o THEN Reserve - o ELSE 0
               IN IF RoomOnly THEN r ELSE IF r > held THEN r - held ELSE 0

InStep(s) == Cardinality({f \in Fillers : pc[f] = s})
Pending == SumOver(tail, Fillers)               \* granted, not yet staged
Unstaged == SumOver(uncounted, Fillers)
OwnUnstaged == InStep("ownio")                  \* own frame taken, not yet staged
Busy(f) == Cardinality({g \in Fillers \ {f} : pc[g] /= "idle"})
OthersTail(f) == SumOver([g \in Fillers |-> IF g = f THEN 0 ELSE tail[g]], Fillers)
\* another filler's hold on the room: its tail, or one frame while it is in flight
Held(g) == IF pc[g] = "idle" THEN 0 ELSE Max(1, tail[g])
OthersHeld(f) == SumOver([g \in Fillers |-> IF g = f THEN 0 ELSE Held(g)], Fillers)

\* a room of r frames is above what the protocol still needs, with a frame
\* for f and for every other filler in flight now
\* (a filler in flight when the room was read may stage from that room
\* and finish before the decision: it is allowed for from the read on)
\* (an own frame taken after the read was still in the room the read saw:
\* the own frames taken count as they stood at the read)
OwnAtRead(f) == IF ownTaken < seenOwn[f] THEN ownTaken ELSE seenOwn[f]
RoomFor(f, r) == r + OwnAtRead(f) > Reserve + Max(Busy(f), seenBusy[f])
\* the same, past every tail the other fillers hold
RoomPastTails(f, r) == r + OwnAtRead(f) > Reserve + Max(OthersHeld(f), seenHeld[f])

TypeOK ==
    /\ free \in 0..MaxFree
    /\ startedBy \in [Slots -> Nat]
    /\ ownBy \in [Slots -> Nat]
    /\ slotOf \in [Fillers -> Slots]
    /\ ownTaken \in 0..MaxOwn
    /\ ownLeft \in 0..MaxOwn
    /\ claims \in Nat
    /\ budget \in Nat
    /\ pc \in [Fillers -> Steps]
    /\ size \in [Fillers -> 1..Chunk]
    /\ claim \in [Fillers -> Nat]
    /\ tail \in [Fillers -> 0..Chunk]
    /\ uncounted \in [Fillers -> 0..Chunk]
    /\ toRead \in [Fillers -> SUBSET Slots]
    /\ seenS \in [Fillers -> [Slots -> Nat]]
    /\ seenO \in [Fillers -> [Slots -> Nat]]
    /\ seenR \in [Fillers -> Nat]
    /\ seenBusy \in [Fillers -> Nat]
    /\ seenHeld \in [Fillers -> Nat]
    /\ seenOwn \in [Fillers -> Nat]
    /\ starved \in BOOLEAN
    /\ unnamed \in BOOLEAN
    /\ claimsDone \in [Fillers -> 0..MaxClaims]
    /\ retired \in 0..MaxRetire
    /\ phases \in 0..MaxPhases

Zero == [s \in Slots |-> 0]

\* the state just after main opens the first phase
Init ==
    /\ free \in 0..MaxFree
    /\ ownLeft \in 0..MaxOwn
    /\ slotOf \in [Fillers -> Slots]
    /\ startedBy = Zero
    /\ ownBy = Zero
    /\ ownTaken = 0
    /\ claims = 0
    /\ budget = Above(free, 0)
    /\ pc = [f \in Fillers |-> "idle"]
    /\ size = [f \in Fillers |-> 1]
    /\ claim = [f \in Fillers |-> 0]
    /\ tail = [f \in Fillers |-> 0]
    /\ uncounted = [f \in Fillers |-> 0]
    /\ toRead = [f \in Fillers |-> {}]
    /\ seenS = [f \in Fillers |-> Zero]
    /\ seenO = [f \in Fillers |-> Zero]
    /\ seenR = [f \in Fillers |-> 0]
    /\ seenBusy = [f \in Fillers |-> 0]
    /\ seenHeld = [f \in Fillers |-> 0]
    /\ seenOwn = [f \in Fillers |-> MaxOwn]
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
    /\ startedBy' = Zero
    /\ ownBy' = Zero
    /\ ownTaken' = 0
    /\ claims' = 0
    /\ phases' = phases + 1
    /\ claimsDone' = [f \in Fillers |-> 0]
    /\ UNCHANGED <<slotOf, pc, size, claim, tail, uncounted, toRead, seenS, seenO,
                   seenR, seenBusy, seenHeld, seenOwn, starved, unnamed, retired>>

(***************************************************************************)
(* A client's own frame: taken, staged, counted.                           *)
(***************************************************************************)
OwnClaim(f) ==
    /\ pc[f] = "idle"
    /\ claimsDone[f] < MaxClaims
    /\ ownLeft > 0
    /\ ownLeft' = ownLeft - 1
    /\ ownTaken' = ownTaken + 1
    /\ ownBy' = IF OwnCountFirst THEN [ownBy EXCEPT ![slotOf[f]] = @ + 1] ELSE ownBy
    /\ pc' = [pc EXCEPT ![f] = "ownio"]
    /\ claimsDone' = [claimsDone EXCEPT ![f] = @ + 1]
    /\ UNCHANGED <<free, startedBy, slotOf, claims, budget, size, claim, tail,
                   uncounted, toRead, seenS, seenO, seenR, seenBusy, seenHeld, seenOwn, starved, unnamed,
                   retired, phases>>

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
    /\ UNCHANGED <<startedBy, ownBy, slotOf, ownLeft, claims, budget, size, claim,
                   tail, uncounted, toRead, seenS, seenO, seenR, seenBusy, seenHeld, seenOwn, starved, unnamed,
                   claimsDone, retired, phases>>

OwnCount(f) ==
    /\ pc[f] = "owncount"
    /\ ownBy' = [ownBy EXCEPT ![slotOf[f]] = @ + 1]
    /\ pc' = [pc EXCEPT ![f] = "idle"]
    /\ UNCHANGED <<free, startedBy, slotOf, ownTaken, ownLeft, claims, budget, size,
                   claim, tail, uncounted, toRead, seenS, seenO, seenR, seenBusy, seenHeld, seenOwn, starved,
                   unnamed, claimsDone, retired, phases>>

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
    /\ seenBusy' = [seenBusy EXCEPT ![f] = 0]
    /\ seenHeld' = [seenHeld EXCEPT ![f] = 0]
    /\ seenOwn' = [seenOwn EXCEPT ![f] = MaxOwn]
    /\ claimsDone' = [claimsDone EXCEPT ![f] = @ + 1]
    /\ UNCHANGED <<free, startedBy, ownBy, slotOf, ownTaken, ownLeft, claims, budget,
                   claim, tail, uncounted, toRead, seenS, seenO, seenR, starved,
                   unnamed, retired, phases>>

Claim(f) ==
    /\ pc[f] = "sized"
    /\ claim' = [claim EXCEPT ![f] = claims]
    /\ IF SplitClaim
          THEN /\ pc' = [pc EXCEPT ![f] = "loaded"]
               /\ UNCHANGED claims
          ELSE /\ pc' = [pc EXCEPT ![f] = "claimed"]
               /\ claims' = claims + size[f]
    /\ UNCHANGED <<free, startedBy, ownBy, slotOf, ownTaken, ownLeft, budget, size,
                   tail, uncounted, toRead, seenS, seenO, seenR, seenBusy, seenHeld, seenOwn, starved, unnamed,
                   claimsDone, retired, phases>>

ClaimStore(f) ==
    /\ pc[f] = "loaded"
    /\ pc' = [pc EXCEPT ![f] = "claimed"]
    /\ claims' = claim[f] + size[f]
    /\ UNCHANGED <<free, startedBy, ownBy, slotOf, ownTaken, ownLeft, budget, size,
                   claim, tail, uncounted, toRead, seenS, seenO, seenR, seenBusy, seenHeld, seenOwn, starved,
                   unnamed, claimsDone, retired, phases>>

\* the refusal: the claim is taken back whole.  A chunk is claimed again as
\* one frame.  A single claim refused is starved if the room the decision
\* read had a frame for f past the reserve and past every frame and tail
\* the other fillers hold; unnamed if that room had a frame for f past
\* every filler in flight while no other filler held a tail.
Refuse(f, r) ==
    /\ claims' = claims - size[f]
    /\ IF size[f] > 1 /\ ~NoFallback
          THEN /\ pc' = [pc EXCEPT ![f] = "sized"]
               /\ size' = [size EXCEPT ![f] = 1]
               /\ UNCHANGED <<starved, unnamed>>
          ELSE /\ pc' = [pc EXCEPT ![f] = "idle"]
               /\ starved' = (starved \/ RoomPastTails(f, r))
               /\ unnamed' = (unnamed \/ (RoomFor(f, r) /\ OthersTail(f) = 0))
               /\ UNCHANGED size

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
                  /\ UNCHANGED tail

\* the whole claim under the budget: granted; else the recompute begins
Check(f) ==
    /\ pc[f] = "claimed"
    /\ IF claim[f] + size[f] <= budget
          THEN /\ Grant(f)
               /\ UNCHANGED <<claims, starved, unnamed, size, toRead>>
          ELSE IF NoRefresh
                  THEN /\ Refuse(f, free)
                       /\ UNCHANGED <<tail, toRead>>
                  ELSE /\ pc' = [pc EXCEPT ![f] = "read1"]
                       /\ toRead' = [toRead EXCEPT ![f] = Slots]
                       /\ UNCHANGED <<claims, starved, unnamed, size, tail>>
    /\ UNCHANGED <<free, startedBy, ownBy, slotOf, ownTaken, ownLeft, budget, claim,
                   uncounted, seenS, seenO, seenR, seenBusy, seenHeld, seenOwn, claimsDone, retired, phases>>

\* the first read: the lines' two counts, one line at a time (RoomFirst: the room)
Read1(f) ==
    /\ pc[f] = "read1"
    /\ IF RoomFirst
          THEN /\ seenR' = [seenR EXCEPT ![f] = free]
               /\ seenBusy' = [seenBusy EXCEPT ![f] = Busy(f)]
               /\ seenHeld' = [seenHeld EXCEPT ![f] = OthersHeld(f)]
               /\ seenOwn' = [seenOwn EXCEPT ![f] = ownTaken]
               /\ pc' = [pc EXCEPT ![f] = "read2"]
               /\ UNCHANGED <<seenS, seenO, toRead>>
          ELSE \E s \in toRead[f] :
                  /\ seenS' = [seenS EXCEPT ![f][s] = startedBy[s]]
                  /\ seenO' = [seenO EXCEPT ![f][s] = ownBy[s]]
                  /\ toRead' = [toRead EXCEPT ![f] = @ \ {s}]
                  /\ pc' = [pc EXCEPT ![f] = IF toRead[f] \ {s} = {} THEN "read2" ELSE "read1"]
                  /\ UNCHANGED <<seenR, seenBusy, seenHeld, seenOwn>>
    /\ UNCHANGED <<free, startedBy, ownBy, slotOf, ownTaken, ownLeft, claims, budget,
                   size, claim, tail, uncounted, starved, unnamed, claimsDone,
                   retired, phases>>

\* the second read: the room (RoomFirst: the counts, then decide)
Read2(f) ==
    /\ pc[f] = "read2"
    /\ IF RoomFirst
          THEN /\ Decide(f, Started, OwnUsed, seenR[f])
               /\ UNCHANGED <<seenS, seenO, seenR, seenBusy, seenHeld, seenOwn, toRead>>
          ELSE /\ seenR' = [seenR EXCEPT ![f] = free]
               /\ seenBusy' = [seenBusy EXCEPT ![f] = Busy(f)]
               /\ seenHeld' = [seenHeld EXCEPT ![f] = OthersHeld(f)]
               /\ seenOwn' = [seenOwn EXCEPT ![f] = ownTaken]
               /\ UNCHANGED <<seenS, seenO>>
               /\ IF NoReread
                     THEN /\ Decide(f, SumOver(seenS[f], Slots), SumOver(seenO[f], Slots), free)
                          /\ UNCHANGED toRead
                     ELSE /\ pc' = [pc EXCEPT ![f] = "read3"]
                          /\ toRead' = [toRead EXCEPT ![f] = Slots]
                          /\ UNCHANGED <<claims, budget, starved, unnamed, size, tail>>
    /\ UNCHANGED <<free, startedBy, ownBy, slotOf, ownTaken, ownLeft, claim, uncounted,
                   claimsDone, retired, phases>>

\* the counts again, one line at a time: a line moved, start over; all
\* lines read unmoved, decide on the three reads
Read3(f) ==
    /\ pc[f] = "read3"
    /\ \E s \in toRead[f] :
          IF startedBy[s] /= seenS[f][s] \/ ownBy[s] /= seenO[f][s]
             THEN /\ pc' = [pc EXCEPT ![f] = "read1"]
                  /\ toRead' = [toRead EXCEPT ![f] = Slots]
                  /\ UNCHANGED <<claims, budget, starved, unnamed, size, tail>>
             ELSE IF toRead[f] \ {s} = {}
                     THEN /\ Decide(f, SumOver(seenS[f], Slots), SumOver(seenO[f], Slots), seenR[f])
                          /\ toRead' = [toRead EXCEPT ![f] = {}]
                     ELSE /\ toRead' = [toRead EXCEPT ![f] = @ \ {s}]
                          /\ UNCHANGED <<pc, claims, budget, starved, unnamed, size, tail>>
    /\ UNCHANGED <<free, startedBy, ownBy, slotOf, ownTaken, ownLeft, claim, uncounted,
                   seenS, seenO, seenR, seenBusy, seenHeld, seenOwn, claimsDone, retired, phases>>

\* the run ends: what is left of the tail goes back with one add, and
\* (BatchCount) the frames staged are counted only now
EndRun(f) ==
    /\ claims' = IF NoTailReturn THEN claims ELSE claims - tail[f]
    /\ tail' = [tail EXCEPT ![f] = 0]
    /\ startedBy' = [startedBy EXCEPT ![slotOf[f]] = @ + uncounted[f]]
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
               /\ UNCHANGED <<claims, startedBy>>
          ELSE /\ EndRun(f)
               /\ UNCHANGED free
    /\ UNCHANGED <<ownBy, slotOf, ownTaken, ownLeft, budget, size, claim, toRead,
                   seenS, seenO, seenR, seenBusy, seenHeld, seenOwn, starved, unnamed, claimsDone, retired, phases>>

Count(f) ==
    /\ pc[f] = "counting"
    /\ startedBy' = [startedBy EXCEPT ![slotOf[f]] = @ + 1]
    /\ pc' = [pc EXCEPT ![f] = "next"]
    /\ UNCHANGED <<free, ownBy, slotOf, ownTaken, ownLeft, claims, budget, size,
                   claim, tail, uncounted, toRead, seenS, seenO, seenR, seenBusy, seenHeld, seenOwn, starved,
                   unnamed, claimsDone, retired, phases>>

\* the run stages its next frame from the tail, or ends at any frame
Continue(f) ==
    /\ pc[f] = "next"
    /\ tail[f] > 0
    /\ pc' = [pc EXCEPT ![f] = "granted"]
    /\ UNCHANGED <<free, startedBy, ownBy, slotOf, ownTaken, ownLeft, claims, budget,
                   size, claim, tail, uncounted, toRead, seenS, seenO, seenR, seenBusy, seenHeld, seenOwn,
                   starved, unnamed, claimsDone, retired, phases>>

End(f) ==
    /\ pc[f] = "next"
    /\ EndRun(f)
    /\ UNCHANGED <<free, ownBy, slotOf, ownTaken, ownLeft, budget, size, claim,
                   toRead, seenS, seenO, seenR, seenBusy, seenHeld, seenOwn, starved, unnamed, claimsDone,
                   retired, phases>>

\* the transport retires a staged frame (an asynchronous send completes)
Retire ==
    /\ retired < MaxRetire
    /\ free < MaxFree
    /\ free' = free + 1
    /\ retired' = retired + 1
    /\ UNCHANGED <<startedBy, ownBy, slotOf, ownTaken, ownLeft, claims, budget, pc,
                   size, claim, tail, uncounted, toRead, seenS, seenO, seenR, seenBusy, seenHeld, seenOwn,
                   starved, unnamed, claimsDone, phases>>

Done ==
    /\ \A f \in Fillers : pc[f] = "idle"
    /\ UNCHANGED vars

Next ==
    \/ OpenFill \/ Retire \/ Done
    \/ \E f \in Fillers :
          \/ OwnClaim(f) \/ OwnIo(f) \/ OwnCount(f)
          \/ Size(f) \/ Claim(f) \/ ClaimStore(f) \/ Check(f)
          \/ Read1(f) \/ Read2(f) \/ Read3(f)
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
    (Started + Pending + Unstaged + InStep("counting") > 0) =>
        free + ownTaken >= Reserve + OwnUnstaged + Pending

\* the fill is never refused while the room it read had a frame for it
\* past the reserve and every frame and tail the other fillers hold
NeverStarved == ~starved

\* a refusal with room for the frame past every filler in flight comes only
\* while another filler holds a tail: the code names it apart from a full
TailsNamed == ~unnamed

=============================================================================
