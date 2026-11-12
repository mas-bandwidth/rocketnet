---------------------------- MODULE RNFillPhase ----------------------------
(***************************************************************************)
(* The fill-phase contract and its check, in every build                  *)
(* (sdk/rocketnet_fill_phase.h, sdk/rocketnet_fill_phase.cpp;              *)
(* README-RNFillPhase.md).                                                 *)
(*                                                                         *)
(* One main thread makes main-thread operations (update, send_packets,     *)
(* stop ...); fillers make fill calls (start, finish, abort), each for one *)
(* client.  The contract: no fill call overlaps a main operation, and in   *)
(* one fill phase (between two main operations) every fill call for a      *)
(* client comes from one filler.  The environment here may break the       *)
(* contract: the model checks that the check flags every break (Sound,     *)
(* OneFiller) and nothing else (NoFalseAlarm).  A flagged call or          *)
(* operation is refused: it does nothing and ends.                         *)
(*                                                                         *)
(* The code's words:                                                       *)
(*   mainCalls  fill_phase->main_calls (fetch_add, then fetch_sub)         *)
(*   epoch      fill_phase->epoch, advanced by main_end                    *)
(*   word[c]    fill_phase->claims[c]: [ep |-> epoch, th |-> thread,       *)
(*              in |-> in bit]; th = NoOwner is the token 0                *)
(*   fepoch[f]  the epoch fill_begin loaded first                          *)
(*   flagged    "refused and counted" (and asserted in a debug build)      *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Fillers,        \* the fill threads
    Clients,        \* the client indexes
    MaxMain,        \* main operations in a behaviour
    MaxFills,       \* fill calls per filler in a behaviour
    LookFirst,      \* TRUE: each side looks at the other before it announces (reversed witness)
    LoadStore,      \* TRUE: the claim is a load, then a store, not a CAS (reversed witness)
    ResetOnLeave,   \* FALSE: main_end leaves the epoch as it was (reversed witness)
    NoOwner         \* the thread field of an unclaimed word (0 in the code), not a filler

ASSUME NoOwner \notin Fillers

Words == [ep : 0..MaxMain, th : Fillers \cup {NoOwner}, in : BOOLEAN]

VARIABLES
    mpc,            \* main: "idle", "announced" (scanning), "looked" (LookFirst only), "in"
    mscan,          \* the clients main has still to look at
    fpc,            \* each filler: "idle", "begun", "looked" (LookFirst), "loaded" (LoadStore), "claimed", "in"
    fclient,        \* the client of each filler's current call
    fepoch,         \* the epoch each filler's call loaded
    seen,           \* the word a LoadStore claim loaded
    mainCalls,
    epoch,
    word,
    flagged,
    \* ghost: the contract itself, read off what happened
    phaseFillers,   \* the fillers that claimed each client in this fill phase
    violated,       \* the contract was broken at some point
    mainsDone,
    fillsDone

vars == <<mpc, mscan, fpc, fclient, fepoch, seen, mainCalls, epoch, word, flagged,
          phaseFillers, violated, mainsDone, fillsDone>>

NoWord == [ep |-> 0, th |-> NoOwner, in |-> FALSE]

TypeOK ==
    /\ mpc \in {"idle", "announced", "looked", "in"}
    /\ mscan \subseteq Clients
    /\ fpc \in [Fillers -> {"idle", "begun", "looked", "loaded", "claimed", "in"}]
    /\ fclient \in [Fillers -> Clients]
    /\ fepoch \in [Fillers -> 0..MaxMain]
    /\ seen \in [Fillers -> Words]
    /\ mainCalls \in 0..1
    /\ epoch \in 0..MaxMain
    /\ word \in [Clients -> Words]
    /\ flagged \in BOOLEAN
    /\ phaseFillers \in [Clients -> SUBSET Fillers]
    /\ violated \in BOOLEAN
    /\ mainsDone \in 0..MaxMain
    /\ fillsDone \in [Fillers -> 0..MaxFills]

Init ==
    /\ mpc = "idle"
    /\ mscan = {}
    /\ fpc = [f \in Fillers |-> "idle"]
    /\ fclient = [f \in Fillers |-> CHOOSE c \in Clients : TRUE]
    /\ fepoch = [f \in Fillers |-> 0]
    /\ seen = [f \in Fillers |-> NoWord]
    /\ mainCalls = 0
    /\ epoch = 0
    /\ word = [c \in Clients |-> NoWord]
    /\ flagged = FALSE
    /\ phaseFillers = [c \in Clients |-> {}]
    /\ violated = FALSE
    /\ mainsDone = 0
    /\ fillsDone = [f \in Fillers |-> 0]

\* a fill call in progress, from its first step to its leave
FillBusy == \E f \in Fillers : fpc[f] # "idle"
MainBusy == mpc # "idle"

(***************************************************************************)
(* Main: main_begin is MainAnnounce, then MainLook for each client (one    *)
(* load each), then MainIn; main_end is MainLeave.  A look that finds an   *)
(* in bit refuses the operation: MainRefused.  LookFirst swaps: it scans,  *)
(* then announces.                                                         *)
(***************************************************************************)
MainAnnounce ==
    /\ mainsDone < MaxMain
    /\ \/ ~LookFirst /\ mpc = "idle" /\ mpc' = "announced" /\ mscan' = Clients
       \/ LookFirst /\ mpc = "looked" /\ mscan = {} /\ mpc' = "in" /\ UNCHANGED mscan
    /\ mainCalls' = mainCalls + 1
    /\ violated' = (violated \/ FillBusy)
    /\ UNCHANGED <<fpc, fclient, fepoch, seen, epoch, word, flagged, phaseFillers, mainsDone, fillsDone>>

MainStartScanFirst ==
    /\ LookFirst
    /\ mainsDone < MaxMain
    /\ mpc = "idle"
    /\ mpc' = "looked"
    /\ mscan' = Clients
    /\ violated' = (violated \/ FillBusy)
    /\ UNCHANGED <<fpc, fclient, fepoch, seen, mainCalls, epoch, word, flagged, phaseFillers, mainsDone, fillsDone>>

\* one client's word: clear, or an in bit (refused)
MainLook(c) ==
    /\ c \in mscan
    /\ \/ ~LookFirst /\ mpc = "announced"
       \/ LookFirst /\ mpc = "looked"
    /\ violated' = (violated \/ FillBusy)
    /\ IF word[c].in
          THEN /\ flagged' = TRUE
               /\ mscan' = {}
               /\ mpc' = "idle"
               /\ mainCalls' = IF LookFirst THEN mainCalls ELSE mainCalls - 1
               /\ mainsDone' = mainsDone + 1
          ELSE /\ mscan' = mscan \ {c}
               /\ UNCHANGED <<flagged, mpc, mainCalls, mainsDone>>
    /\ UNCHANGED <<fpc, fclient, fepoch, seen, epoch, word, phaseFillers, fillsDone>>

MainIn ==
    /\ ~LookFirst
    /\ mpc = "announced"
    /\ mscan = {}
    /\ mpc' = "in"
    /\ violated' = (violated \/ FillBusy)
    /\ UNCHANGED <<mscan, fpc, fclient, fepoch, seen, mainCalls, epoch, word, flagged, phaseFillers, mainsDone, fillsDone>>

\* the operation's work done: the epoch moves (the next fill phase begins), then main_calls drops
MainLeave ==
    /\ mpc = "in"
    /\ mpc' = "idle"
    /\ epoch' = IF ResetOnLeave THEN epoch + 1 ELSE epoch
    /\ mainCalls' = mainCalls - 1
    /\ mainsDone' = mainsDone + 1
    /\ phaseFillers' = [c \in Clients |-> {}]
    /\ violated' = (violated \/ FillBusy)
    /\ UNCHANGED <<mscan, fpc, fclient, fepoch, seen, word, flagged, fillsDone>>

(***************************************************************************)
(* A filler: fill_begin is FillBegin (load the epoch), FillClaim (one CAS  *)
(* that sets the in bit, or refuses) or FillLoad then FillStore, then      *)
(* FillLook (load main_calls and the epoch again); fill_end is FillLeave.  *)
(* LookFirst looks before it claims.                                       *)
(***************************************************************************)
FillBegin(f, c) ==
    /\ fillsDone[f] < MaxFills
    /\ fpc[f] = "idle"
    /\ fpc' = [fpc EXCEPT ![f] = "begun"]
    /\ fclient' = [fclient EXCEPT ![f] = c]
    /\ fepoch' = [fepoch EXCEPT ![f] = epoch]
    /\ violated' = (violated \/ MainBusy)
    /\ UNCHANGED <<mpc, mscan, seen, mainCalls, epoch, word, flagged, phaseFillers, mainsDone, fillsDone>>

\* the claim refuses a client another filler is inside, or claimed in this
\* epoch or a later one: a call that loaded an older epoch is stale (a main
\* operation ended inside it) and must not overwrite the current phase's claim
Taken(f, w) == w.th \notin {NoOwner, f} /\ (w.in \/ w.ep >= fepoch[f])

\* the contract is broken by a second filler on a client in one phase,
\* whether its claim lands or is refused
Second(f) == (phaseFillers[fclient[f]] \ {f}) # {}

\* the ghost of a claim that lands
Claimed(f) ==
    LET c == fclient[f] IN
    /\ phaseFillers' = [phaseFillers EXCEPT ![c] = @ \cup {f}]
    /\ violated' = (violated \/ Second(f) \/ MainBusy)

Refuse(f) ==
    /\ fpc' = [fpc EXCEPT ![f] = "idle"]
    /\ fillsDone' = [fillsDone EXCEPT ![f] = @ + 1]
    /\ flagged' = TRUE

ReadyToClaim(f) ==
    \/ ~LookFirst /\ fpc[f] = "begun"
    \/ LookFirst /\ fpc[f] = "looked"

FillClaim(f) ==
    LET c == fclient[f] IN
    /\ ~LoadStore
    /\ ReadyToClaim(f)
    /\ IF Taken(f, word[c])
          THEN /\ Refuse(f)
               /\ violated' = (violated \/ MainBusy \/ Second(f))
               /\ UNCHANGED <<word, phaseFillers>>
          ELSE /\ word' = [word EXCEPT ![c] = [ep |-> fepoch[f], th |-> f, in |-> TRUE]]
               /\ fpc' = [fpc EXCEPT ![f] = IF LookFirst THEN "in" ELSE "claimed"]
               /\ Claimed(f)
               /\ UNCHANGED <<flagged, fillsDone>>
    /\ UNCHANGED <<mpc, mscan, fclient, fepoch, seen, mainCalls, epoch, mainsDone>>

FillLoad(f) ==
    /\ LoadStore
    /\ ReadyToClaim(f)
    /\ fpc' = [fpc EXCEPT ![f] = "loaded"]
    /\ seen' = [seen EXCEPT ![f] = word[fclient[f]]]
    /\ violated' = (violated \/ MainBusy)
    /\ UNCHANGED <<mpc, mscan, fclient, fepoch, mainCalls, epoch, word, flagged, phaseFillers, mainsDone, fillsDone>>

FillStore(f) ==
    LET c == fclient[f] IN
    /\ fpc[f] = "loaded"
    /\ IF Taken(f, seen[f])
          THEN /\ Refuse(f)
               /\ violated' = (violated \/ MainBusy \/ Second(f))
               /\ UNCHANGED <<word, phaseFillers>>
          ELSE /\ word' = [word EXCEPT ![c] = [ep |-> fepoch[f], th |-> f, in |-> TRUE]]
               /\ fpc' = [fpc EXCEPT ![f] = "claimed"]
               /\ Claimed(f)
               /\ UNCHANGED <<flagged, fillsDone>>
    /\ UNCHANGED <<mpc, mscan, fclient, fepoch, seen, mainCalls, epoch, mainsDone>>

\* a main operation running, or one that ran since this call loaded the epoch: refused
FillLook(f) ==
    LET c == fclient[f] IN
    /\ \/ ~LookFirst /\ fpc[f] = "claimed"
       \/ LookFirst /\ fpc[f] = "begun"
    /\ violated' = (violated \/ MainBusy)
    /\ IF mainCalls > 0 \/ epoch # fepoch[f]
          THEN /\ Refuse(f)
               /\ word' = IF LookFirst THEN word ELSE [word EXCEPT ![c] = [@ EXCEPT !.in = FALSE]]
          ELSE /\ fpc' = [fpc EXCEPT ![f] = IF LookFirst THEN "looked" ELSE "in"]
               /\ UNCHANGED <<flagged, fillsDone, word>>
    /\ UNCHANGED <<mpc, mscan, fclient, fepoch, seen, mainCalls, epoch, phaseFillers, mainsDone>>

FillLeave(f) ==
    LET c == fclient[f] IN
    /\ fpc[f] = "in"
    /\ fpc' = [fpc EXCEPT ![f] = "idle"]
    /\ word' = [word EXCEPT ![c] = [@ EXCEPT !.in = FALSE]]
    /\ fillsDone' = [fillsDone EXCEPT ![f] = @ + 1]
    /\ violated' = (violated \/ MainBusy)
    /\ UNCHANGED <<mpc, mscan, fclient, fepoch, seen, mainCalls, epoch, flagged, phaseFillers, mainsDone>>

Done ==
    /\ mpc = "idle" /\ mainsDone = MaxMain
    /\ \A f \in Fillers : fpc[f] = "idle" /\ fillsDone[f] = MaxFills
    /\ UNCHANGED vars

Next ==
    \/ MainAnnounce \/ MainStartScanFirst \/ MainIn \/ MainLeave
    \/ \E c \in Clients : MainLook(c)
    \/ \E f \in Fillers, c \in Clients : FillBegin(f, c)
    \/ \E f \in Fillers : FillClaim(f) \/ FillLoad(f) \/ FillStore(f) \/ FillLook(f) \/ FillLeave(f)
    \/ Done

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Properties                                                              *)
(***************************************************************************)

\* a main operation and a fill call both past their checks at once never happens unflagged
Sound == (mpc = "in" /\ \E f \in Fillers : fpc[f] = "in") => flagged

\* two fillers claimed one client in one fill phase: flagged
OneFiller == (\E c \in Clients : Cardinality(phaseFillers[c]) > 1) => flagged

\* a refusal happens only when the contract was broken
NoFalseAlarm == flagged => violated

=============================================================================
