---------------------------- MODULE RNSendReserve ----------------------------
(***************************************************************************)
(* The protocol send reserve of one send queue                             *)
(* (sdk/rocketnet_server_transport.cpp: open_fill_budget, fill_claim,      *)
(* queue_budget_now, refresh_fill_budget, claim_frames, fill_started,      *)
(* fill_refused, return_fill_tails; README-RNSendReserve.md)               *)
(*                                                                         *)
(* The game's fillers and the protocol share the queue's staging.  Main    *)
(* opens a fill phase with a budget, the grants the fill may take: the     *)
(* room above the reserve the next update's protocol frames need.          *)
(*                                                                         *)
(* The words, at the code's layout.  The claims (fill_claims), the budget  *)
(* (fill_budget) and the own frames used (fill_own_used) are one word      *)
(* each.  The grants staged are one counter on each fill line              *)
(* (fill_lines[i].started), and a recompute reads their sum one line at a  *)
(* time.  The tails are one word on each line (fill_lines[i].tails, the    *)
(* frames granted and not yet taken, and the chunk claims in flight): a    *)
(* line is shared by every filler whose thread hashes to it (SharedLine:   *)
(* all on one line; else each on its own) and holds its tail between       *)
(* calls, while its fillers are idle.  Every load, store, add, max and     *)
(* compare-and-swap of a shared word is a step of its own, but for six     *)
(* steps that each join two of the code's atomic operations (the           *)
(* README's "Steps and the code" says which, and what each join costs).    *)
(*                                                                         *)
(* A start, from idle.  A client's own frame (its first frame this phase,  *)
(* once its keep-alive is acknowledged) is taken first, whatever its line  *)
(* holds: staged, then counted in the own frames used, which lowers the    *)
(* reserve the budget keeps back.  Else a frame of the line's tail, taken  *)
(* with one compare-and-swap (Continue).  Else a claim with one atomic add *)
(* on the claims: a chunk of Chunk frames when the budget read less the    *)
(* claims read after it is at least Chunk x GuardWidth (the guard, two     *)
(* loads), else one frame.  It is granted when the whole claim fits under  *)
(* the budget; else the budget is recomputed from the queue as it reads    *)
(* now (the lines' grants summed and the own frames used, then the room,   *)
(* then those two again, starting over if either moved), raised to that   *)
(* by an atomic max, never lowered, and the claims outstanding tested      *)
(* against the budget loaded after the max.  A granted chunk's first frame *)
(* is this start's, the rest goes on the line's tail.  A refused chunk is  *)
(* taken back whole and claimed again as one frame; a refused single claim *)
(* is taken back, then the claims are loaded and then the budget: a frame  *)
(* now under the budget is claimed again (the retest), else the claim is   *)
(* refused, and the code reads every line's tails and chunk claims, one    *)
(* line at a time, to pick the counter (SEND_RESERVE_REFUSED_TAILS_HELD    *)
(* when any is held, else SEND_RESERVE_REFUSED).  A granted frame is       *)
(* staged (it leaves the room) and counted on the filler's line; the       *)
(* transport may refuse it at any time, room or not, and then the filler   *)
(* takes its line's whole tail with one compare-and-swap and gives it back *)
(* with the frame's claim in one add (End).  Main returns every tail when  *)
(* it opens the next phase.  The transport may retire staged frames at any *)
(* time (an asynchronous send), which returns room.                        *)
(*                                                                         *)
(* Properties.  ReserveKept: once the fill holds a frame, the room left    *)
(* after every frame in flight has staged still covers the reserve (less   *)
(* the own frames taken).  NeverStarved: no single claim is refused while  *)
(* the room its recompute read had a frame for it past the reserve, past   *)
(* what the other fillers held at that read (a claim added, a frame        *)
(* granted and not yet staged or counted, the claims being given back, an  *)
(* own frame not yet staged), past every line's tail, and past what they   *)
(* claimed since; nor refused for good while the claims its retest loaded  *)
(* leave a frame under the budget.  TailsNamed: a refusal the code counts  *)
(* as SEND_RESERVE_REFUSED (its read of the lines found no tail and no     *)
(* chunk claim) had no room, at the decision that refused it, for its      *)
(* frame past the reserve and every filler in flight: the plain counter is *)
(* a true full, the tails' refusals counted apart.  The code breaks it     *)
(* (MCRNSendReserveCodeTailsNamed.cfg): it reads the lines after the       *)
(* decision, when held frames may be on no line word or already staged.    *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Fillers,        \* the fill threads
    Reserve,        \* the queue's protocol reserve, in frames
    MaxFree,        \* the queue's staging, in frames
    MaxOwn,         \* own frames main may hand the fill in one phase
    MaxClaims,      \* starts per filler in a phase
    MaxRetire,      \* frames the transport may retire in a behaviour
    MaxPhases,      \* fill phases main opens in a behaviour
    Chunk,          \* frames per chunk claim (1: every claim is one frame)
    GuardWidth,     \* the guard's line bound (0: no guard)
    SharedLine,     \* TRUE: every filler's thread hashes to one fill line
    \* reversed witnesses, each TRUE breaks one rule of the code
    RoomOnly,       \* the budget is the whole room, no reserve           (ReserveKept)
    SplitClaim,     \* the claim is a load then a store, not one add      (ReserveKept)
    RoomFirst,      \* the recompute reads the room before the counts     (ReserveKept)
    OwnCountFirst,  \* an own frame is counted before it is staged        (ReserveKept)
    NoRefresh,      \* the budget stays as main opened it                 (NeverStarved)
    NoReread,       \* the counts are read once, not again after the room (NeverStarved)
    IndexTest,      \* the recompute tests the claim's index, not the claims outstanding (NeverStarved)
    NoFallback,     \* a refused chunk is not claimed again as one frame  (NeverStarved)
    BatchCount,     \* a staged frame is counted at the filler's next start (NeverStarved)
    NoTailReturn,   \* a refused frame takes the line's tail but gives back its own claim only (NeverStarved)
    NoRetry         \* a single claim taken back is refused, no retest    (NeverStarved)

ASSUME MaxOwn <= Reserve
ASSUME Chunk >= 1

\* the fill lines, and the one each filler's thread hashes to
FillerIndex == CHOOSE m \in [Fillers -> 1..Cardinality(Fillers)] :
                   \A f, g \in Fillers : f /= g => m[f] /= m[g]
L == IF SharedLine THEN 1 ELSE Cardinality(Fillers)
Lines == 1..L
LineOf(f) == IF SharedLine THEN 1 ELSE FillerIndex[f]

VARIABLES
    free,           \* the queue's free frames, as the transport reports them
    started,        \* grants staged and counted this phase, each line (fill_lines[i].started)
    ownUsed,        \* own frames staged and counted this phase (fill_own_used)
    ownTaken,       \* own frames taken this phase and not refused (fill_own_frame cleared)
    ownLeft,        \* own frames main handed the fill and not yet taken
    claims,         \* the claims counter (fill_claims)
    budget,         \* the grant cap (fill_budget)
    tail,           \* granted frames not yet taken, each line (the low half of fill_lines[i].tails)
    pc,             \* each filler's step
    size,           \* the frames a filler's claim asks for
    claim,          \* the claim index a filler's add returned
    ret,            \* frames a filler took from its line and has not given back to the claims
    uncounted,      \* staged frames a filler has not counted (BatchCount only)
    ld,             \* a filler's last load of the budget (guard, after the max) or the claims (retest)
    rpos,           \* lines a filler's running sum has read
    racc,           \* that running sum (grants staged, or tails and chunk claims)
    seenS,          \* the recompute's reads: grants staged,
    seenO,          \*   own frames used,
    seenR,          \*   room, with ownTaken, every other filler's hold and the lines' tails at that read
    roomy,          \* the decision that refused a filler's single claim read room for it past every filler in flight
    starved,        \* history: a refusal NeverStarved forbids
    unnamed,        \* history: a refusal TailsNamed forbids
    claimsDone,
    retired,
    phases

vars == <<free, started, ownUsed, ownTaken, ownLeft, claims, budget, tail, pc, size,
          claim, ret, uncounted, ld, rpos, racc, seenS, seenO, seenR, roomy, starved,
          unnamed, claimsDone, retired, phases>>

Steps == {"idle", "sizing", "sized", "loaded", "claimed", "read1", "read2", "read3",
          "raise", "test", "refused", "retest", "retestb", "naming", "granted",
          "counting", "returning", "ownio", "owncount"}

Max(a, b) == IF a > b THEN a ELSE b

RECURSIVE SumOver(_, _)
SumOver(fn, S) == IF S = {} THEN 0
                  ELSE LET x == CHOOSE x \in S : TRUE IN fn[x] + SumOver(fn, S \ {x})

\* the room the fill may take above the reserve still held back
Above(r, o) == LET held == IF Reserve > o THEN Reserve - o ELSE 0
               IN IF RoomOnly THEN r ELSE IF r > held THEN r - held ELSE 0

InStep(s) == Cardinality({f \in Fillers : pc[f] = s})
Tails == SumOver(tail, Lines)                   \* granted, not yet taken
Pending == Tails + InStep("granted")            \* granted, not yet staged
Unstaged == SumOver(uncounted, Fillers)         \* staged, not yet counted (BatchCount)
OwnUnstaged == InStep("ownio")                  \* own frame taken, not yet staged
Busy(f) == Cardinality({g \in Fillers \ {f} : pc[g] /= "idle"})

\* a filler's chunk claim, from the guard that sized it (the code adds it to
\* its line's high half then) until it is granted or taken back
Checking == {"sized", "loaded", "claimed", "read1", "read2", "read3", "raise", "test", "refused"}
ChunkHold(g) == IF pc[g] \in Checking /\ size[g] > 1 THEN size[g] ELSE 0
\* what the code's tails_held reads on line l: its tail and its chunk claims
TailsOn(l) == tail[l] + SumOver([g \in Fillers |-> IF LineOf(g) = l THEN ChunkHold(g) ELSE 0], Fillers)

\* another filler's hold on the room: the claim it has added and not taken
\* back, its granted frame not yet staged, the frame it staged and has not
\* counted (the budget's reads miss it), the claims it is giving back, its
\* own frame taken and not yet staged or counted.  A filler with no claim holds
\* nothing: what it claims after a read is counted then (Acquired)
Held(g) == CASE pc[g] \in {"claimed", "read1", "read2", "read3", "raise", "test", "refused"} -> size[g]
             [] pc[g] \in {"granted", "counting", "ownio", "owncount"} -> 1
             [] pc[g] = "returning" -> ret[g]
             [] OTHER -> 0

\* the room read (Read2) and, at that same step, the own frames taken, the
\* other fillers in flight and their holds, and the lines' tails: frames
\* held then are inside that room.  a counts the frames the other fillers
\* claimed since (a claim's add), up to MaxFree + 1,
\* past which no room read can have them
Snap(f) == [r |-> free, o |-> ownTaken, b |-> Busy(f), t |-> Tails, a |-> 0,
            h |-> SumOver([g \in Fillers |-> IF g = f THEN 0 ELSE Held(g)], Fillers)]
NoSnap == [r |-> 0, o |-> 0, b |-> 0, t |-> 0, a |-> 0, h |-> 0]
Window == {"read1", "read3", "raise", "test"}   \* after the room read, before the decision
Acquired(g, n) == [f \in Fillers |-> IF f /= g /\ pc[f] \in Window
                                        THEN [seenR[f] EXCEPT !.a = IF @ + n > MaxFree THEN MaxFree + 1 ELSE @ + n]
                                        ELSE seenR[f]]

\* a decision on the room s.r read with s: that room was above what the
\* protocol still needed (the reserve less the own frames taken), with a
\* frame for f, one for every other filler in flight at the read, and one
\* for every frame claimed since
RoomFor(f, s) == s.r + s.o > Reserve + s.b + s.a
\* the same, past every other filler's hold and the lines' tails at the
\* read and every frame claimed since: room given back after the read is
\* no starvation, nor is room a filler claiming since takes
RoomPastTails(f, s) == s.r + s.o > Reserve + s.h + s.t + s.a

TypeOK ==
    /\ free \in 0..MaxFree
    /\ started \in [Lines -> Nat]
    /\ ownUsed \in Nat
    /\ ownTaken \in 0..MaxOwn
    /\ ownLeft \in 0..MaxOwn
    /\ claims \in Nat
    /\ budget \in Nat
    /\ tail \in [Lines -> Nat]
    /\ pc \in [Fillers -> Steps]
    /\ size \in [Fillers -> 1..Chunk]
    /\ claim \in [Fillers -> Nat]
    /\ ret \in [Fillers -> Nat]
    /\ uncounted \in [Fillers -> Nat]
    /\ ld \in [Fillers -> Nat]
    /\ rpos \in [Fillers -> 0..L]
    /\ racc \in [Fillers -> Nat]
    /\ seenS \in [Fillers -> Nat]
    /\ seenO \in [Fillers -> Nat]
    /\ seenR \in [Fillers -> [r : Nat, o : Nat, b : Nat, t : Nat, a : Nat, h : Nat]]
    /\ roomy \in [Fillers -> BOOLEAN]
    /\ starved \in BOOLEAN
    /\ unnamed \in BOOLEAN
    /\ claimsDone \in [Fillers -> 0..MaxClaims]
    /\ retired \in 0..MaxRetire
    /\ phases \in 0..MaxPhases

\* the state just after main opens the first phase
Init ==
    /\ free \in 0..MaxFree
    /\ ownLeft \in 0..MaxOwn
    /\ started = [l \in Lines |-> 0]
    /\ ownUsed = 0
    /\ ownTaken = 0
    /\ claims = 0
    /\ budget = Above(free, 0)
    /\ tail = [l \in Lines |-> 0]
    /\ pc = [f \in Fillers |-> "idle"]
    /\ size = [f \in Fillers |-> 1]
    /\ claim = [f \in Fillers |-> 0]
    /\ ret = [f \in Fillers |-> 0]
    /\ uncounted = [f \in Fillers |-> 0]
    /\ ld = [f \in Fillers |-> 0]
    /\ rpos = [f \in Fillers |-> 0]
    /\ racc = [f \in Fillers |-> 0]
    /\ seenS = [f \in Fillers |-> 0]
    /\ seenO = [f \in Fillers |-> 0]
    /\ seenR = [f \in Fillers |-> NoSnap]
    /\ roomy = [f \in Fillers |-> FALSE]
    /\ starved = FALSE
    /\ unnamed = FALSE
    /\ claimsDone = [f \in Fillers |-> 0]
    /\ retired = 0
    /\ phases = 1

(***************************************************************************)
(* Main, with no fill running: the update's protocol frames take what      *)
(* they need of the room (at most the reserve), every line's tail goes     *)
(* back and its grants staged are zeroed, then the next phase opens.       *)
(***************************************************************************)
OpenFill ==
    /\ phases < MaxPhases
    /\ \A f \in Fillers : pc[f] = "idle"
    /\ \E used \in 0..(IF free < Reserve THEN free ELSE Reserve), own \in 0..MaxOwn :
          /\ free' = free - used
          /\ budget' = Above(free - used, 0)
          /\ ownLeft' = own
    /\ started' = [l \in Lines |-> 0]
    /\ tail' = [l \in Lines |-> 0]
    /\ uncounted' = [f \in Fillers |-> 0]
    /\ ownUsed' = 0
    /\ ownTaken' = 0
    /\ claims' = 0
    /\ phases' = phases + 1
    /\ claimsDone' = [f \in Fillers |-> 0]
    /\ UNCHANGED <<pc, size, claim, ret, ld, rpos, racc, seenS, seenO, seenR, roomy,
                   starved, unnamed, retired>>

\* a start begins: what a BatchCount filler staged last is counted now
Flush(f) ==
    /\ started' = [started EXCEPT ![LineOf(f)] = @ + uncounted[f]]
    /\ uncounted' = [uncounted EXCEPT ![f] = 0]

(***************************************************************************)
(* A client's own frame: taken (whatever the filler's line holds), staged, *)
(* counted.                                                                *)
(***************************************************************************)
OwnClaim(f) ==
    /\ pc[f] = "idle"
    /\ claimsDone[f] < MaxClaims
    /\ ownLeft > 0
    /\ Flush(f)
    /\ ownLeft' = ownLeft - 1
    /\ ownTaken' = ownTaken + 1
    /\ ownUsed' = IF OwnCountFirst THEN ownUsed + 1 ELSE ownUsed
    /\ pc' = [pc EXCEPT ![f] = "ownio"]
    /\ claimsDone' = [claimsDone EXCEPT ![f] = @ + 1]
    /\ UNCHANGED <<free, claims, budget, tail, size, claim, ret, ld, rpos, racc, seenS,
                   seenO, seenR, roomy, starved, unnamed, retired, phases>>

\* staged; or the transport refuses it (no room, or its own reason): the
\* client's keep-alive stays due and its frame stays in the reserve
OwnIo(f) ==
    /\ pc[f] = "ownio"
    /\ \/ /\ free > 0
          /\ free' = free - 1
          /\ pc' = [pc EXCEPT ![f] = IF OwnCountFirst THEN "idle" ELSE "owncount"]
          /\ UNCHANGED ownTaken
       \/ /\ pc' = [pc EXCEPT ![f] = "idle"]
          /\ ownTaken' = ownTaken - 1
          /\ UNCHANGED free
    /\ UNCHANGED <<started, ownUsed, ownLeft, claims, budget, tail, size, claim, ret,
                   uncounted, ld, rpos, racc, seenS, seenO, seenR, roomy, starved,
                   unnamed, claimsDone, retired, phases>>

OwnCount(f) ==
    /\ pc[f] = "owncount"
    /\ ownUsed' = ownUsed + 1
    /\ pc' = [pc EXCEPT ![f] = "idle"]
    /\ UNCHANGED <<free, started, ownTaken, ownLeft, claims, budget, tail, size, claim,
                   ret, uncounted, ld, rpos, racc, seenS, seenO, seenR, roomy, starved,
                   unnamed, claimsDone, retired, phases>>

(***************************************************************************)
(* A frame of the line's tail: one compare-and-swap takes it, with no      *)
(* write to the claims (fill_claim's loop on the line's word).             *)
(***************************************************************************)
Continue(f) ==
    /\ pc[f] = "idle"
    /\ claimsDone[f] < MaxClaims
    /\ tail[LineOf(f)] > 0
    /\ Flush(f)
    /\ tail' = [tail EXCEPT ![LineOf(f)] = @ - 1]
    /\ pc' = [pc EXCEPT ![f] = "granted"]
    /\ claimsDone' = [claimsDone EXCEPT ![f] = @ + 1]
    /\ UNCHANGED <<free, ownUsed, ownTaken, ownLeft, claims, budget, size, claim, ret,
                   ld, rpos, racc, seenS, seenO, seenR, roomy, starved, unnamed, retired,
                   phases>>

(***************************************************************************)
(* Any other frame: size, claim, check, recompute, decide; then stage and  *)
(* count, or give back.                                                    *)
(***************************************************************************)

\* the guard, on a line whose tail is empty: the budget loaded, then the
\* claims; a chunk while the budget leaves Chunk frames for every line the
\* guard allows for past the claims, else one frame (Chunk 1: always one)
Size(f) ==
    /\ \/ /\ pc[f] = "idle"
          /\ claimsDone[f] < MaxClaims
          /\ tail[LineOf(f)] = 0
          /\ Flush(f)
          /\ claimsDone' = [claimsDone EXCEPT ![f] = @ + 1]
          /\ IF Chunk = 1
                THEN /\ pc' = [pc EXCEPT ![f] = "sized"]
                     /\ size' = [size EXCEPT ![f] = 1]
                     /\ UNCHANGED ld
                ELSE /\ pc' = [pc EXCEPT ![f] = "sizing"]
                     /\ ld' = [ld EXCEPT ![f] = budget]
                     /\ UNCHANGED size
       \/ /\ pc[f] = "sizing"
          /\ size' = [size EXCEPT ![f] = IF ld[f] >= claims + Chunk * GuardWidth THEN Chunk ELSE 1]
          /\ pc' = [pc EXCEPT ![f] = "sized"]
          /\ ld' = [ld EXCEPT ![f] = 0]
          /\ UNCHANGED <<started, uncounted, claimsDone>>
    /\ UNCHANGED <<free, ownUsed, ownTaken, ownLeft, claims, budget, tail, claim, ret,
                   rpos, racc, seenS, seenO, seenR, roomy, starved, unnamed, retired,
                   phases>>

Claim(f) ==
    /\ pc[f] = "sized"
    /\ claim' = [claim EXCEPT ![f] = claims]
    /\ IF SplitClaim
          THEN /\ pc' = [pc EXCEPT ![f] = "loaded"]
               /\ UNCHANGED claims
          ELSE /\ pc' = [pc EXCEPT ![f] = "claimed"]
               /\ claims' = claims + size[f]
    /\ seenR' = Acquired(f, size[f])
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, budget, tail, size, ret,
                   uncounted, ld, rpos, racc, seenS, seenO, roomy, starved,
                   unnamed, claimsDone, retired, phases>>

ClaimStore(f) ==
    /\ pc[f] = "loaded"
    /\ pc' = [pc EXCEPT ![f] = "claimed"]
    /\ claims' = claim[f] + size[f]
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, budget, tail, size, claim,
                   ret, uncounted, ld, rpos, racc, seenS, seenO, seenR, roomy, starved,
                   unnamed, claimsDone, retired, phases>>

\* granted: this start's frame, and the rest of a chunk onto the line's
\* tail (one add on the line's word, which also drops the chunk claim)
Grant(f) ==
    /\ pc' = [pc EXCEPT ![f] = "granted"]
    /\ tail' = [tail EXCEPT ![LineOf(f)] = @ + size[f] - 1]
    /\ roomy' = [roomy EXCEPT ![f] = FALSE]

\* the refusal decided, on the room its recompute read (s): the claim is
\* still added until its take back, a step of its own.  A single claim
\* refused is starved if that room had a frame for f past the reserve and
\* past every hold of the other fillers and of the lines; and the decision
\* remembers whether it had a frame for f past every filler in flight,
\* which the counter the code picks later must name.  Room that came back
\* after the read, or that a filler claiming after the read takes, is no
\* starvation.
RefuseAt(f, s) ==
    /\ pc' = [pc EXCEPT ![f] = "refused"]
    /\ IF size[f] > 1 /\ ~NoFallback
          THEN UNCHANGED <<starved, roomy>>
          ELSE /\ starved' = (starved \/ RoomPastTails(f, s))
               /\ roomy' = [roomy EXCEPT ![f] = RoomFor(f, s)]

\* the whole claim under the budget loaded now: granted; else the
\* recompute begins
Check(f) ==
    /\ pc[f] = "claimed"
    /\ IF claim[f] + size[f] <= budget
          THEN /\ Grant(f)
               /\ UNCHANGED <<starved, seenR>>
          ELSE IF NoRefresh
                  THEN /\ RefuseAt(f, Snap(f))
                       /\ UNCHANGED <<tail, seenR>>
                  ELSE /\ pc' = [pc EXCEPT ![f] = IF RoomFirst THEN "read2" ELSE "read1"]
                       /\ UNCHANGED <<tail, starved, roomy, seenR>>
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, claims, budget, size,
                   claim, ret, uncounted, ld, rpos, racc, seenS, seenO, unnamed,
                   claimsDone, retired, phases>>

\* the first read of the counts: each line's grants staged, one line a
\* step, then the own frames used (RoomFirst: after the room, and then the
\* decision)
Read1(f) ==
    /\ pc[f] = "read1"
    /\ IF rpos[f] < L
          THEN /\ racc' = [racc EXCEPT ![f] = @ + started[rpos[f] + 1]]
               /\ rpos' = [rpos EXCEPT ![f] = @ + 1]
               /\ UNCHANGED <<pc, seenS, seenO>>
          ELSE /\ seenS' = [seenS EXCEPT ![f] = racc[f]]
               /\ seenO' = [seenO EXCEPT ![f] = ownUsed]
               /\ racc' = [racc EXCEPT ![f] = 0]
               /\ rpos' = [rpos EXCEPT ![f] = 0]
               /\ pc' = [pc EXCEPT ![f] = IF RoomFirst THEN "raise" ELSE "read2"]
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, claims, budget, tail, size,
                   claim, ret, uncounted, ld, seenR, roomy, starved, unnamed, claimsDone,
                   retired, phases>>

\* the room (RoomFirst: before the counts)
Read2(f) ==
    /\ pc[f] = "read2"
    /\ seenR' = [seenR EXCEPT ![f] = Snap(f)]
    /\ pc' = [pc EXCEPT ![f] = IF RoomFirst THEN "read1" ELSE IF NoReread THEN "raise" ELSE "read3"]
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, claims, budget, tail, size,
                   claim, ret, uncounted, ld, rpos, racc, seenS, seenO, roomy, starved,
                   unnamed, claimsDone, retired, phases>>

\* the counts again, the lines one a step then the own frames used: either
\* moved, start over; else on to the raise.  Each line only grows within a
\* phase, so two sums that are equal saw no line move between its reads.
Read3(f) ==
    /\ pc[f] = "read3"
    /\ IF rpos[f] < L
          THEN /\ racc' = [racc EXCEPT ![f] = @ + started[rpos[f] + 1]]
               /\ rpos' = [rpos EXCEPT ![f] = @ + 1]
               /\ UNCHANGED pc
          ELSE /\ racc' = [racc EXCEPT ![f] = 0]
               /\ rpos' = [rpos EXCEPT ![f] = 0]
               /\ pc' = [pc EXCEPT ![f] = IF racc[f] /= seenS[f] \/ ownUsed /= seenO[f]
                                             THEN "read1" ELSE "raise"]
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, claims, budget, tail, size,
                   claim, ret, uncounted, ld, seenS, seenO, seenR, roomy, starved,
                   unnamed, claimsDone, retired, phases>>

\* the atomic max of the budget with what the reads allow; then the test:
\* the claims outstanding against the budget loaded after the max.  The
\* code loads the budget and then the claims; the budget only rises, so
\* the budget it loaded is one of the values from the max's result to the
\* budget now, and the step tests the claims now against any of them
\* (IndexTest: the claim's index instead of the claims)
Ok(f, b) == IF IndexTest THEN claim[f] + size[f] <= b ELSE claims <= b

Decide(f) ==
    \/ /\ pc[f] = "raise"
       /\ LET b2 == Max(budget, seenS[f] + Above(seenR[f].r, seenO[f]))
          IN /\ budget' = b2
             /\ ld' = [ld EXCEPT ![f] = b2]
       /\ pc' = [pc EXCEPT ![f] = "test"]
       /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, claims, tail, size,
                      claim, ret, uncounted, rpos, racc, seenS, seenO, seenR, roomy,
                      starved, unnamed, claimsDone, retired, phases>>
    \/ /\ pc[f] = "test"
       /\ \/ /\ Ok(f, budget)
             /\ Grant(f)
             /\ UNCHANGED starved
          \/ /\ ~Ok(f, ld[f])
             /\ RefuseAt(f, seenR[f])
             /\ UNCHANGED tail
       /\ ld' = [ld EXCEPT ![f] = 0]
       /\ seenS' = [seenS EXCEPT ![f] = 0]
       /\ seenO' = [seenO EXCEPT ![f] = 0]
       /\ seenR' = [seenR EXCEPT ![f] = NoSnap]
       /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, claims, budget, size,
                      claim, ret, uncounted, rpos, racc, unnamed, claimsDone, retired,
                      phases>>

\* a claim refused for good, the claims c its last load saw: starved too if
\* those leave a frame for it under the budget; then the code reads the
\* lines to pick its counter
FinalRefuse(f, c) ==
    /\ pc' = [pc EXCEPT ![f] = "naming"]
    /\ starved' = (starved \/ c + size[f] <= budget)

\* the take back: the claim leaves the claims whole.  A chunk is claimed
\* again as one frame; a single claim is retested (NoRetry: refused)
TakeBack(f) ==
    /\ pc[f] = "refused"
    /\ claims' = claims - size[f]
    /\ IF size[f] > 1 /\ ~NoFallback
          THEN /\ pc' = [pc EXCEPT ![f] = "sized"]
               /\ size' = [size EXCEPT ![f] = 1]
               /\ UNCHANGED starved
          ELSE IF NoRetry
                  THEN /\ FinalRefuse(f, claims - size[f])
                       /\ UNCHANGED size
                  ELSE /\ pc' = [pc EXCEPT ![f] = "retest"]
                       /\ UNCHANGED <<starved, size>>
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, budget, tail, claim, ret,
                   uncounted, ld, rpos, racc, seenS, seenO, seenR, roomy, unnamed,
                   claimsDone, retired, phases>>

\* the retest: the claims loaded, then the budget; a frame under the budget
\* is claimed again (the claim counts as the same start), else the claim is
\* refused.  Then the counter: every line's tail and chunk claims read, one
\* line a step; none read is SEND_RESERVE_REFUSED, which TailsNamed holds to
\* a decision that had no room for the frame past every filler in flight
Retest(f) ==
    \/ /\ pc[f] = "retest"
       /\ ld' = [ld EXCEPT ![f] = claims]
       /\ pc' = [pc EXCEPT ![f] = "retestb"]
       /\ UNCHANGED <<starved, unnamed, roomy, rpos, racc>>
    \/ /\ pc[f] = "retestb"
       /\ ld' = [ld EXCEPT ![f] = 0]
       /\ IF ld[f] + size[f] <= budget
             THEN /\ pc' = [pc EXCEPT ![f] = "sized"]
                  /\ UNCHANGED starved
             ELSE FinalRefuse(f, ld[f])
       /\ UNCHANGED <<unnamed, roomy, rpos, racc>>
    \/ /\ pc[f] = "naming"
       /\ LET acc == racc[f] + TailsOn(rpos[f] + 1)
          IN IF rpos[f] + 1 < L
                THEN /\ racc' = [racc EXCEPT ![f] = acc]
                     /\ rpos' = [rpos EXCEPT ![f] = @ + 1]
                     /\ UNCHANGED <<pc, unnamed, roomy>>
                ELSE /\ unnamed' = (unnamed \/ (roomy[f] /\ acc = 0))
                     /\ racc' = [racc EXCEPT ![f] = 0]
                     /\ rpos' = [rpos EXCEPT ![f] = 0]
                     /\ roomy' = [roomy EXCEPT ![f] = FALSE]
                     /\ pc' = [pc EXCEPT ![f] = "idle"]
       /\ UNCHANGED <<ld, starved>>
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, claims, budget, tail, size,
                   claim, ret, uncounted, seenS, seenO, seenR, claimsDone, retired, phases>>

\* a granted frame is staged: it leaves the room, and is counted next
\* (BatchCount: at the filler's next start).  Or the transport refuses it,
\* room or not: the filler takes its line's whole tail with one
\* compare-and-swap, to give it back with the frame's claim (End)
Stage(f) ==
    /\ pc[f] = "granted"
    /\ \/ /\ free > 0
          /\ free' = free - 1
          /\ IF BatchCount
                THEN /\ uncounted' = [uncounted EXCEPT ![f] = @ + 1]
                     /\ pc' = [pc EXCEPT ![f] = "idle"]
                ELSE /\ pc' = [pc EXCEPT ![f] = "counting"]
                     /\ UNCHANGED uncounted
          /\ UNCHANGED <<tail, ret>>
       \/ /\ ret' = [ret EXCEPT ![f] = IF NoTailReturn THEN 1 ELSE tail[LineOf(f)] + 1]
          /\ tail' = [tail EXCEPT ![LineOf(f)] = 0]
          /\ pc' = [pc EXCEPT ![f] = "returning"]
          /\ UNCHANGED <<free, uncounted>>
    /\ UNCHANGED <<started, ownUsed, ownTaken, ownLeft, claims, budget, size, claim, ld,
                   rpos, racc, seenS, seenO, seenR, roomy, starved, unnamed, claimsDone,
                   retired, phases>>

Count(f) ==
    /\ pc[f] = "counting"
    /\ started' = [started EXCEPT ![LineOf(f)] = @ + 1]
    /\ pc' = [pc EXCEPT ![f] = "idle"]
    /\ UNCHANGED <<free, ownUsed, ownTaken, ownLeft, claims, budget, tail, size, claim,
                   ret, uncounted, ld, rpos, racc, seenS, seenO, seenR, roomy, starved,
                   unnamed, claimsDone, retired, phases>>

\* the frame's claim and the tail it took leave the claims in one add
End(f) ==
    /\ pc[f] = "returning"
    /\ claims' = claims - ret[f]
    /\ ret' = [ret EXCEPT ![f] = 0]
    /\ pc' = [pc EXCEPT ![f] = "idle"]
    /\ UNCHANGED <<free, started, ownUsed, ownTaken, ownLeft, budget, tail, size, claim,
                   uncounted, ld, rpos, racc, seenS, seenO, seenR, roomy, starved,
                   unnamed, claimsDone, retired, phases>>

\* the transport retires a staged frame (an asynchronous send completes)
Retire ==
    /\ retired < MaxRetire
    /\ free < MaxFree
    /\ free' = free + 1
    /\ retired' = retired + 1
    /\ UNCHANGED <<started, ownUsed, ownTaken, ownLeft, claims, budget, tail, pc, size,
                   claim, ret, uncounted, ld, rpos, racc, seenS, seenO, seenR, roomy,
                   starved, unnamed, claimsDone, phases>>

Done ==
    /\ \A f \in Fillers : pc[f] = "idle"
    /\ UNCHANGED vars

Next ==
    \/ OpenFill \/ Retire \/ Done
    \/ \E f \in Fillers :
          \/ OwnClaim(f) \/ OwnIo(f) \/ OwnCount(f) \/ Continue(f)
          \/ Size(f) \/ Claim(f) \/ ClaimStore(f) \/ Check(f)
          \/ Read1(f) \/ Read2(f) \/ Read3(f) \/ Decide(f) \/ TakeBack(f) \/ Retest(f)
          \/ Stage(f) \/ Count(f) \/ End(f)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Properties                                                              *)
(***************************************************************************)

\* once the fill holds a frame, the room left after every frame in flight
\* has staged covers the reserve, less the own frames taken (their clients'
\* keep-alives are not due): the next update's protocol frames are never
\* refused for the fill's sake
ReserveKept ==
    (SumOver(started, Lines) + Pending + Unstaged + InStep("counting") > 0) =>
        free + ownTaken >= Reserve + OwnUnstaged + Pending

\* no single claim is refused while the room its recompute read had a frame
\* for it past the reserve, every other filler in flight and every line's
\* tail, nor refused for good while the claims its retest loaded leave a
\* frame for it under the budget
NeverStarved == ~starved

\* a refusal counted SEND_RESERVE_REFUSED (no tail or chunk claim read on
\* any line) had no room at its decision for its frame past every filler in
\* flight: the plain counter is a true full
TailsNamed == ~unnamed

=============================================================================
