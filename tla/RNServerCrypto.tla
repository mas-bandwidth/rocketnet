---------------------------- MODULE RNServerCrypto ----------------------------
(***************************************************************************)
(* Server packet AEAD leaves the game thread on a three-bank handoff.     *)
(* sdk/rocketnet_server_crypto.cpp.                                       *)
(*                                                                         *)
(* One bank is FILL: the game writes jobs into it and does not seal them. *)
(* Publish moves that bank to READY and clears the fill index. The worker *)
(* takes the oldest READY bank (WorkerTake), runs AEAD outside the lock   *)
(* (WorkerSeal), then marks it DONE (WorkerDone). The game takes a DONE   *)
(* bank only when it is the next ordinal (GameTake) and otherwise skips.  *)
(* A skip is not a wait. A full set of banks refuses another claim; the   *)
(* caller keeps the packet. No action rewrites a READY, RUN or DONE bank. *)
(*                                                                         *)
(* The mutex in the code covers only the bank state word and the fill     *)
(* index. This model has no lock variable: WorkerSeal is a separate       *)
(* action from WorkerTake and WorkerDone, so the seal is not the exchange.*)
(*                                                                         *)
(* NeverWaits FALSE enables GameWait, which records that the game spun    *)
(* on a bank that is not DONE. That is the reversed witness for           *)
(* WaitedStaysClear. BrokenOverwrite FALSE is the rule; TRUE wipes a      *)
(* READY bank's jobs without taking them, and Accounted catches it.       *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS NumBanks, Cap, MaxJobs, NeverWaits, BrokenOverwrite

ASSUME NumBanks = 3 /\ Cap >= 1 /\ MaxJobs >= 1
ASSUME NeverWaits \in BOOLEAN /\ BrokenOverwrite \in BOOLEAN

Banks == 1..NumBanks
Jobs == 1..MaxJobs

FREE == "FREE"
FILL == "FILL"
READY == "READY"
RUN == "RUN"
DONE == "DONE"

VARIABLES
    state, filling, running, ord, jobs, nextOrd, nextTake,
    sealed, taken, submitted, waited

vars == <<state, filling, running, ord, jobs, nextOrd, nextTake,
          sealed, taken, submitted, waited>>

Range(s) == { s[i] : i \in 1..Len(s) }

TypeOK ==
    /\ state \in [Banks -> {FREE, FILL, READY, RUN, DONE}]
    /\ filling \in Banks \cup {0}
    /\ running \in Banks \cup {0}
    /\ ord \in [Banks -> Nat]
    /\ jobs \in [Banks -> Seq(Jobs)]
    /\ nextOrd \in Nat /\ nextTake \in Nat
    /\ sealed \subseteq Jobs
    /\ taken \in Seq(Jobs)
    /\ submitted \in 0..MaxJobs
    /\ waited \in {0, 1}
    /\ \A b \in Banks : Len(jobs[b]) <= Cap

Init ==
    /\ state = [b \in Banks |-> FREE]
    /\ filling = 0 /\ running = 0
    /\ ord = [b \in Banks |-> 0]
    /\ jobs = [b \in Banks |-> <<>>]
    /\ nextOrd = 1 /\ nextTake = 1
    /\ sealed = {} /\ taken = <<>>
    /\ submitted = 0 /\ waited = 0

Exclusive ==
    /\ filling = 0 \/ state[filling] = FILL
    /\ running = 0 \/ state[running] = RUN
    /\ filling = 0 \/ running = 0 \/ filling /= running
    /\ \A b \in Banks : state[b] = FILL <=> b = filling
    /\ \A b \in Banks : state[b] = RUN <=> b = running
    /\ \A b1, b2 \in Banks :
        (b1 /= b2 /\ state[b1] /= FREE /\ state[b2] /= FREE) => ord[b1] /= ord[b2]

\* A published bank still holds the jobs the game wrote. GameTake is the
\* only action that clears them, and it records them in taken.
Accounted ==
    LET live == UNION { Range(jobs[b]) : b \in Banks }
    IN submitted = Cardinality(live) + Len(taken)
       /\ \A i \in 1..Len(taken) : taken[i] \notin live

\* Taken jobs were sealed, and they stay in submit order.
Order ==
    /\ \A i \in 1..Len(taken) : taken[i] \in sealed
    /\ \A i, j \in 1..Len(taken) : i < j => taken[i] < taken[j]

WaitedStaysClear == waited = 0

NoWorkUnderLock == running = 0 \/ state[running] = RUN

Claim(b) ==
    /\ filling = 0
    /\ state[b] = FREE
    /\ state' = [state EXCEPT ![b] = FILL]
    /\ filling' = b
    /\ jobs' = [jobs EXCEPT ![b] = <<>>]
    /\ ord' = [ord EXCEPT ![b] = 0]
    /\ UNCHANGED <<running, nextOrd, nextTake, sealed, taken, submitted, waited>>

Fill ==
    /\ filling /= 0
    /\ Len(jobs[filling]) < Cap
    /\ submitted < MaxJobs
    /\ jobs' = [jobs EXCEPT ![filling] = Append(jobs[filling], submitted + 1)]
    /\ submitted' = submitted + 1
    /\ UNCHANGED <<state, filling, running, ord, nextOrd, nextTake, sealed, taken, waited>>

ReleaseEmpty ==
    /\ filling /= 0
    /\ Len(jobs[filling]) = 0
    /\ state' = [state EXCEPT ![filling] = FREE]
    /\ filling' = 0
    /\ UNCHANGED <<running, ord, jobs, nextOrd, nextTake, sealed, taken, submitted, waited>>

Publish ==
    /\ filling /= 0
    /\ Len(jobs[filling]) > 0
    /\ state' = [state EXCEPT ![filling] = READY]
    /\ ord' = [ord EXCEPT ![filling] = nextOrd]
    /\ nextOrd' = nextOrd + 1
    /\ filling' = 0
    /\ UNCHANGED <<running, jobs, nextTake, sealed, taken, submitted, waited>>

WorkerTake(b) ==
    /\ running = 0
    /\ state[b] = READY
    /\ \A c \in Banks : state[c] = READY => ord[b] <= ord[c]
    /\ state' = [state EXCEPT ![b] = RUN]
    /\ running' = b
    /\ UNCHANGED <<filling, ord, jobs, nextOrd, nextTake, sealed, taken, submitted, waited>>

WorkerSeal ==
    /\ running /= 0
    /\ \E i \in 1..Len(jobs[running]) : jobs[running][i] \notin sealed
    /\ sealed' = sealed \cup Range(jobs[running])
    /\ UNCHANGED <<state, filling, running, ord, jobs, nextOrd, nextTake, taken, submitted, waited>>

WorkerDone ==
    /\ running /= 0
    /\ Range(jobs[running]) \subseteq sealed
    /\ state' = [state EXCEPT ![running] = DONE]
    /\ running' = 0
    /\ UNCHANGED <<filling, ord, jobs, nextOrd, nextTake, sealed, taken, submitted, waited>>

GameTake(b) ==
    /\ state[b] = DONE
    /\ ord[b] = nextTake
    /\ taken' = taken \o jobs[b]
    /\ state' = [state EXCEPT ![b] = FREE]
    /\ jobs' = [jobs EXCEPT ![b] = <<>>]
    /\ nextTake' = nextTake + 1
    /\ UNCHANGED <<filling, running, ord, nextOrd, sealed, submitted, waited>>

\* The game spins because the next bank is not DONE. Disabled when NeverWaits.
GameWait ==
    /\ ~NeverWaits
    /\ \A b \in Banks : state[b] = DONE => ord[b] /= nextTake
    /\ \E b \in Banks : state[b] \in {READY, RUN} /\ ord[b] = nextTake
    /\ waited' = 1
    /\ UNCHANGED <<state, filling, running, ord, jobs, nextOrd, nextTake, sealed, taken, submitted>>

\* Wipes a published bank. Disabled when BrokenOverwrite is FALSE.
Overwrite(b) ==
    /\ BrokenOverwrite
    /\ state[b] = READY
    /\ jobs' = [jobs EXCEPT ![b] = <<>>]
    /\ state' = [state EXCEPT ![b] = FREE]
    /\ UNCHANGED <<filling, running, ord, nextOrd, nextTake, sealed, taken, submitted, waited>>

Next ==
    \/ \E b \in Banks : Claim(b)
    \/ Fill
    \/ ReleaseEmpty
    \/ Publish
    \/ \E b \in Banks : WorkerTake(b)
    \/ WorkerSeal
    \/ WorkerDone
    \/ \E b \in Banks : GameTake(b)
    \/ GameWait
    \/ \E b \in Banks : Overwrite(b)

Spec == Init /\ [][Next]_vars
=============================================================================
