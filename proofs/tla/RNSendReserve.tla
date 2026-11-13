---------------------------- MODULE RNSendReserve ----------------------------
(***************************************************************************)
(* The protocol send reserve of one send queue                             *)
(* (sdk/rocketnet_server_transport.cpp: open_fill_budget, fill_claim,      *)
(* claim_frame, queue_budget_now, fill_started, fill_refused;              *)
(* README-RNSendReserve.md)                                                *)
(*                                                                         *)
(* The game's packets and the protocol's own frames share the queue's      *)
(* staging.  Main opens a budget at the end of update and of send_packets: *)
(* the game packets the queue may grant until it is opened again, which is *)
(* the room above the reserve the next update's protocol frames need.      *)
(*                                                                         *)
(* The steps are those of one thread starting a game packet, written so    *)
(* that several threads may take them at once (Fillers): every shared word *)
(* is one atomic, and each read and write is a step of its own.            *)
(*                                                                         *)
(* A game packet's frame goes one of two ways.  A client's own frame (its  *)
(* first frame since the budget opened, once its keep-alive is             *)
(* acknowledged) takes the frame the reserve holds for that client: it is  *)
(* staged, then counted in the own frames used, which lowers the reserve   *)
(* the budget keeps back.  Any other frame claims with one atomic add on   *)
(* the claims.  It is granted when it fits under the budget; else the      *)
(* budget is recomputed from the queue as it reads now (the grants staged  *)
(* and the own frames used, then the room, then those two again, starting  *)
(* over if either moved), raised to that, never lowered, and the claims    *)
(* outstanding tested against it.  The test and the take back of a refused *)
(* claim are two steps: another thread's claim may be the one that put the *)
(* claims past the budget, and it may be taken back in between.  So a      *)
(* refused claim is taken back, and then the claims and the budget are     *)
(* read again: a frame now under the budget is claimed again (the retest), *)
(* else the claim is refused.  Two threads refused together by each        *)
(* other's claims would otherwise both be refused with a frame left under  *)
(* the budget (witness NoRetry).                                           *)
(* A granted frame is staged (it leaves the room) and then counted in the  *)
(* grants staged; a queue with no room refuses it and its claim is taken   *)
(* back.  The transport may retire staged frames at any time (an           *)
(* asynchronous send), which returns room.                                 *)
(*                                                                         *)
(* Properties.  ReserveKept: once a game frame is granted, the room left   *)
(* after every frame in flight has staged still covers the reserve (less   *)
(* the own frames taken).  NeverStarved: no claim is refused while the     *)
(* room its recompute read had a frame for it past the reserve and past    *)
(* every other thread in flight, nor refused for good with a frame left    *)
(* under the budget.                                                       *)
(*                                                                         *)
(* The code is bound to this model through RNSendReserveCalls.tla, whose   *)
(* actions are whole calls (one thread, each call run to its end) and      *)
(* whose trace check runs the code (tests/tla_trace_send_reserve.cpp).     *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Fillers,        \* the threads that start game packets
    Reserve,        \* the queue's protocol reserve, in frames
    MaxFree,        \* the queue's staging, in frames
    MaxOwn,         \* own frames main may hand out in one opening
    MaxClaims,      \* claims per thread in a behaviour
    MaxRetire,      \* frames the transport may retire in a behaviour
    MaxPhases,      \* times main opens the budget in a behaviour
    \* reversed witnesses, each TRUE breaks one rule of the code
    RoomOnly,       \* the budget is the whole room, no reserve           (ReserveKept)
    SplitClaim,     \* the claim is a load then a store, not one add      (ReserveKept)
    RoomFirst,      \* the recompute reads the room before the counts     (ReserveKept)
    OwnCountFirst,  \* an own frame is counted before it is staged        (ReserveKept)
    NoRefresh,      \* the budget stays as main opened it                 (NeverStarved)
    NoReread,       \* the counts are read once, not again after the room (NeverStarved)
    IndexTest,      \* the recompute tests the claim's index, not the claims outstanding (NeverStarved)
    NoRetry         \* a claim taken back is refused, no retest           (NeverStarved)

ASSUME MaxOwn <= Reserve

VARIABLES
    free,           \* the queue's free frames, as the transport reports them
    started,        \* game frames staged and counted since the budget opened (fill_started)
    ownUsed,        \* own frames staged and counted since then (fill_own_used)
    ownTaken,       \* own frames taken since then (fill_own_frame cleared)
    ownLeft,        \* own frames main handed out and not yet taken
    claims,         \* the claims counter (fill_claims)
    budget,         \* the grant cap (fill_budget)
    pc,             \* each thread's step
    claim,          \* the claim index a thread holds
    seenS,          \* the recompute's reads: grants staged,
    seenO,          \*   own frames used,
    seenR,          \*   room
    starved,        \* a refusal came with room past every thread in flight (history)
    claimsDone,
    retired,
    phases

vars == <<free, started, ownUsed, ownTaken, ownLeft, claims, budget, pc, claim,
          seenS, seenO, seenR, starved, claimsDone, retired, phases>>

Steps == {"idle", "loaded", "claimed", "read1", "read2", "read3", "refused",
          "retest", "again", "granted", "counting", "ownio", "owncount"}

Max(a, b) == IF a > b THEN a ELSE b

\* the room game packets may take above the reserve still held back
Above(r, o) == LET held == IF Reserve > o THEN Reserve - o ELSE 0
               IN IF RoomOnly THEN r ELSE IF r > held THEN r - held ELSE 0

InStep(s) == Cardinality({f \in Fillers : pc[f] = s})
Pending == InStep("granted")                    \* granted, not yet staged
OwnUnstaged == InStep("ownio")                  \* own frame taken, not yet staged
Busy(f) == Cardinality({g \in Fillers \ {f} : pc[g] /= "idle"})

\* a room of r frames is above what the protocol still needs, with a frame
\* for f and for every other thread in flight now
RoomFor(f, r) == r + ownTaken > Reserve + Busy(f)

TypeOK ==
    /\ free \in 0..MaxFree
    /\ started \in Nat
    /\ ownUsed \in Nat
    /\ ownTaken \in 0..MaxOwn
    /\ ownLeft \in 0..MaxOwn
    /\ claims \in Nat
    /\ budget \in Nat
    /\ pc \in [Fillers -> Steps]
    /\ claim \in [Fillers -> Nat]
    /\ seenS \in [Fillers -> Nat]
    /\ seenO \in [Fillers -> Nat]
    /\ seenR \in [Fillers -> Nat]
    /\ starved \in BOOLEAN
    /\ claimsDone \in [Fillers -> 0..MaxClaims]
    /\ retired \in 0..MaxRetire
    /\ phases \in 0..MaxPhases

\* the state just after main first opens the budget
Init ==
    /\ free \in 0..MaxFree
    /\ ownLeft \in 0..MaxOwn
    /\ started = 0
    /\ ownUsed = 0
    /\ ownTaken = 0
    /\ claims = 0
    /\ budget = Above(free, 0)
    /\ pc = [f \in Fillers |-> "idle"]
    /\ claim = [f \in Fillers |-> 0]
    /\ seenS = [f \in Fillers |-> 0]
    /\ seenO = [f \in Fillers |-> 0]
    /\ seenR = [f \in Fillers |-> 0]
    /\ starved = FALSE
    /\ claimsDone = [f \in Fillers |-> 0]
    /\ retired = 0
    /\ phases = 1

(***************************************************************************)
(* Main, with no game packet being started: the update's protocol frames   *)
(* take what they need of the room (at most the reserve), then the budget  *)
(* opens again.                                                            *)
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
    /\ UNCHANGED <<pc, claim, seenS, seenO, seenR, starved, retired>>

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
    /\ UNCHANGED <<free, started, claims, budget, claim, seenS, seenO, seenR,
                   starved, retired, phases>>

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
    /\ UNCHANGED <<started, ownUsed, ownLeft, claims, budget, claim, seenS, seenO,
                   seenR, starved, claimsDone, retired, phases>>

OwnCount(f) ==
    /\ pc[f] = "owncount"
    /\ ownUsed' = ownUsed + 1
    /\ pc' = [pc EXCEPT ![f] = "idle"]
    /\ UNCHANGED <<free, started, ownTaken, ownLeft, claims, budget, claim, seenS,
                   seenO, seenR, starved, claimsDone, retired, phases>>

(***************************************************************************)
(* Any other frame: claim, check, recompute; then it is staged and         *)
(* counted.                                                                *)
(***************************************************************************)

\* the claim: one atomic add (SplitClaim: a load, then a store).  A new
\* frame, or the same frame claimed again after its retest ("again")
Claim(f) ==
    /\ \/ /\ pc[f] = "idle"
          /\ claimsDone[f] < MaxClaims
          /\ claimsDone' = [claimsDone EXCEPT ![f] = @ + 1]
       \/ /\ pc[f] = "again"
          /\ UNCHANGED claimsDone
    /\ claim' = [claim EXCEPT ![f] = claims]
    /\ IF SplitClaim
          THEN /\ pc' = [pc EXCEPT ![f] = "loaded"]
               /\ UNCHANGED claims
          ELSE /\ pc' = [pc EXCEPT ![f] = "claimed"]
               /\ claims' = claims + 1
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, budget, seenS, seenO,
                   seenR, starved, retired, phases>>

ClaimStore(f) ==
    /\ pc[f] = "loaded"
    /\ pc' = [pc EXCEPT ![f] = "claimed"]
    /\ claims' = claim[f] + 1
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, budget, claim, seenS,
                   seenO, seenR, starved, claimsDone, retired, phases>>

\* the refusal decided: the claim is still added until its take back, a
\* step of its own.  It is starved if the room the decision read had a
\* frame for f past the reserve and past every other thread in flight.
\* Room that came back after the read, or that a thread claiming after the
\* read takes, is no starvation.
Refuse(f, r) ==
    /\ pc' = [pc EXCEPT ![f] = "refused"]
    /\ starved' = (starved \/ RoomFor(f, r))

\* a claim refused for good, the claims c outstanding after its take back:
\* starved too if a frame was left under the budget (the claims that
\* refused it were taken back before it gave up)
FinalRefuse(f, c) ==
    /\ pc' = [pc EXCEPT ![f] = "idle"]
    /\ starved' = (starved \/ c + 1 <= budget)

\* the take back: the claim leaves the claims, then it is retested
\* (NoRetry: refused)
TakeBack(f) ==
    /\ pc[f] = "refused"
    /\ claims' = claims - 1
    /\ IF NoRetry
          THEN FinalRefuse(f, claims - 1)
          ELSE /\ pc' = [pc EXCEPT ![f] = "retest"]
               /\ UNCHANGED starved
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, budget, claim, seenS,
                   seenO, seenR, claimsDone, retired, phases>>

\* the retest: the claims and the budget read again; a frame under the
\* budget now is claimed again (the claim counts as the same one), else
\* the claim is refused
Retest(f) ==
    /\ pc[f] = "retest"
    /\ IF claims + 1 <= budget
          THEN /\ pc' = [pc EXCEPT ![f] = "again"]
               /\ UNCHANGED starved
          ELSE FinalRefuse(f, claims)
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, claims, budget, claim,
                   seenS, seenO, seenR, claimsDone, retired, phases>>

\* the raise (an atomic max) and the test after it
Decide(f, s, o, r) ==
    LET b2 == Max(budget, s + Above(r, o))
        ok == IF IndexTest THEN claim[f] + 1 <= b2 ELSE claims <= b2
    IN /\ budget' = b2
       /\ IF ok
             THEN /\ pc' = [pc EXCEPT ![f] = "granted"]
                  /\ UNCHANGED starved
             ELSE Refuse(f, r)

\* the claim under the budget: granted; else the recompute begins
Check(f) ==
    /\ pc[f] = "claimed"
    /\ IF claim[f] + 1 <= budget
          THEN /\ pc' = [pc EXCEPT ![f] = "granted"]
               /\ UNCHANGED starved
          ELSE IF NoRefresh
                  THEN Refuse(f, free)
                  ELSE /\ pc' = [pc EXCEPT ![f] = "read1"]
                       /\ UNCHANGED starved
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, claims, budget, claim,
                   seenS, seenO, seenR, claimsDone, retired, phases>>

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
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, claims, budget, claim,
                   starved, claimsDone, retired, phases>>

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
                          /\ UNCHANGED <<budget, starved>>
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, claims, claim,
                   claimsDone, retired, phases>>

\* the counts again: moved, start over; else decide on the three reads
Read3(f) ==
    /\ pc[f] = "read3"
    /\ IF started /= seenS[f] \/ ownUsed /= seenO[f]
          THEN /\ pc' = [pc EXCEPT ![f] = "read1"]
               /\ UNCHANGED <<budget, starved>>
          ELSE Decide(f, seenS[f], seenO[f], seenR[f])
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, claims, claim, seenS,
                   seenO, seenR, claimsDone, retired, phases>>

\* a granted frame is staged: it leaves the room.  A queue with no room
\* refuses it and its claim is taken back.
Stage(f) ==
    /\ pc[f] = "granted"
    /\ IF free > 0
          THEN /\ free' = free - 1
               /\ pc' = [pc EXCEPT ![f] = "counting"]
               /\ UNCHANGED claims
          ELSE /\ claims' = claims - 1
               /\ pc' = [pc EXCEPT ![f] = "idle"]
               /\ UNCHANGED free
    /\ UNCHANGED <<started, ownUsed, ownTaken, ownLeft, budget, claim, seenS, seenO,
                   seenR, starved, claimsDone, retired, phases>>

\* counted only once it has left the room
Count(f) ==
    /\ pc[f] = "counting"
    /\ started' = started + 1
    /\ pc' = [pc EXCEPT ![f] = "idle"]
    /\ UNCHANGED <<free, ownUsed, ownTaken, ownLeft, claims, budget, claim, seenS,
                   seenO, seenR, starved, claimsDone, retired, phases>>

\* the transport retires a staged frame (an asynchronous send completes)
Retire ==
    /\ retired < MaxRetire
    /\ free < MaxFree
    /\ free' = free + 1
    /\ retired' = retired + 1
    /\ UNCHANGED <<started, ownUsed, ownTaken, ownLeft, claims, budget, pc, claim,
                   seenS, seenO, seenR, starved, claimsDone, phases>>

Done ==
    /\ \A f \in Fillers : pc[f] = "idle"
    /\ UNCHANGED vars

Next ==
    \/ OpenFill \/ Retire \/ Done
    \/ \E f \in Fillers :
          \/ OwnClaim(f) \/ OwnIo(f) \/ OwnCount(f)
          \/ Claim(f) \/ ClaimStore(f) \/ Check(f)
          \/ Read1(f) \/ Read2(f) \/ Read3(f) \/ TakeBack(f) \/ Retest(f)
          \/ Stage(f) \/ Count(f)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Properties                                                              *)
(***************************************************************************)

\* once a game frame is granted, the room left after every frame in flight
\* has staged covers the reserve, less the own frames taken (their clients'
\* keep-alives are not due): the next update's protocol frames are never
\* refused for the game packets' sake
ReserveKept ==
    (started + Pending + InStep("counting") > 0) =>
        free + ownTaken >= Reserve + OwnUnstaged + Pending

\* a game packet is never refused while the room it read had a frame for it
\* past the reserve and every thread in flight, nor refused for good while
\* the claims left after its take back leave a frame for it under the budget
NeverStarved == ~starved

=============================================================================
