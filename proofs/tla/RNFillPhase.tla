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
(* contract.  The model checks that no break gets through the check        *)
(* (Sound, OneFiller) and that every refusal is of a call or operation     *)
(* that broke the contract or met a break (NoFalseAlarm).  A refused call  *)
(* or operation does nothing and ends.                                     *)
(*                                                                         *)
(* The code's words:                                                       *)
(*   mainCalls  fill_phase->main_calls                                     *)
(*   fillsIn[l] fill_phase->lines[l].fills_in; a call counts on the line  *)
(*              its thread id hashes to, a line of LineChoice(f) (any line; *)
(*              an MC gives each filler its own), and fillers may share one *)
(*   fline[f]   the line f's call counted on, NoLine when it counts on none *)
(*   mscan      the lines main_begin has still to look at                  *)
(*   epoch      fill_phase->epoch (1 at init), advanced by main_end        *)
(*   word[c]    clients[c].claim:       [ep |-> epoch, held |-> depth > 0,  *)
(*              in |-> in bit]                                             *)
(*   owner[c]   fill_phase->clients[c].owner; NoOwner is 0                *)
(*   fepoch[f]  the epoch fill_begin loaded first                          *)
(*   seen[f]    the word fill_begin's lock replaced (before)               *)
(*   saved[f]   the owner fill_begin's decide replaced (owner_before)      *)
(* Nesting (main_owner, main_depth, a word's depth above 1) and a client   *)
(* index outside the server's range are not modelled: a nested call or     *)
(* operation is the outer one's own work and checks nothing.               *)
(*                                                                         *)
(* What a step of the code gives back is an argument of its action: the   *)
(* line a count took (FillCount's l), and whether a look, lock or decide   *)
(* let the call or operation go on (ok), which the action requires to be  *)
(* what the state says.                                                    *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Fillers,        \* the fill threads
    Clients,        \* the client indexes
    MaxMain,        \* main operations in a behaviour
    MaxFills,       \* fill calls per filler in a behaviour
    LookFirst,      \* TRUE: each side looks at the other before it announces (reversed witness)
    LoadStore,      \* TRUE: the lock is a load, then a store, not a CAS (reversed witness)
    ResetOnLeave,   \* FALSE: main_end leaves the epoch as it was (reversed witness)
    NoEpochLook,    \* TRUE: the look reads main_calls only, not the epoch again (reversed witness)
    NoRestore,      \* TRUE: a refused look leaves its claim, out, in place of the word and owner it replaced (reversed witness)
    ScanOne,        \* TRUE: main_begin looks at one fills_in line only, as if there were one counter (reversed witness)
    OptedAtStart,   \* TRUE: the game named a client before anything here (the armed check, as every case before the latch)
    ArmedAtCreate,  \* TRUE: the check is armed from create, whatever the game calls (reversed witness)
    Lines,          \* the fills_in lines, their indexes
    NoOwner         \* the owner of a client nobody claimed (0 in the code), not a filler

ASSUME NoOwner \notin Fillers
ASSUME Lines \subseteq Nat

Epochs == 0..(MaxMain + 1)
Words == [ep : Epochs, held : BOOLEAN, in : BOOLEAN]
Owners == Fillers \cup {NoOwner}

\* the lines filler f's calls may count on: any (the code hashes the thread
\* id); an MC overrides it to give each filler one line, as a thread has
LineChoice(f) == Lines

\* a call that counts on no line
NoLine == -1

VARIABLES
    mpc,            \* main: "idle", "announced" (scanning), "scan" (LookFirst: scanning before it announces), "in", "epoched"
    mscan,
    fpc,            \* each filler: "idle", "begun", "loaded" (LoadStore), "locked", "decided", "counted", "looked" (LookFirst), "in", "unclaimed"
    fclient,        \* the client of each filler's current call
    fepoch,
    seen,
    saved,
    mainCalls,
    fillsIn,
    fline,
    epoch,
    word,
    owner,
    armed,          \* the latch: calls enter the check (armed)
    \* ghost: the contract itself, read off what happened
    opted,          \* the game has made a call that names a client: from then on the thread rules are its contract
    fbroke,         \* each filler's current call has overlapped a main operation
    phaseIn,        \* the fillers whose calls on each client passed the check in this fill phase
    falseAlarm,     \* a refusal of a call or operation that neither broke the contract nor met a break
    mainsDone,
    fillsDone

vars == <<mpc, mscan, fpc, fclient, fepoch, seen, saved, mainCalls, fillsIn, fline, epoch, word, owner,
          armed, opted, fbroke, phaseIn, falseAlarm, mainsDone, fillsDone>>

NoWord == [ep |-> 0, held |-> FALSE, in |-> FALSE]

TypeOK ==
    /\ mpc \in {"idle", "announced", "scan", "in", "epoched"}
    /\ mscan \subseteq Lines
    /\ fpc \in [Fillers -> {"idle", "begun", "loaded", "locked", "decided", "counted", "looked", "in", "unclaimed"}]
    /\ fclient \in [Fillers -> Clients]
    /\ fepoch \in [Fillers -> Epochs]
    /\ seen \in [Fillers -> Words]
    /\ saved \in [Fillers -> Owners]
    /\ mainCalls \in 0..1
    /\ fillsIn \in [Lines -> 0..Cardinality(Fillers)]
    /\ fline \in [Fillers -> Lines \cup {NoLine}]
    /\ epoch \in Epochs
    /\ word \in [Clients -> Words]
    /\ owner \in [Clients -> Owners]
    /\ armed \in BOOLEAN
    /\ opted \in BOOLEAN
    /\ fbroke \in [Fillers -> BOOLEAN]
    /\ phaseIn \in [Clients -> SUBSET Fillers]
    /\ falseAlarm \in BOOLEAN
    /\ mainsDone \in 0..MaxMain
    /\ fillsDone \in [Fillers -> 0..MaxFills]

Init ==
    /\ mpc = "idle"
    /\ mscan = {}
    /\ fpc = [f \in Fillers |-> "idle"]
    /\ fclient = [f \in Fillers |-> CHOOSE c \in Clients : TRUE]
    /\ fepoch = [f \in Fillers |-> 0]
    /\ seen = [f \in Fillers |-> NoWord]
    /\ saved = [f \in Fillers |-> NoOwner]
    /\ mainCalls = 0
    /\ fillsIn = [l \in Lines |-> 0]
    /\ fline = [f \in Fillers |-> NoLine]
    /\ epoch = 1
    /\ word = [c \in Clients |-> NoWord]
    /\ owner = [c \in Clients |-> NoOwner]
    /\ armed = (OptedAtStart \/ ArmedAtCreate)
    /\ opted = OptedAtStart
    /\ fbroke = [f \in Fillers |-> FALSE]
    /\ phaseIn = [c \in Clients |-> {}]
    /\ falseAlarm = FALSE
    /\ mainsDone = 0
    /\ fillsDone = [f \in Fillers |-> 0]

\* a fill call in progress, from its first step to its leave
FillBusy == \E f \in Fillers : fpc[f] # "idle"
MainBusy == mpc # "idle"

\* the ghost of a main step: every fill call in progress has met a main operation
MainMeets == fbroke' = [g \in Fillers |-> fbroke[g] \/ fpc[g] # "idle"]

(***************************************************************************)
(* Main: main_begin is MainAnnounce (main_calls fetch_add), then MainLook  *)
(* for each fills_in line (one load each; refused at a line that is not   *)
(* 0), then MainIn; main_end is MainEpoch (the epoch store) then MainDrop  *)
(* (main_calls fetch_sub).  LookFirst scans first (MainScan), then         *)
(* announces.  One main thread: the refusal of a second main operation is  *)
(* not modelled.                                                           *)
(***************************************************************************)
ScanLines == IF ScanOne THEN {CHOOSE l \in Lines : TRUE} ELSE Lines

MainAnnounce ==
    /\ \/ ~LookFirst /\ armed /\ mpc = "idle" /\ mainsDone < MaxMain /\ mpc' = "announced" /\ mscan' = ScanLines
       \/ LookFirst /\ mpc = "scan" /\ mscan = {} /\ mpc' = "in" /\ UNCHANGED mscan
    /\ mainCalls' = mainCalls + 1
    /\ MainMeets
    /\ UNCHANGED <<fpc, fclient, fepoch, seen, saved, fillsIn, fline, epoch, word, owner, phaseIn, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<armed, opted>>

MainScan ==
    /\ LookFirst
    /\ armed
    /\ mpc = "idle"
    /\ mainsDone < MaxMain
    /\ mpc' = "scan"
    /\ mscan' = ScanLines
    /\ MainMeets
    /\ UNCHANGED <<fpc, fclient, fepoch, seen, saved, mainCalls, fillsIn, fline, epoch, word, owner, phaseIn, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<armed, opted>>

\* ok: main_begin's look at line l let the operation go on (the line's
\* count was 0); a look at a counted line refuses it
MainLook(l, ok) ==
    /\ mpc \in {"announced", "scan"}
    /\ l \in mscan
    /\ ok = (fillsIn[l] = 0)
    /\ MainMeets
    /\ IF ~ok
          THEN /\ mpc' = "idle"
               /\ mscan' = {}
               /\ mainsDone' = mainsDone + 1
               /\ falseAlarm' = (falseAlarm \/ ~FillBusy)
               /\ mainCalls' = IF LookFirst THEN mainCalls ELSE mainCalls - 1
          ELSE /\ mscan' = mscan \ {l}
               /\ UNCHANGED <<mpc, mainCalls, mainsDone, falseAlarm>>
    /\ UNCHANGED <<fpc, fclient, fepoch, seen, saved, fillsIn, fline, epoch, word, owner, phaseIn, fillsDone>>
    /\ UNCHANGED <<armed, opted>>

MainIn ==
    /\ ~LookFirst
    /\ mpc = "announced"
    /\ mscan = {}
    /\ mpc' = "in"
    /\ MainMeets
    /\ UNCHANGED <<mscan, fpc, fclient, fepoch, seen, saved, mainCalls, fillsIn, fline, epoch, word, owner, phaseIn, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<armed, opted>>

\* the operation's work done: the epoch moves, and a new fill phase begins
MainEpoch ==
    /\ mpc = "in"
    /\ mpc' = "epoched"
    /\ epoch' = IF ResetOnLeave THEN epoch + 1 ELSE epoch
    /\ phaseIn' = [c \in Clients |-> {}]
    /\ MainMeets
    /\ UNCHANGED <<mscan, fpc, fclient, fepoch, seen, saved, mainCalls, fillsIn, fline, word, owner, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<armed, opted>>

MainDrop ==
    /\ mpc = "epoched"
    /\ mpc' = "idle"
    /\ mainCalls' = mainCalls - 1
    /\ mainsDone' = mainsDone + 1
    /\ MainMeets
    /\ UNCHANGED <<mscan, fpc, fclient, fepoch, seen, saved, fillsIn, fline, epoch, word, owner, phaseIn, falseAlarm, fillsDone>>
    /\ UNCHANGED <<armed, opted>>

(***************************************************************************)
(* A filler: fill_begin is FillBegin (load the epoch), FillLock (one CAS   *)
(* that sets the in bit, refused when it is set) or FillLoad then          *)
(* FillStore, FillDecide (refused when another filler claimed the client   *)
(* in this epoch, restoring the word; else the owner and the claim are     *)
(* stored), FillCount (fills_in fetch_add on the line l the thread hashes  *)
(* to), FillLook (load main_calls and the epoch again; refused, restoring  *)
(* the owner and the word, when a main operation is running or ran since   *)
(* FillBegin).  fill_end is FillUnclaim (clear in) then FillLeave          *)
(* (fills_in fetch_sub).  The fetch_subs are on the line the call counted  *)
(* on (fline).  LookFirst looks before it counts.                          *)
(***************************************************************************)

\* a fill step: the call meets the main operation in progress, if any
FillMeets(f) == fbroke' = [fbroke EXCEPT ![f] = @ \/ MainBusy]

\* another filler passed the check on this client in this fill phase
Second(f) == (phaseIn[fclient[f]] \ {f}) # {}

\* another filler is inside a call on this client
OtherOn(f) == \E g \in Fillers \ {f} : fpc[g] # "idle" /\ fclient[g] = fclient[f]

\* a refusal of f's call is justified when the call broke the contract or met a break
Justified(f) == fbroke[f] \/ MainBusy \/ Second(f) \/ OtherOn(f)

FillRefused(f) ==
    /\ fpc' = [fpc EXCEPT ![f] = "idle"]
    /\ fillsDone' = [fillsDone EXCEPT ![f] = @ + 1]
    /\ falseAlarm' = (falseAlarm \/ ~Justified(f))

FillBegin(f, c) ==
    /\ armed
    /\ fillsDone[f] < MaxFills
    /\ fpc[f] = "idle"
    /\ fpc' = [fpc EXCEPT ![f] = "begun"]
    /\ fclient' = [fclient EXCEPT ![f] = c]
    /\ fepoch' = [fepoch EXCEPT ![f] = epoch]
    /\ fbroke' = [fbroke EXCEPT ![f] = MainBusy]
    /\ UNCHANGED <<mpc, mscan, seen, saved, mainCalls, fillsIn, fline, epoch, word, owner, phaseIn, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<armed, opted>>

\* ok: the lock was taken (the in bit was clear); else the call is refused
FillLock(f, ok) ==
    LET c == fclient[f] IN
    /\ ~LoadStore
    /\ fpc[f] = "begun"
    /\ ok = ~word[c].in
    /\ FillMeets(f)
    /\ IF ~ok
          THEN /\ FillRefused(f)
               /\ UNCHANGED <<seen, word>>
          ELSE /\ seen' = [seen EXCEPT ![f] = word[c]]
               /\ word' = [word EXCEPT ![c] = [@ EXCEPT !.in = TRUE]]
               /\ fpc' = [fpc EXCEPT ![f] = "locked"]
               /\ UNCHANGED <<falseAlarm, fillsDone>>
    /\ UNCHANGED <<mpc, mscan, fclient, fepoch, saved, mainCalls, fillsIn, fline, epoch, owner, phaseIn, mainsDone>>
    /\ UNCHANGED <<armed, opted>>

FillLoad(f) ==
    /\ LoadStore
    /\ fpc[f] = "begun"
    /\ fpc' = [fpc EXCEPT ![f] = "loaded"]
    /\ seen' = [seen EXCEPT ![f] = word[fclient[f]]]
    /\ FillMeets(f)
    /\ UNCHANGED <<mpc, mscan, fclient, fepoch, saved, mainCalls, fillsIn, fline, epoch, word, owner, phaseIn, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<armed, opted>>

FillStore(f) ==
    LET c == fclient[f] IN
    /\ fpc[f] = "loaded"
    /\ FillMeets(f)
    /\ IF seen[f].in
          THEN /\ FillRefused(f)
               /\ UNCHANGED word
          ELSE /\ word' = [word EXCEPT ![c] = [seen[f] EXCEPT !.in = TRUE]]
               /\ fpc' = [fpc EXCEPT ![f] = "locked"]
               /\ UNCHANGED <<falseAlarm, fillsDone>>
    /\ UNCHANGED <<mpc, mscan, fclient, fepoch, seen, saved, mainCalls, fillsIn, fline, epoch, owner, phaseIn, mainsDone>>
    /\ UNCHANGED <<armed, opted>>

\* the lock is held: only f writes the word and the owner until it clears in.
\* ok: f claimed the client; else another filler holds it in this epoch and
\* the call is refused
FillDecide(f, ok) ==
    LET c == fclient[f] IN
    /\ fpc[f] = "locked"
    /\ ok = ~(owner[c] \notin {NoOwner, f} /\ seen[f].ep = fepoch[f])
    /\ FillMeets(f)
    /\ IF ~ok
          THEN /\ FillRefused(f)
               /\ word' = [word EXCEPT ![c] = seen[f]]
               /\ UNCHANGED <<saved, owner>>
          ELSE /\ saved' = [saved EXCEPT ![f] = owner[c]]
               /\ owner' = [owner EXCEPT ![c] = f]
               /\ word' = [word EXCEPT ![c] = [ep |-> fepoch[f], held |-> TRUE, in |-> TRUE]]
               /\ fpc' = [fpc EXCEPT ![f] = "decided"]
               /\ UNCHANGED <<falseAlarm, fillsDone>>
    /\ UNCHANGED <<mpc, mscan, fclient, fepoch, seen, mainCalls, fillsIn, fline, epoch, phaseIn, mainsDone>>
    /\ UNCHANGED <<armed, opted>>

\* l: the line the call counted on
FillCount(f, l) ==
    LET c == fclient[f] IN
    /\ \/ ~LookFirst /\ fpc[f] = "decided" /\ fpc' = [fpc EXCEPT ![f] = "counted"] /\ UNCHANGED phaseIn
       \/ LookFirst /\ fpc[f] = "looked" /\ fpc' = [fpc EXCEPT ![f] = "in"]
          /\ phaseIn' = [phaseIn EXCEPT ![c] = @ \cup {f}]
    /\ fillsIn' = [fillsIn EXCEPT ![l] = @ + 1]
    /\ fline' = [fline EXCEPT ![f] = l]
    /\ FillMeets(f)
    /\ UNCHANGED <<mpc, mscan, fclient, fepoch, seen, saved, mainCalls, epoch, word, owner, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<armed, opted>>

\* a main operation running, or one that ran since this call loaded the
\* epoch: refused (ok FALSE)
FillLook(f, ok) ==
    LET c == fclient[f] IN
    /\ \/ ~LookFirst /\ fpc[f] = "counted"
       \/ LookFirst /\ fpc[f] = "decided"
    /\ ok = ~(mainCalls > 0 \/ (~NoEpochLook /\ epoch # fepoch[f]))
    /\ FillMeets(f)
    /\ IF ~ok
          THEN /\ FillRefused(f)
               /\ word' = [word EXCEPT ![c] = IF NoRestore THEN [ep |-> fepoch[f], held |-> FALSE, in |-> FALSE] ELSE seen[f]]
               /\ owner' = IF NoRestore THEN owner ELSE [owner EXCEPT ![c] = saved[f]]
               /\ fillsIn' = IF LookFirst THEN fillsIn ELSE [fillsIn EXCEPT ![fline[f]] = @ - 1]
               /\ fline' = IF LookFirst THEN fline ELSE [fline EXCEPT ![f] = NoLine]
               /\ UNCHANGED phaseIn
          ELSE /\ fpc' = [fpc EXCEPT ![f] = IF LookFirst THEN "looked" ELSE "in"]
               /\ phaseIn' = IF LookFirst THEN phaseIn ELSE [phaseIn EXCEPT ![c] = @ \cup {f}]
               /\ UNCHANGED <<falseAlarm, fillsDone, word, owner, fillsIn, fline>>
    /\ UNCHANGED <<mpc, mscan, fclient, fepoch, seen, saved, mainCalls, epoch, mainsDone>>
    /\ UNCHANGED <<armed, opted>>

FillUnclaim(f) ==
    LET c == fclient[f] IN
    /\ fpc[f] = "in"
    /\ fpc' = [fpc EXCEPT ![f] = "unclaimed"]
    /\ word' = [word EXCEPT ![c] = [@ EXCEPT !.held = FALSE, !.in = FALSE]]
    /\ FillMeets(f)
    /\ UNCHANGED <<mpc, mscan, fclient, fepoch, seen, saved, mainCalls, fillsIn, fline, epoch, owner, phaseIn, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<armed, opted>>

FillLeave(f) ==
    /\ fpc[f] = "unclaimed"
    /\ fpc' = [fpc EXCEPT ![f] = "idle"]
    /\ fillsIn' = [fillsIn EXCEPT ![fline[f]] = @ - 1]
    /\ fline' = [fline EXCEPT ![f] = NoLine]
    /\ fillsDone' = [fillsDone EXCEPT ![f] = @ + 1]
    /\ FillMeets(f)
    /\ UNCHANGED <<mpc, mscan, fclient, fepoch, seen, saved, mainCalls, epoch, word, owner, phaseIn, falseAlarm, mainsDone>>
    /\ UNCHANGED <<armed, opted>>

\* The latch.  The server creates the check not armed, and the game's first
\* call that names a client arms it, for good (Arm).  Not armed, a call's
\* scope does nothing: the whole call is one step that touches no word of
\* the check (FillSkip, MainSkip).  A call that began armed runs the check to
\* its end.  A call that began before Arm and is still running after it is
\* not in the model: the check does not see it.
Arm ==
    /\ ~opted
    /\ opted' = TRUE
    /\ armed' = TRUE
    /\ UNCHANGED <<mpc, mscan, fpc, fclient, fepoch, seen, saved, mainCalls, fillsIn, fline, epoch, word, owner,
                   fbroke, phaseIn, falseAlarm, mainsDone, fillsDone>>

FillSkip(f) ==
    /\ ~armed
    /\ fpc[f] = "idle"
    /\ fillsDone[f] < MaxFills
    /\ fillsDone' = [fillsDone EXCEPT ![f] = @ + 1]
    /\ UNCHANGED <<mpc, mscan, fpc, fclient, fepoch, seen, saved, mainCalls, fillsIn, fline, epoch, word, owner,
                   armed, opted, fbroke, phaseIn, falseAlarm, mainsDone>>

MainSkip ==
    /\ ~armed
    /\ mpc = "idle"
    /\ mainsDone < MaxMain
    /\ mainsDone' = mainsDone + 1
    /\ UNCHANGED <<mscan, mpc, fpc, fclient, fepoch, seen, saved, mainCalls, fillsIn, fline, epoch, word, owner,
                   armed, opted, fbroke, phaseIn, falseAlarm, fillsDone>>

Done ==
    /\ mpc = "idle" /\ mainsDone = MaxMain
    /\ \A f \in Fillers : fpc[f] = "idle" /\ fillsDone[f] = MaxFills
    /\ UNCHANGED vars

Next ==
    \/ MainAnnounce \/ MainScan \/ MainIn \/ MainEpoch \/ MainDrop
    \/ \E l \in Lines, ok \in BOOLEAN : MainLook(l, ok)
    \/ \E f \in Fillers, c \in Clients : FillBegin(f, c)
    \/ \E f \in Fillers, ok \in BOOLEAN : FillLock(f, ok) \/ FillDecide(f, ok) \/ FillLook(f, ok)
    \/ \E f \in Fillers : \E l \in LineChoice(f) : FillCount(f, l)
    \/ \E f \in Fillers : FillLoad(f) \/ FillStore(f) \/ FillUnclaim(f) \/ FillLeave(f) \/ FillSkip(f)
    \/ Arm \/ MainSkip
    \/ Done

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Properties                                                              *)
(***************************************************************************)

\* a main operation and a fill call are never both past their checks
Sound == ~(mpc \in {"in", "epoched"} /\ \E f \in Fillers : fpc[f] \in {"in", "unclaimed"})

\* in one fill phase, the calls on a client that pass the check are one filler's
OneFiller == \A c \in Clients : Cardinality(phaseIn[c]) <= 1

\* every refusal is of a call or operation that broke the contract or met a break
NoFalseAlarm == ~falseAlarm

\* before the game opts in no call is inside the check, so none is refused or counted
NoCheckBeforeOptIn == ~opted => (mpc = "idle" /\ \A f \in Fillers : fpc[f] = "idle")

\* the latch only arms
ArmedStays == [][armed => armed']_vars

=============================================================================
