---------------------------- MODULE RNFillPhase ----------------------------
(***************************************************************************)
(* The fill-phase contract and its debug-build check                       *)
(* (sdk/rocketnet_fill_phase.h; README-RNFillPhase.md).                    *)
(*                                                                         *)
(* One main thread makes main-thread operations (update, send_packets,     *)
(* stop ...); fillers make fill calls (start, finish, abort), each for one *)
(* client.  The contract: no fill call overlaps a main operation, and in   *)
(* one fill phase (between two main operations) every fill call for a      *)
(* client comes from one filler.  The environment here may break the       *)
(* contract: the model checks that the check flags every break (Sound,     *)
(* OneFiller) and nothing else (NoFalseAlarm).                             *)
(*                                                                         *)
(* The code's words: mainCalls and fillCalls are the two counters (a       *)
(* fetch_add, then a load of the other, both sequentially consistent);     *)
(* owner is the per-client owner word, claimed by compare-and-swap and     *)
(* cleared by main_end.  flagged is "an assert fired".                     *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Fillers,        \* the fill threads
    Clients,        \* the client indexes
    MaxMain,        \* main operations in a behaviour
    MaxFills,       \* fill calls per filler in a behaviour
    LookFirst,      \* TRUE: each side looks at the other before it announces (reversed witness)
    LoadStore,      \* TRUE: the claim is a load, then a store, not a CAS (reversed witness)
    ResetOnLeave,   \* FALSE: main_end leaves the owners set (reversed witness)
    NoOwner         \* the owner word of an unclaimed client (0 in the code), not a filler

ASSUME NoOwner \notin Fillers

VARIABLES
    mpc,            \* the main thread: "idle", "announced", "looked", "in", "reset"
    fpc,            \* each filler: "idle", "announced", "looked", "loaded", "in"
    fclient,        \* the client of each filler's current call
    seen,           \* the owner word a LoadStore claim loaded
    mainCalls,
    fillCalls,
    owner,
    flagged,
    \* ghost: the contract itself, read off what happened
    phaseFillers,   \* the fillers that claimed each client in this fill phase
    violated,       \* the contract was broken at some point
    mainsDone,
    fillsDone

vars == <<mpc, fpc, fclient, seen, mainCalls, fillCalls, owner, flagged,
          phaseFillers, violated, mainsDone, fillsDone>>

TypeOK ==
    /\ mpc \in {"idle", "announced", "looked", "in", "reset"}
    /\ fpc \in [Fillers -> {"idle", "announced", "looked", "loaded", "in"}]
    /\ fclient \in [Fillers -> Clients]
    /\ seen \in [Fillers -> Fillers \cup {NoOwner}]
    /\ mainCalls \in 0..1
    /\ fillCalls \in 0..Cardinality(Fillers)
    /\ owner \in [Clients -> Fillers \cup {NoOwner}]
    /\ flagged \in BOOLEAN
    /\ phaseFillers \in [Clients -> SUBSET Fillers]
    /\ violated \in BOOLEAN
    /\ mainsDone \in 0..MaxMain
    /\ fillsDone \in [Fillers -> 0..MaxFills]

Init ==
    /\ mpc = "idle"
    /\ fpc = [f \in Fillers |-> "idle"]
    /\ fclient = [f \in Fillers |-> CHOOSE c \in Clients : TRUE]
    /\ seen = [f \in Fillers |-> NoOwner]
    /\ mainCalls = 0
    /\ fillCalls = 0
    /\ owner = [c \in Clients |-> NoOwner]
    /\ flagged = FALSE
    /\ phaseFillers = [c \in Clients |-> {}]
    /\ violated = FALSE
    /\ mainsDone = 0
    /\ fillsDone = [f \in Fillers |-> 0]

\* a fill call in progress: from its announce (or, looking first, its look) to its leave
FillBusy == \E f \in Fillers : fpc[f] # "idle"
MainBusy == mpc # "idle"

(***************************************************************************)
(* Main: main_begin is MainAnnounce then MainLook (LookFirst swaps them);  *)
(* main_end is MainReset then MainLeave.                                   *)
(***************************************************************************)
MainAnnounce ==
    /\ mainsDone < MaxMain
    /\ \/ ~LookFirst /\ mpc = "idle" /\ mpc' = "announced"
       \/ LookFirst /\ mpc = "looked" /\ mpc' = "in"
    /\ mainCalls' = mainCalls + 1
    /\ violated' = (violated \/ FillBusy)
    /\ UNCHANGED <<fpc, fclient, seen, fillCalls, owner, flagged, phaseFillers, mainsDone, fillsDone>>

MainLook ==
    /\ mainsDone < MaxMain
    /\ \/ ~LookFirst /\ mpc = "announced" /\ mpc' = "in"
       \/ LookFirst /\ mpc = "idle" /\ mpc' = "looked"
    /\ flagged' = (flagged \/ fillCalls > 0)
    /\ violated' = (violated \/ FillBusy)
    /\ UNCHANGED <<fpc, fclient, seen, mainCalls, fillCalls, owner, phaseFillers, mainsDone, fillsDone>>

MainReset ==
    /\ mpc = "in"
    /\ mpc' = "reset"
    /\ owner' = IF ResetOnLeave THEN [c \in Clients |-> NoOwner] ELSE owner
    /\ UNCHANGED <<fpc, fclient, seen, mainCalls, fillCalls, flagged, phaseFillers, violated, mainsDone, fillsDone>>

MainLeave ==
    /\ mpc = "reset"
    /\ mpc' = "idle"
    /\ mainCalls' = mainCalls - 1
    /\ mainsDone' = mainsDone + 1
    \* the next fill phase begins
    /\ phaseFillers' = [c \in Clients |-> {}]
    /\ UNCHANGED <<fpc, fclient, seen, fillCalls, owner, flagged, violated, fillsDone>>

(***************************************************************************)
(* A filler: fill_begin is FillAnnounce, FillLook, then FillClaim (or      *)
(* FillLoad then FillStore); fill_end is FillLeave.                        *)
(***************************************************************************)
FillAnnounce(f, c) ==
    /\ fillsDone[f] < MaxFills
    /\ \/ ~LookFirst /\ fpc[f] = "idle" /\ fpc' = [fpc EXCEPT ![f] = "announced"]
                     /\ fclient' = [fclient EXCEPT ![f] = c]
       \/ LookFirst /\ fpc[f] = "looked" /\ fclient[f] = c /\ fpc' = [fpc EXCEPT ![f] = "announced"]
                    /\ UNCHANGED fclient
    /\ fillCalls' = fillCalls + 1
    /\ violated' = (violated \/ MainBusy)
    /\ UNCHANGED <<mpc, seen, mainCalls, owner, flagged, phaseFillers, mainsDone, fillsDone>>

FillLook(f, c) ==
    /\ fillsDone[f] < MaxFills
    /\ \/ ~LookFirst /\ fpc[f] = "announced" /\ fpc' = [fpc EXCEPT ![f] = "looked"] /\ fclient[f] = c
                     /\ UNCHANGED fclient
       \/ LookFirst /\ fpc[f] = "idle" /\ fpc' = [fpc EXCEPT ![f] = "looked"]
                    /\ fclient' = [fclient EXCEPT ![f] = c]
    /\ flagged' = (flagged \/ mainCalls > 0)
    /\ violated' = (violated \/ MainBusy)
    /\ UNCHANGED <<mpc, seen, mainCalls, fillCalls, owner, phaseFillers, mainsDone, fillsDone>>

\* the next state of the claim's ghost: a second filler on a client in one phase breaks the contract
Claimed(f) ==
    LET c == fclient[f] IN
    /\ phaseFillers' = [phaseFillers EXCEPT ![c] = @ \cup {f}]
    /\ violated' = (violated \/ (phaseFillers[c] \ {f}) # {})

\* the code ready to claim: announced and looked, in either order
ReadyToClaim(f) ==
    \/ ~LookFirst /\ fpc[f] = "looked"
    \/ LookFirst /\ fpc[f] = "announced"

FillClaim(f) ==
    LET c == fclient[f] IN
    /\ ~LoadStore
    /\ ReadyToClaim(f)
    /\ fpc' = [fpc EXCEPT ![f] = "in"]
    /\ IF owner[c] = NoOwner
          THEN /\ owner' = [owner EXCEPT ![c] = f] /\ UNCHANGED flagged
          ELSE /\ UNCHANGED owner /\ flagged' = (flagged \/ owner[c] # f)
    /\ Claimed(f)
    /\ UNCHANGED <<mpc, fclient, seen, mainCalls, fillCalls, mainsDone, fillsDone>>

FillLoad(f) ==
    /\ LoadStore
    /\ ReadyToClaim(f)
    /\ fpc' = [fpc EXCEPT ![f] = "loaded"]
    /\ seen' = [seen EXCEPT ![f] = owner[fclient[f]]]
    /\ UNCHANGED <<mpc, fclient, mainCalls, fillCalls, owner, flagged, phaseFillers, violated, mainsDone, fillsDone>>

FillStore(f) ==
    LET c == fclient[f] IN
    /\ fpc[f] = "loaded"
    /\ fpc' = [fpc EXCEPT ![f] = "in"]
    /\ IF seen[f] = NoOwner
          THEN /\ owner' = [owner EXCEPT ![c] = f] /\ UNCHANGED flagged
          ELSE /\ UNCHANGED owner /\ flagged' = (flagged \/ seen[f] # f)
    /\ Claimed(f)
    /\ UNCHANGED <<mpc, fclient, seen, mainCalls, fillCalls, mainsDone, fillsDone>>

FillLeave(f) ==
    /\ fpc[f] = "in"
    /\ fpc' = [fpc EXCEPT ![f] = "idle"]
    /\ fillCalls' = fillCalls - 1
    /\ fillsDone' = [fillsDone EXCEPT ![f] = @ + 1]
    /\ UNCHANGED <<mpc, fclient, seen, mainCalls, owner, flagged, phaseFillers, violated, mainsDone>>

Done ==
    /\ mpc = "idle" /\ mainsDone = MaxMain
    /\ \A f \in Fillers : fpc[f] = "idle" /\ fillsDone[f] = MaxFills
    /\ UNCHANGED vars

Next ==
    \/ MainAnnounce \/ MainLook \/ MainReset \/ MainLeave
    \/ \E f \in Fillers, c \in Clients : FillAnnounce(f, c) \/ FillLook(f, c)
    \/ \E f \in Fillers : FillClaim(f) \/ FillLoad(f) \/ FillStore(f) \/ FillLeave(f)
    \/ Done

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Properties                                                              *)
(***************************************************************************)

\* a main operation and a fill call both past their checks at once: an assert fired
Sound == (mpc \in {"in", "reset"} /\ \E f \in Fillers : fpc[f] \in {"loaded", "in"}) => flagged

\* two fillers claimed one client in one fill phase: an assert fired
OneFiller == (\E c \in Clients : Cardinality(phaseFillers[c]) > 1) => flagged

\* an assert fires only when the contract was broken
NoFalseAlarm == flagged => violated

=============================================================================
