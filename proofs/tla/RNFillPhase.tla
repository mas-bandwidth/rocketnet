---------------------------- MODULE RNFillPhase ----------------------------
(***************************************************************************)
(* The fill-phase contract and its check, in every build                  *)
(* (sdk/rocketnet_fill_phase.h, sdk/rocketnet_fill_phase.cpp;              *)
(* README-RNFillPhase.md).                                                 *)
(*                                                                         *)
(* One main thread makes main-thread operations (update, send_packets,     *)
(* stop ...); fillers make fill calls (start, finish, abort), each for one *)
(* client, and takes (update_client).  The contract: no fill call or take  *)
(* overlaps a main operation; in one fill phase (between two main          *)
(* operations) every fill call for a client comes from one filler; and in  *)
(* the window of the update in three calls (from update_receive's main     *)
(* operation to update_finish's) only takes and the update calls run.  The *)
(* environment here may break the contract.  The model checks that no      *)
(* break gets through the check (Sound, OneFiller, TakeSound, WindowShut)  *)
(* and that every refusal is of a call or operation that broke the         *)
(* contract or met a break (NoFalseAlarm).  A refused call or operation    *)
(* does nothing and ends.                                                  *)
(*                                                                         *)
(* The code's words:                                                       *)
(*   mainCalls  fill_phase->main_calls                                     *)
(*   window     fill_phase->window: the update in three calls is open      *)
(*   stage      the message layer's update_stage is not CLOSED: the update *)
(*              is open, whether or not the transport opened its own       *)
(*   kicked     ghost: an update call ended connections for deliveries     *)
(*              its own finish of the open update handed over (KickAll)    *)
(*   unchecked  ghost: a plain finish or abort ran unchecked (ScanFirst)   *)
(*   mkind      the main operation's kind: "update" (begun with           *)
(*              update_call: update, update_receive, update_finish) or    *)
(*              "other"; "none" between operations                         *)
(*   fillsIn[l] fill_phase->lines[l].fills_in; a call counts on the line  *)
(*              its thread id hashes to, a line of LineChoice(f) (any line; *)
(*              an MC gives each filler its own), and fillers may share one *)
(*   fline[f]   the line f's call counted on, NoLine when it counts on none *)
(*   mscan      the lines main_begin has still to look at                  *)
(*   epoch      fill_phase->epoch (1 at init), advanced by main_end        *)
(*   word[c]    clients[c].claim:       [ep |-> epoch, depth |-> depth,     *)
(*              in |-> in bit]; depth runs to DepthMax                     *)
(*   owner[c]   fill_phase->clients[c].owner; NoOwner is 0                *)
(*   fepoch[f]  the epoch fill_begin loaded first                          *)
(*   seen[f]    the word fill_begin's lock replaced, or the word its      *)
(*              nested test loaded (before)                                *)
(*   saved[f]   the owner fill_begin's decide loaded (owner_before)        *)
(*   overlaps   the MAIN_THREAD_CALL_OVERLAP counter (overlap_counter)     *)
(*   twoFillers the CLIENT_SEND_TWO_THREADS counter (two_fillers_counter)  *)
(* A fill call nests on its client: the call inside a fill call for the   *)
(* same client on the same thread (the message layer's start calls the     *)
(* transport's start_packet) loads the word (FillPeek), finds it in with   *)
(* depth 1 or more, loads the owner and finds itself (FillNest), and only *)
(* counts depth; its end takes the depth back (FillUnnest).  The same two  *)
(* loads by a filler that is not inside refuse it, and the model lets any *)
(* filler make them: NoFalseNest is that none takes itself as nested.  The *)
(* depth is the code's 7 bits: a call nested DepthMax deep is refused as   *)
(* API misuse, never carried into the epoch.  Nesting of main operations   *)
(* (main_owner, main_depth) and a client index outside the server's range  *)
(* are not modelled: a nested operation is the outer one's own work and    *)
(* checks nothing.                                                         *)
(*                                                                         *)
(* What a step of the code gives back is an argument of its action: the   *)
(* line a count took (FillCount's and TakeCount's l), whether a look, lock *)
(* or decide let the call or operation go on (ok), which the action        *)
(* requires to be what the state says, and the window an update           *)
(* operation's work left (MainEpoch's w).                                  *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Fillers,        \* the fill threads
    Clients,        \* the client indexes
    MaxMain,        \* main operations in a behaviour
    MaxFills,       \* fill calls per filler in a behaviour
    MaxTakes,       \* takes (update_client) per filler in a behaviour
    LookFirst,      \* TRUE: each side looks at the other before it announces (reversed witness)
    LoadStore,      \* TRUE: the lock is a load, then a store, not a CAS (reversed witness)
    ResetOnLeave,   \* FALSE: main_end leaves the epoch as it was (reversed witness)
    NoEpochLook,    \* TRUE: the look reads main_calls only, not the epoch again (reversed witness)
    NoRestore,      \* TRUE: a refused look leaves its claim, out, in place of the word and owner it replaced (reversed witness)
    ScanOne,        \* TRUE: main_begin looks at one fills_in line only, as if there were one counter (reversed witness)
    NoWindow,       \* TRUE: a fill call's look does not read the window (reversed witness)
    NoMainWindow,   \* TRUE: main_begin does not read the window (reversed witness)
    TakeUncounted,  \* TRUE: a take does not count itself in on a fills_in line (reversed witness)
    OptedAtStart,   \* TRUE: the game named a client before anything here (the armed check, as every case before the latch)
    ArmedAtCreate,  \* TRUE: the check is armed from create, whatever the game calls (reversed witness)
    OwnerAtDecide,  \* TRUE: the decide stores the owner and the claim, and a refused look puts them back in three stores (reversed witness)
    KickAll,        \* TRUE: an update call that finds the update open finishes it and begins the next in the same call (reversed witness)
    ShutOnEarly,    \* TRUE: an update call's work may leave the update open with the window shut (reversed witness)
    ScanFirst,      \* TRUE: the plain finish and abort scan for the open message before any check (reversed witness)
    DepthMax,       \* ROCKETNET_FILL_PHASE_DEPTH_MAX: the deepest nesting the word holds
    Lines,          \* the fills_in lines, their indexes
    NoOwner         \* the owner of a client nobody claimed (0 in the code), not a filler

ASSUME NoOwner \notin Fillers
ASSUME Lines \subseteq Nat
ASSUME DepthMax \in Nat /\ DepthMax >= 1

Epochs == 0..(MaxMain + 1)
Words == [ep : Epochs, depth : 0..DepthMax, in : BOOLEAN]
Owners == Fillers \cup {NoOwner}
Kinds == {"update", "other"}

\* the lines filler f's calls may count on: any (the code hashes the thread
\* id); an MC overrides it to give each filler one line, as a thread has
LineChoice(f) == Lines

\* a call that counts on no line
NoLine == -1

\* a filler's control states inside a fill call, and inside a take.
\* "peeked": a call not inside found the word claimed and is to load the
\* owner; "nbegun", "npeeked": a call nested inside f's own, before and
\* after its word load; "nested": a
\* call not inside took itself as nested (NoFalseNest); "unwind1",
\* "unwind2": OwnerAtDecide's refused look between its stores
FillStates == {"begun", "loaded", "locked", "decided", "counted", "looked", "in", "unclaimed",
               "nbegun", "peeked", "npeeked", "nested", "unwind1", "unwind2"}
TakeStates == {"tcounted", "tin"}

\* the states of a fill call past its check
PastCheck == {"in", "unclaimed", "nbegun", "npeeked", "nested"}

\* the refusal steps, for the trace check's reach predicates
Refusals == {"none", "MainLook", "MainIn", "FillLock", "FillDecide", "FillLook", "FillNest", "TakeLook"}

VARIABLES
    mpc,            \* main: "idle", "announced" (scanning), "scan" (LookFirst: scanning before it announces), "in", "epoched"
    mscan,
    mkind,
    fpc,            \* each filler: "idle", a fill state, or a take state ("tcounted", "tin")
    fclient,        \* the client of each filler's current fill call
    fepoch,
    seen,
    saved,
    mainCalls,
    window,
    stage,          \* the update is open: the message layer's update_stage is not CLOSED (update_receive to update_finish)
    fillsIn,
    fline,
    epoch,
    word,
    owner,
    armed,          \* the latch: calls enter the check (armed)
    overlaps,
    twoFillers,
    \* ghost: the contract itself, read off what happened
    opted,          \* the game has made a call that names a client or opens a split update: from then on the thread rules are its contract
    fbroke,         \* each filler's current call has overlapped a main operation
    phaseIn,        \* the fillers whose calls on each client passed the check in this fill phase
    falseAlarm,     \* a refusal of a call or operation that neither broke the contract nor met a break
    refusal,        \* the last refusal step taken ("none" before any): history for the trace check's reach predicates
    kicked,         \* an update call ended connections for deliveries its own finish of the open update handed over (KickAll)
    unchecked,      \* a plain finish or abort ran its scan in the window or beside a fill call past its check (ScanFirst)
    mainsDone,
    fillsDone,
    takesDone

vars == <<mpc, mscan, mkind, fpc, fclient, fepoch, seen, saved, mainCalls, window, stage, fillsIn, fline, epoch, word, owner,
          armed, overlaps, twoFillers, opted, fbroke, phaseIn, falseAlarm, refusal, kicked, unchecked, mainsDone, fillsDone,
          takesDone>>

\* what the MC runs fingerprint (VIEW View): the refusal counts and the last
\* refusal are history no step reads but to count on, and would only
\* multiply the states
View == <<mpc, mscan, mkind, fpc, fclient, fepoch, seen, saved, mainCalls, window, stage, fillsIn, fline, epoch, word, owner,
               armed, opted, fbroke, phaseIn, falseAlarm, kicked, unchecked, mainsDone, fillsDone, takesDone>>

NoWord == [ep |-> 0, depth |-> 0, in |-> FALSE]

TypeOK ==
    /\ mpc \in {"idle", "announced", "scan", "in", "epoched"}
    /\ mscan \subseteq Lines
    /\ mkind \in Kinds \cup {"none"}
    /\ fpc \in [Fillers -> {"idle"} \cup FillStates \cup TakeStates]
    /\ fclient \in [Fillers -> Clients]
    /\ fepoch \in [Fillers -> Epochs]
    /\ seen \in [Fillers -> Words]
    /\ saved \in [Fillers -> Owners]
    /\ mainCalls \in 0..1
    /\ window \in BOOLEAN
    /\ stage \in BOOLEAN
    /\ kicked \in BOOLEAN
    /\ unchecked \in BOOLEAN
    /\ fillsIn \in [Lines -> 0..Cardinality(Fillers)]
    /\ fline \in [Fillers -> Lines \cup {NoLine}]
    /\ epoch \in Epochs
    /\ word \in [Clients -> Words]
    /\ owner \in [Clients -> Owners]
    /\ armed \in BOOLEAN
    /\ overlaps \in Nat
    /\ twoFillers \in Nat
    /\ opted \in BOOLEAN
    /\ fbroke \in [Fillers -> BOOLEAN]
    /\ phaseIn \in [Clients -> SUBSET Fillers]
    /\ falseAlarm \in BOOLEAN
    /\ refusal \in Refusals
    /\ mainsDone \in 0..MaxMain
    /\ fillsDone \in [Fillers -> 0..MaxFills]
    /\ takesDone \in [Fillers -> 0..MaxTakes]

Init ==
    /\ mpc = "idle"
    /\ mscan = {}
    /\ mkind = "none"
    /\ fpc = [f \in Fillers |-> "idle"]
    /\ fclient = [f \in Fillers |-> CHOOSE c \in Clients : TRUE]
    /\ fepoch = [f \in Fillers |-> 0]
    /\ seen = [f \in Fillers |-> NoWord]
    /\ saved = [f \in Fillers |-> NoOwner]
    /\ mainCalls = 0
    /\ window = FALSE
    /\ stage = FALSE
    /\ kicked = FALSE
    /\ unchecked = FALSE
    /\ fillsIn = [l \in Lines |-> 0]
    /\ fline = [f \in Fillers |-> NoLine]
    /\ epoch = 1
    /\ word = [c \in Clients |-> NoWord]
    /\ owner = [c \in Clients |-> NoOwner]
    /\ armed = (OptedAtStart \/ ArmedAtCreate)
    /\ overlaps = 0
    /\ twoFillers = 0
    /\ opted = OptedAtStart
    /\ fbroke = [f \in Fillers |-> FALSE]
    /\ phaseIn = [c \in Clients |-> {}]
    /\ falseAlarm = FALSE
    /\ refusal = "none"
    /\ mainsDone = 0
    /\ fillsDone = [f \in Fillers |-> 0]
    /\ takesDone = [f \in Fillers |-> 0]

\* a fill call or a take in progress, from its first step to its leave
FillBusy == \E f \in Fillers : fpc[f] # "idle"
MainBusy == mpc # "idle"

\* the ghost of a main step: every call in progress has met a main operation
MainMeets == fbroke' = [g \in Fillers |-> fbroke[g] \/ fpc[g] # "idle"]

\* a falseAlarm, when the step's refusal is not justified
Alarm(justified) == IF justified THEN UNCHANGED falseAlarm ELSE falseAlarm' = TRUE

(***************************************************************************)
(* Main: main_begin is MainAnnounce (main_calls fetch_add), then MainLook  *)
(* for each fills_in line (one load each; refused at a line that is not   *)
(* 0), then MainIn (the window's load: an operation other than an update  *)
(* call is refused while the window is open); main_end is MainEpoch (the  *)
(* operation's work, which for an update call may open or close the       *)
(* window, then the epoch store) then MainDrop (main_calls fetch_sub).    *)
(* LookFirst scans first (MainScan), then announces.  One main thread: the *)
(* refusal of a second main operation is not modelled.                    *)
(***************************************************************************)
ScanLines == IF ScanOne THEN {CHOOSE l \in Lines : TRUE} ELSE Lines

MainAnnounce(k) ==
    /\ \/ ~LookFirst /\ armed /\ mpc = "idle" /\ mainsDone < MaxMain /\ mpc' = "announced" /\ mscan' = ScanLines
          /\ mkind' = k
       \/ LookFirst /\ mpc = "scan" /\ mscan = {} /\ mpc' = "in" /\ UNCHANGED mscan /\ k = mkind /\ UNCHANGED mkind
    /\ mainCalls' = mainCalls + 1
    /\ MainMeets
    /\ UNCHANGED <<fpc, fclient, fepoch, seen, saved, window, fillsIn, fline, epoch, word, owner, phaseIn, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, overlaps, twoFillers, refusal, takesDone>>

MainScan(k) ==
    /\ LookFirst
    /\ armed
    /\ mpc = "idle"
    /\ mainsDone < MaxMain
    /\ mpc' = "scan"
    /\ mscan' = ScanLines
    /\ mkind' = k
    /\ MainMeets
    /\ UNCHANGED <<fpc, fclient, fepoch, seen, saved, mainCalls, window, fillsIn, fline, epoch, word, owner, phaseIn, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, overlaps, twoFillers, refusal, takesDone>>

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
               /\ mkind' = "none"
               /\ mainsDone' = mainsDone + 1
               /\ Alarm(FillBusy)
               /\ mainCalls' = IF LookFirst THEN mainCalls ELSE mainCalls - 1
               /\ overlaps' = overlaps + 1
               /\ refusal' = "MainLook"
          ELSE /\ mscan' = mscan \ {l}
               /\ UNCHANGED <<mpc, mkind, mainCalls, mainsDone, falseAlarm, overlaps, refusal>>
    /\ UNCHANGED <<fpc, fclient, fepoch, seen, saved, window, fillsIn, fline, epoch, word, owner, phaseIn, fillsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, twoFillers, takesDone>>

\* ok: the window's load let the operation in: it is an update call, or the
\* window is shut. An operation refused here broke the window's rule
MainIn(ok) ==
    /\ ~LookFirst
    /\ mpc = "announced"
    /\ mscan = {}
    /\ ok = ~(window /\ mkind = "other" /\ ~NoMainWindow)
    /\ MainMeets
    /\ IF ~ok
          THEN /\ mpc' = "idle"
               /\ mkind' = "none"
               /\ mainCalls' = mainCalls - 1
               /\ mainsDone' = mainsDone + 1
               /\ overlaps' = overlaps + 1
               /\ refusal' = "MainIn"
          ELSE /\ mpc' = "in"
               /\ UNCHANGED <<mkind, mainCalls, mainsDone, overlaps, refusal>>
    /\ UNCHANGED <<mscan, fpc, fclient, fepoch, seen, saved, window, fillsIn, fline, epoch, word, owner, phaseIn, falseAlarm, fillsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, twoFillers, takesDone>>

\* An update call's work: s is the stage it leaves, the update open (an
\* update_receive) or closed (update_finish, update), and w the window it
\* leaves, which follows the stage (the message server stores it from the
\* stage as the work ends).  ShutOnEarly, the old code, stored the window
\* only in the transport's receive, which an update_receive that returned
\* early never reached: the update open, the window shut.  Any other
\* operation leaves both.
UpdateWork(k, w, s) ==
    IF k = "update"
       THEN /\ s \in BOOLEAN
            /\ IF ShutOnEarly THEN w \in {s, FALSE} ELSE w = s
       ELSE /\ s = stage
            /\ w = window

\* An update call that finds the update open (no update_finish, or a
\* refused one) finishes it, which hands the update's deliveries to the
\* game, and begins none: they wait for receive_messages.  KickAll, the old
\* code, began the next update in the same call, and its not-received check
\* ended every connection the finish had just delivered to.  The ghost
\* is the step's own (no step reads it): the invariant fails in the state
\* the kicking step leaves
KickedBy(k) == kicked' = (KickAll /\ k = "update" /\ stage)

\* the operation's work done, then the epoch moves and a new fill phase
\* begins.  w and s: the window and the stage the work left (UpdateWork)
MainEpoch(w, s) ==
    /\ mpc = "in"
    /\ UpdateWork(mkind, w, s)
    /\ mpc' = "epoched"
    /\ window' = w
    /\ stage' = s
    /\ KickedBy(mkind)
    /\ epoch' = IF ResetOnLeave THEN epoch + 1 ELSE epoch
    /\ phaseIn' = [c \in Clients |-> {}]
    /\ MainMeets
    /\ UNCHANGED <<mscan, mkind, fpc, fclient, fepoch, seen, saved, mainCalls, fillsIn, fline, word, owner, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<unchecked, armed, opted, overlaps, twoFillers, refusal, takesDone>>

MainDrop ==
    /\ mpc = "epoched"
    /\ mpc' = "idle"
    /\ mkind' = "none"
    /\ mainCalls' = mainCalls - 1
    /\ mainsDone' = mainsDone + 1
    /\ MainMeets
    /\ UNCHANGED <<mscan, fpc, fclient, fepoch, seen, saved, window, fillsIn, fline, epoch, word, owner, phaseIn, falseAlarm, fillsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, overlaps, twoFillers, refusal, takesDone>>

(***************************************************************************)
(* A filler: fill_begin is FillBegin (load the epoch), FillLock (one CAS   *)
(* that sets the in bit, refused when it is set with depth 0) or FillLoad  *)
(* then FillStore, FillDecide (refused when another filler claimed the     *)
(* client in this epoch, restoring the word), FillCount (fills_in          *)
(* fetch_add on the line l the thread hashes to), FillLook (load           *)
(* main_calls, the epoch and the window again; refused, restoring the      *)
(* word, when a main operation is running or ran since FillBegin, or the   *)
(* window is open; else the owner and the claim are stored).  A word in    *)
(* with depth 1 or more is FillPeek then FillNest.  fill_end is FillUnnest *)
(* for a nested call, else FillUnclaim (clear in) then FillLeave (fills_in *)
(* fetch_sub).  The fetch_subs are on the line the call counted on         *)
(* (fline).  LookFirst looks before it counts.                             *)
(***************************************************************************)

\* a fill step: the call meets the main operation in progress, if any
FillMeets(f) == fbroke' = [fbroke EXCEPT ![f] = @ \/ MainBusy]

\* another filler passed the check on this client in this fill phase
Second(f) == (phaseIn[fclient[f]] \ {f}) # {}

\* another filler is inside a fill call on this client
OtherOn(f) == \E g \in Fillers \ {f} : fpc[g] \in FillStates /\ fclient[g] = fclient[f]

\* a refusal of f's call is justified when the call broke the contract or met a break
Justified(f) == fbroke[f] \/ MainBusy \/ Second(f) \/ OtherOn(f) \/ window

FillRefused(f) ==
    /\ fpc' = [fpc EXCEPT ![f] = "idle"]
    /\ fillsDone' = [fillsDone EXCEPT ![f] = @ + 1]
    /\ Alarm(Justified(f))

\* the epoch loaded.  From "in", the call is one nested inside f's own for
\* the same client ("nbegun"): it loads the epoch too, and has no use for it
FillBegin(f, c) ==
    /\ armed
    /\ \/ /\ fillsDone[f] < MaxFills
          /\ fpc[f] = "idle"
          /\ fpc' = [fpc EXCEPT ![f] = "begun"]
          /\ fclient' = [fclient EXCEPT ![f] = c]
          /\ fepoch' = [fepoch EXCEPT ![f] = epoch]
          /\ fbroke' = [fbroke EXCEPT ![f] = MainBusy]
       \/ /\ fpc[f] = "in"
          /\ ~LookFirst
          /\ c = fclient[f]
          /\ fpc' = [fpc EXCEPT ![f] = "nbegun"]
          /\ FillMeets(f)
          /\ UNCHANGED <<fclient, fepoch>>
    /\ UNCHANGED <<mpc, mscan, mkind, seen, saved, mainCalls, window, fillsIn, fline, epoch, word, owner, phaseIn, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, overlaps, twoFillers, refusal, takesDone>>

\* ok: the lock was taken (the in bit was clear); else, the word in with
\* depth 0 (another call deciding), the call is refused.  A word in with
\* depth 1 or more is FillPeek's
FillLock(f, ok) ==
    LET c == fclient[f] IN
    /\ ~LoadStore
    /\ fpc[f] = "begun"
    /\ ok = ~word[c].in
    /\ ok \/ word[c].depth = 0
    /\ FillMeets(f)
    /\ IF ~ok
          THEN /\ FillRefused(f)
               /\ twoFillers' = twoFillers + 1
               /\ refusal' = "FillLock"
               /\ UNCHANGED <<seen, word>>
          ELSE /\ seen' = [seen EXCEPT ![f] = word[c]]
               /\ word' = [word EXCEPT ![c] = [@ EXCEPT !.in = TRUE]]
               /\ fpc' = [fpc EXCEPT ![f] = "locked"]
               /\ UNCHANGED <<falseAlarm, fillsDone, twoFillers, refusal>>
    /\ UNCHANGED <<mpc, mscan, mkind, fclient, fepoch, saved, mainCalls, window, fillsIn, fline, epoch, owner, phaseIn, mainsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, overlaps, takesDone>>

FillLoad(f) ==
    /\ LoadStore
    /\ fpc[f] = "begun"
    /\ fpc' = [fpc EXCEPT ![f] = "loaded"]
    /\ seen' = [seen EXCEPT ![f] = word[fclient[f]]]
    /\ FillMeets(f)
    /\ UNCHANGED <<mpc, mscan, mkind, fclient, fepoch, saved, mainCalls, window, fillsIn, fline, epoch, word, owner, phaseIn, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, overlaps, twoFillers, refusal, takesDone>>

FillStore(f) ==
    LET c == fclient[f] IN
    /\ fpc[f] = "loaded"
    /\ FillMeets(f)
    /\ IF seen[f].in
          THEN /\ FillRefused(f)
               /\ twoFillers' = twoFillers + 1
               /\ UNCHANGED word
          ELSE /\ word' = [word EXCEPT ![c] = [seen[f] EXCEPT !.in = TRUE]]
               /\ fpc' = [fpc EXCEPT ![f] = "locked"]
               /\ UNCHANGED <<falseAlarm, fillsDone, twoFillers>>
    /\ UNCHANGED <<mpc, mscan, mkind, fclient, fepoch, seen, saved, mainCalls, window, fillsIn, fline, epoch, owner, phaseIn, mainsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, overlaps, refusal, takesDone>>

\* the lock is held: only f writes the word and the owner until it clears in.
\* ok: f may claim the client; else another filler holds it in this epoch and
\* the call is refused.  The code stores the owner and the claim only once
\* its look lets it in (FillLook); OwnerAtDecide stores them here
FillDecide(f, ok) ==
    LET c == fclient[f] IN
    /\ fpc[f] = "locked"
    /\ ok = ~(owner[c] \notin {NoOwner, f} /\ seen[f].ep = fepoch[f])
    /\ FillMeets(f)
    /\ IF ~ok
          THEN /\ FillRefused(f)
               /\ twoFillers' = twoFillers + 1
               /\ refusal' = "FillDecide"
               /\ word' = [word EXCEPT ![c] = seen[f]]
               /\ UNCHANGED <<saved, owner>>
          ELSE /\ saved' = [saved EXCEPT ![f] = owner[c]]
               /\ owner' = IF OwnerAtDecide THEN [owner EXCEPT ![c] = f] ELSE owner
               /\ word' = IF OwnerAtDecide THEN [word EXCEPT ![c] = [ep |-> fepoch[f], depth |-> 1, in |-> TRUE]] ELSE word
               /\ fpc' = [fpc EXCEPT ![f] = "decided"]
               /\ UNCHANGED <<falseAlarm, fillsDone, twoFillers, refusal>>
    /\ UNCHANGED <<mpc, mscan, mkind, fclient, fepoch, seen, mainCalls, window, fillsIn, fline, epoch, phaseIn, mainsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, overlaps, takesDone>>

\* l: the line the call counted on
FillCount(f, l) ==
    LET c == fclient[f] IN
    /\ \/ ~LookFirst /\ fpc[f] = "decided" /\ fpc' = [fpc EXCEPT ![f] = "counted"] /\ UNCHANGED phaseIn
       \/ LookFirst /\ fpc[f] = "looked" /\ fpc' = [fpc EXCEPT ![f] = "in"]
          /\ phaseIn' = [phaseIn EXCEPT ![c] = @ \cup {f}]
    /\ fillsIn' = [fillsIn EXCEPT ![l] = @ + 1]
    /\ fline' = [fline EXCEPT ![f] = l]
    /\ FillMeets(f)
    /\ UNCHANGED <<mpc, mscan, mkind, fclient, fepoch, seen, saved, mainCalls, window, epoch, word, owner, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, overlaps, twoFillers, refusal, takesDone>>

\* a main operation running, or one that ran since this call loaded the
\* epoch, or the window open: refused (ok FALSE), and the word, the only
\* store the call made, goes back.  Let in, the call claims the client: the
\* owner, then the word [epoch, 1, in], both before the step's end (the
\* hook), and no other filler reads the owner until the word shows the
\* claim.  OwnerAtDecide's refusal is three stores, the word back to the
\* lock's form, the owner back, the word back (FillLook, FillUnwind1,
\* FillUnwind2), the old code's
FillLook(f, ok) ==
    LET c == fclient[f] IN
    /\ \/ ~LookFirst /\ fpc[f] = "counted"
       \/ LookFirst /\ fpc[f] = "decided"
    /\ ok = ~(mainCalls > 0 \/ (~NoEpochLook /\ epoch # fepoch[f]) \/ (~NoWindow /\ window))
    /\ FillMeets(f)
    /\ IF ~ok /\ OwnerAtDecide /\ ~NoRestore
          THEN /\ fpc' = [fpc EXCEPT ![f] = "unwind1"]
               /\ word' = [word EXCEPT ![c] = [seen[f] EXCEPT !.in = TRUE]]
               /\ UNCHANGED <<falseAlarm, fillsDone, owner, fillsIn, fline, overlaps, refusal, phaseIn>>
       ELSE IF ~ok
          THEN /\ FillRefused(f)
               /\ word' = [word EXCEPT ![c] = IF NoRestore THEN [ep |-> fepoch[f], depth |-> 0, in |-> FALSE] ELSE seen[f]]
               /\ UNCHANGED owner
               /\ fillsIn' = IF LookFirst THEN fillsIn ELSE [fillsIn EXCEPT ![fline[f]] = @ - 1]
               /\ fline' = IF LookFirst THEN fline ELSE [fline EXCEPT ![f] = NoLine]
               /\ overlaps' = overlaps + 1
               /\ refusal' = "FillLook"
               /\ UNCHANGED phaseIn
          ELSE /\ fpc' = [fpc EXCEPT ![f] = IF LookFirst THEN "looked" ELSE "in"]
               /\ phaseIn' = IF LookFirst THEN phaseIn ELSE [phaseIn EXCEPT ![c] = @ \cup {f}]
               /\ owner' = IF OwnerAtDecide THEN owner ELSE [owner EXCEPT ![c] = f]
               /\ word' = IF OwnerAtDecide THEN word ELSE [word EXCEPT ![c] = [ep |-> fepoch[f], depth |-> 1, in |-> TRUE]]
               /\ UNCHANGED <<falseAlarm, fillsDone, fillsIn, fline, overlaps, refusal>>
    /\ UNCHANGED <<mpc, mscan, mkind, fclient, fepoch, seen, saved, mainCalls, window, epoch, mainsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, twoFillers, takesDone>>

\* OwnerAtDecide's refused look, its second store: the owner put back
FillUnwind1(f) ==
    LET c == fclient[f] IN
    /\ fpc[f] = "unwind1"
    /\ fpc' = [fpc EXCEPT ![f] = "unwind2"]
    /\ owner' = [owner EXCEPT ![c] = saved[f]]
    /\ FillMeets(f)
    /\ UNCHANGED <<mpc, mscan, mkind, fclient, fepoch, seen, saved, mainCalls, window, fillsIn, fline, epoch, word, phaseIn, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, overlaps, twoFillers, refusal, takesDone>>

\* its third: the word put back, then the count dropped and the refusal counted
FillUnwind2(f) ==
    LET c == fclient[f] IN
    /\ fpc[f] = "unwind2"
    /\ FillRefused(f)
    /\ word' = [word EXCEPT ![c] = seen[f]]
    /\ fillsIn' = [fillsIn EXCEPT ![fline[f]] = @ - 1]
    /\ fline' = [fline EXCEPT ![f] = NoLine]
    /\ overlaps' = overlaps + 1
    /\ refusal' = "FillLook"
    /\ FillMeets(f)
    /\ UNCHANGED <<mpc, mscan, mkind, fclient, fepoch, seen, saved, mainCalls, window, epoch, owner, phaseIn, mainsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, twoFillers, takesDone>>

\* fill_begin's first load found the word in with depth 1 or more: a call
\* not inside one for the client ("begun") and a call nested inside f's own
\* ("nbegun") take the same step; the owner is loaded next (FillNest)
FillPeek(f) ==
    LET c == fclient[f] IN
    /\ \/ fpc[f] = "begun" /\ ~LoadStore
       \/ fpc[f] = "nbegun"
    /\ word[c].in
    /\ word[c].depth > 0
    /\ fpc' = [fpc EXCEPT ![f] = IF @ = "begun" THEN "peeked" ELSE "npeeked"]
    /\ seen' = IF fpc[f] = "begun" THEN [seen EXCEPT ![f] = word[c]] ELSE seen
    /\ FillMeets(f)
    /\ UNCHANGED <<mpc, mscan, mkind, fclient, fepoch, saved, mainCalls, window, fillsIn, fline, epoch, word, owner, phaseIn, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, overlaps, twoFillers, refusal, takesDone>>

\* the owner loaded.  ok: it is f and the depth is under DepthMax: the call is
\* nested, and the word's depth grows by one from the word it loaded.  Not
\* f: refused, counted CLIENT_SEND_TWO_THREADS.  At DepthMax: refused, API
\* misuse (a count outside the check).  A call not inside that is let in
\* ("nested") counted nothing and claimed nothing: NoFalseNest
FillNest(f, ok) ==
    LET c == fclient[f]
        mine == owner[c] = f
        from == IF fpc[f] = "peeked" THEN seen[f] ELSE word[c]
    IN
    /\ fpc[f] \in {"peeked", "npeeked"}
    /\ ok = (mine /\ from.depth < DepthMax)
    /\ FillMeets(f)
    /\ twoFillers' = IF mine THEN twoFillers ELSE twoFillers + 1
    /\ IF ok
          THEN /\ word' = [word EXCEPT ![c] = [ep |-> from.ep, depth |-> from.depth + 1, in |-> TRUE]]
               /\ UNCHANGED refusal
          ELSE /\ refusal' = "FillNest"
               /\ UNCHANGED word
    /\ IF fpc[f] = "peeked"
          THEN IF ok
                  THEN /\ fpc' = [fpc EXCEPT ![f] = "nested"]
                       /\ UNCHANGED <<falseAlarm, fillsDone>>
                  ELSE FillRefused(f)
          ELSE /\ fpc' = [fpc EXCEPT ![f] = "in"]
               \* a nested call refused for another owner would be a false alarm
               /\ Alarm(mine)
               /\ UNCHANGED fillsDone
    /\ UNCHANGED <<mpc, mscan, mkind, fclient, fepoch, seen, saved, mainCalls, window, fillsIn, fline, epoch, owner, phaseIn, mainsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, overlaps, takesDone>>

\* a nested call's end: the depth back by one
FillUnnest(f) ==
    LET c == fclient[f] IN
    /\ fpc[f] = "in"
    /\ word[c].depth > 1
    /\ word' = [word EXCEPT ![c] = [@ EXCEPT !.depth = @ - 1]]
    /\ FillMeets(f)
    /\ UNCHANGED <<mpc, mscan, mkind, fpc, fclient, fepoch, seen, saved, mainCalls, window, fillsIn, fline, epoch, owner, phaseIn, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, overlaps, twoFillers, refusal, takesDone>>

FillUnclaim(f) ==
    LET c == fclient[f] IN
    /\ fpc[f] = "in"
    /\ word[c].depth <= 1
    /\ fpc' = [fpc EXCEPT ![f] = "unclaimed"]
    /\ word' = [word EXCEPT ![c] = [@ EXCEPT !.depth = 0, !.in = FALSE]]
    /\ FillMeets(f)
    /\ UNCHANGED <<mpc, mscan, mkind, fclient, fepoch, seen, saved, mainCalls, window, fillsIn, fline, epoch, owner, phaseIn, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, overlaps, twoFillers, refusal, takesDone>>

FillLeave(f) ==
    /\ fpc[f] = "unclaimed"
    /\ fpc' = [fpc EXCEPT ![f] = "idle"]
    /\ fillsIn' = [fillsIn EXCEPT ![fline[f]] = @ - 1]
    /\ fline' = [fline EXCEPT ![f] = NoLine]
    /\ fillsDone' = [fillsDone EXCEPT ![f] = @ + 1]
    /\ FillMeets(f)
    /\ UNCHANGED <<mpc, mscan, mkind, fclient, fepoch, seen, saved, mainCalls, window, epoch, word, owner, phaseIn, falseAlarm, mainsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, overlaps, twoFillers, refusal, takesDone>>

(***************************************************************************)
(* A take (update_client): take_begin is TakeCount (fills_in fetch_add on  *)
(* the thread's line) then TakeLook (main_calls load; refused, the count   *)
(* dropped, when a main operation runs); take_end is TakeLeave (fills_in  *)
(* fetch_sub).  A take claims no client.  TakeUncounted takes no count, so *)
(* a main operation's look cannot see it.                                  *)
(***************************************************************************)
TakeCount(f, l) ==
    /\ armed
    /\ takesDone[f] < MaxTakes
    /\ fpc[f] = "idle"
    /\ fpc' = [fpc EXCEPT ![f] = "tcounted"]
    /\ fillsIn' = IF TakeUncounted THEN fillsIn ELSE [fillsIn EXCEPT ![l] = @ + 1]
    /\ fline' = [fline EXCEPT ![f] = l]
    /\ fbroke' = [fbroke EXCEPT ![f] = MainBusy]
    /\ UNCHANGED <<mpc, mscan, mkind, fclient, fepoch, seen, saved, mainCalls, window, epoch, word, owner, phaseIn, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, overlaps, twoFillers, refusal, takesDone>>

TakeLook(f, ok) ==
    /\ fpc[f] = "tcounted"
    /\ ok = (mainCalls = 0)
    /\ FillMeets(f)
    /\ IF ~ok
          THEN /\ fpc' = [fpc EXCEPT ![f] = "idle"]
               /\ takesDone' = [takesDone EXCEPT ![f] = @ + 1]
               /\ fillsIn' = IF TakeUncounted THEN fillsIn ELSE [fillsIn EXCEPT ![fline[f]] = @ - 1]
               /\ fline' = [fline EXCEPT ![f] = NoLine]
               /\ Alarm(fbroke[f] \/ MainBusy)
               /\ overlaps' = overlaps + 1
               /\ refusal' = "TakeLook"
          ELSE /\ fpc' = [fpc EXCEPT ![f] = "tin"]
               /\ UNCHANGED <<takesDone, fillsIn, fline, falseAlarm, overlaps, refusal>>
    /\ UNCHANGED <<mpc, mscan, mkind, fclient, fepoch, seen, saved, mainCalls, window, epoch, word, owner, phaseIn, mainsDone, fillsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, twoFillers>>

TakeLeave(f) ==
    /\ fpc[f] = "tin"
    /\ fpc' = [fpc EXCEPT ![f] = "idle"]
    /\ fillsIn' = IF TakeUncounted THEN fillsIn ELSE [fillsIn EXCEPT ![fline[f]] = @ - 1]
    /\ fline' = [fline EXCEPT ![f] = NoLine]
    /\ takesDone' = [takesDone EXCEPT ![f] = @ + 1]
    /\ FillMeets(f)
    /\ UNCHANGED <<mpc, mscan, mkind, fclient, fepoch, seen, saved, mainCalls, window, epoch, word, owner, phaseIn, falseAlarm, mainsDone, fillsDone>>
    /\ UNCHANGED <<stage, kicked, unchecked, armed, opted, overlaps, twoFillers, refusal>>

\* The latch.  The server creates the check not armed, and the game's first
\* call that names a client, or its first update_receive, arms it, for good
\* (Arm).  Not armed, a call's scope does nothing: the whole call is one
\* step that touches no word of the check (FillSkip, TakeSkip, MainSkip; an
\* update call's work may still open or close the window, MainSkip's w).  A
\* call that began armed runs the check to its end.  A call that began
\* before Arm and is still running after it is not in the model: the check
\* does not see it.
Arm ==
    /\ ~opted
    /\ opted' = TRUE
    /\ armed' = TRUE
    /\ UNCHANGED <<mpc, mscan, mkind, fpc, fclient, fepoch, seen, saved, mainCalls, window, stage, fillsIn, fline, epoch, word, owner,
                   overlaps, twoFillers, fbroke, phaseIn, falseAlarm, refusal, kicked, unchecked, mainsDone, fillsDone, takesDone>>

FillSkip(f) ==
    /\ ~armed
    /\ fpc[f] = "idle"
    /\ fillsDone[f] < MaxFills
    /\ fillsDone' = [fillsDone EXCEPT ![f] = @ + 1]
    /\ UNCHANGED <<mpc, mscan, mkind, fpc, fclient, fepoch, seen, saved, mainCalls, window, stage, fillsIn, fline, epoch, word, owner,
                   armed, overlaps, twoFillers, opted, fbroke, phaseIn, falseAlarm, refusal, kicked, unchecked, mainsDone, takesDone>>

TakeSkip(f) ==
    /\ ~armed
    /\ fpc[f] = "idle"
    /\ takesDone[f] < MaxTakes
    /\ takesDone' = [takesDone EXCEPT ![f] = @ + 1]
    /\ UNCHANGED <<mpc, mscan, mkind, fpc, fclient, fepoch, seen, saved, mainCalls, window, stage, fillsIn, fline, epoch, word, owner,
                   armed, overlaps, twoFillers, opted, fbroke, phaseIn, falseAlarm, refusal, kicked, unchecked, mainsDone, fillsDone>>

\* a main operation of kind k made not armed: its work only (UpdateWork)
MainSkip(k, w, s) ==
    /\ ~armed
    /\ mpc = "idle"
    /\ mainsDone < MaxMain
    /\ UpdateWork(k, w, s)
    /\ window' = w
    /\ stage' = s
    /\ KickedBy(k)
    /\ mainsDone' = mainsDone + 1
    /\ UNCHANGED <<mscan, mpc, mkind, fpc, fclient, fepoch, seen, saved, mainCalls, fillsIn, fline, epoch, word, owner,
                   armed, overlaps, twoFillers, opted, fbroke, phaseIn, falseAlarm, refusal, unchecked, fillsDone, takesDone>>

\* ScanFirst, the old plain finish and abort (the forms that name no
\* client): armed, the call scanned every connection for the open message
\* before any check, and checked only when another thread held the
\* client.  The step is that scan, with no check: in the window, or beside
\* a fill call past its check, it ran unrefused and uncounted.  The code
\* now makes the call a main operation of kind "other" (MainAnnounce ..
\* MainDrop), the scan its work
PlainScan ==
    /\ ScanFirst
    /\ armed
    /\ mpc = "idle"
    /\ mainsDone < MaxMain
    /\ unchecked' = (window \/ \E f \in Fillers : fpc[f] \in PastCheck)
    /\ mainsDone' = mainsDone + 1
    /\ UNCHANGED <<mscan, mpc, mkind, fpc, fclient, fepoch, seen, saved, mainCalls, window, stage, fillsIn, fline, epoch, word, owner,
                   armed, overlaps, twoFillers, opted, fbroke, phaseIn, falseAlarm, refusal, kicked, fillsDone, takesDone>>

Done ==
    /\ mpc = "idle" /\ mainsDone = MaxMain
    /\ \A f \in Fillers : fpc[f] = "idle" /\ fillsDone[f] = MaxFills /\ takesDone[f] = MaxTakes
    \* every variable by name, not vars: the ghost refusal is read by no step
    /\ UNCHANGED <<mpc, mscan, mkind, fpc, fclient, fepoch, seen, saved, mainCalls, window, stage, fillsIn, fline, epoch, word, owner,
                   armed, overlaps, twoFillers, opted, fbroke, phaseIn, falseAlarm, refusal, kicked, unchecked, mainsDone, fillsDone,
                   takesDone>>

Next ==
    \/ \E k \in Kinds : MainAnnounce(k) \/ MainScan(k)
    \/ \E ok \in BOOLEAN : MainIn(ok)
    \/ \E w, s \in BOOLEAN : MainEpoch(w, s) \/ \E k \in Kinds : MainSkip(k, w, s)
    \/ MainDrop
    \/ PlainScan
    \/ \E l \in Lines, ok \in BOOLEAN : MainLook(l, ok)
    \/ \E f \in Fillers, c \in Clients : FillBegin(f, c)
    \/ \E f \in Fillers, ok \in BOOLEAN : FillLock(f, ok) \/ FillDecide(f, ok) \/ FillLook(f, ok) \/ FillNest(f, ok) \/ TakeLook(f, ok)
    \/ \E f \in Fillers : \E l \in LineChoice(f) : FillCount(f, l) \/ TakeCount(f, l)
    \/ \E f \in Fillers : FillLoad(f) \/ FillStore(f) \/ FillUnclaim(f) \/ FillLeave(f) \/ FillSkip(f) \/ TakeLeave(f) \/ TakeSkip(f)
    \/ \E f \in Fillers : FillPeek(f) \/ FillUnnest(f) \/ FillUnwind1(f) \/ FillUnwind2(f)
    \/ Arm
    \/ Done

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Properties                                                              *)
(***************************************************************************)

\* a main operation and a fill call are never both past their checks
Sound == ~(mpc \in {"in", "epoched"} /\ \E f \in Fillers : fpc[f] \in PastCheck)

\* a main operation and a take are never both past their checks
TakeSound == ~(mpc \in {"in", "epoched"} /\ \E f \in Fillers : fpc[f] = "tin")

\* with the window open no fill call and no main operation but an update
\* call is past its check
WindowShut == window => /\ \A f \in Fillers : fpc[f] \notin PastCheck
                        /\ ~(mpc \in {"in", "epoched"} /\ mkind = "other")

\* while the update is open (update_receive to update_finish, whatever the
\* transport did) no fill call and no main operation but an update call is
\* past its check: the window follows the stage (MCRNFillPhaseBrokenShutOnEarly)
StageShut == stage => /\ \A f \in Fillers : fpc[f] \notin PastCheck
                      /\ ~(mpc \in {"in", "epoched"} /\ mkind = "other")

\* no update call ends a connection for a delivery the game never had the
\* chance to receive (MCRNFillPhaseBrokenKickAll)
NoKickOnOpenUpdate == ~kicked

\* the plain finish and abort never run beside a fill call past its check,
\* nor in the window, unrefused (MCRNFillPhaseBrokenScanFirst)
PlainChecked == ~unchecked

\* in one fill phase, the calls on a client that pass the check are one filler's
OneFiller == \A c \in Clients : Cardinality(phaseIn[c]) <= 1

\* every refusal is of a call or operation that broke the contract or met a break
NoFalseAlarm == ~falseAlarm

\* a fill call not inside one for its client is never taken as nested: the
\* nested test's two loads never pair a claim's word with the caller's own
\* id as owner (MCRNFillPhaseBrokenOwnerAtDecide)
NoFalseNest == \A f \in Fillers : fpc[f] # "nested"

\* a client's word shows a claim (depth 1 or more) only while its owner is
\* inside a fill call for it
ClaimHasOwner == \A c \in Clients : word[c].depth > 0 =>
                     /\ owner[c] \in Fillers
                     /\ fclient[owner[c]] = c
                     /\ fpc[owner[c]] \in {"decided", "counted", "looked", "in", "nbegun", "npeeked", "unwind1"}

\* before the game opts in no call is inside the check, so none is refused or counted
NoCheckBeforeOptIn == ~opted => (mpc = "idle" /\ \A f \in Fillers : fpc[f] = "idle")

\* the latch only arms
ArmedStays == [][armed => armed']_vars

=============================================================================
