----------------------------- MODULE RNMsgxPause -----------------------------
(***************************************************************************)
(* The msgx pause machine of sdk/rocketnet_platform.cpp:                   *)
(* rocketnet_msgx_pause and rocketnet_msgx_active. Two workers share one   *)
(* socket. down_until is 0, or the time at which a pause expires. Each     *)
(* worker has the expiry it last observed and a step: idle, hold (an       *)
(* expired value loaded, the compare-and-swap not yet done) or probe (the  *)
(* pump was clear, so the batch attempt is the re-probe).                  *)
(*                                                                         *)
(* Pause stores now + Retry, an unconditional publish. Expire advances the *)
(* clock by one tick, up to MaxTime. ObserveExpired loads a value the      *)
(* clock has already passed. ClearIfUnchanged stores 0 only when the slot  *)
(* still holds that observed value. Probe is the live check that finds 0.  *)
(*                                                                         *)
(* FreshPauseSurvives: a pause published after a worker observed an older  *)
(* expiry is never cleared by that worker's clear. The reversed witness    *)
(* sets CasClearsOnlyObserved to FALSE, so the clear stores 0 whether or   *)
(* not the slot still holds the observed value.                            *)
(*                                                                         *)
(* ExpiredPauseSettled: once the clock has reached an expiry inside        *)
(* 1..MaxTime, that value is cleared or replaced. Done is the terminal     *)
(* stutter after MaxTime when no clear is in progress and nothing expired  *)
(* is still sitting in the slot; it keeps a finished run from looking like *)
(* a stall.                                                                *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Workers, MaxTime, Retry, CasClearsOnlyObserved

ASSUME /\ Cardinality(Workers) = 2
       /\ MaxTime \in Nat /\ MaxTime >= 2
       /\ Retry \in Nat /\ Retry >= 1
       /\ CasClearsOnlyObserved \in BOOLEAN

Times    == 0..MaxTime
Expiries == 0..(MaxTime + Retry)
Steps    == {"idle", "hold", "probe"}

VARIABLES now, down_until, observed, step, cleared_unobserved

vars == <<now, down_until, observed, step, cleared_unobserved>>

Init ==
    /\ now = 0
    /\ down_until = 0
    /\ observed = [w \in Workers |-> 0]
    /\ step = [w \in Workers |-> "idle"]
    /\ cleared_unobserved = FALSE

\* rocketnet_msgx_pause: publish now + the retry window. A worker does this
\* from outside the load/clear gap, so not while it is in hold.
Pause(w) ==
    /\ step[w] \in {"idle", "probe"}
    /\ down_until' = now + Retry
    /\ step' = [step EXCEPT ![w] = "idle"]
    /\ UNCHANGED <<now, observed, cleared_unobserved>>

\* The clock. One tick; an expiry is reached when now catches it.
Expire ==
    /\ now < MaxTime
    /\ now' = now + 1
    /\ UNCHANGED <<down_until, observed, step, cleared_unobserved>>

\* The load inside rocketnet_msgx_active, in the branch that sees an expiry
\* the clock has already passed.
ObserveExpired(w) ==
    /\ step[w] \in {"idle", "probe"}
    /\ down_until > 0
    /\ now >= down_until
    /\ observed' = [observed EXCEPT ![w] = down_until]
    /\ step' = [step EXCEPT ![w] = "hold"]
    /\ UNCHANGED <<now, down_until, cleared_unobserved>>

\* The compare-and-swap. On a match, store 0 and the call is the re-probe.
\* On a mismatch the rule leaves the slot alone: 0 means the other worker
\* already cleared, a later expiry means a fresh pause won. The reversed
\* witness stores 0 on a mismatch too, and that is the only step that sets
\* cleared_unobserved.
ClearIfUnchanged(w) ==
    /\ step[w] = "hold"
    /\ \/ /\ down_until = observed[w]
          /\ down_until' = 0
          /\ step' = [step EXCEPT ![w] = "probe"]
          /\ UNCHANGED <<now, observed, cleared_unobserved>>
       \/ /\ down_until # observed[w]
          /\ CasClearsOnlyObserved
          /\ step' = [step EXCEPT ![w] =
                          IF down_until = 0 THEN "probe" ELSE "idle"]
          /\ UNCHANGED <<now, down_until, observed, cleared_unobserved>>
       \/ /\ down_until # observed[w]
          /\ ~CasClearsOnlyObserved
          /\ down_until' = 0
          /\ step' = [step EXCEPT ![w] = "probe"]
          /\ cleared_unobserved' = cleared_unobserved \/ (down_until # 0)
          /\ UNCHANGED <<now, observed>>

\* rocketnet_msgx_active finding 0: the pump is clear, so this batch attempt
\* is the re-probe. A worker already in probe has recorded that.
Probe(w) ==
    /\ step[w] = "idle"
    /\ down_until = 0
    /\ step' = [step EXCEPT ![w] = "probe"]
    /\ UNCHANGED <<now, down_until, observed, cleared_unobserved>>

\* Clock finished, no load/clear gap open, and the slot is not holding an
\* expiry the clock has reached. The run stutters.
Done ==
    /\ now = MaxTime
    /\ \A w \in Workers : step[w] # "hold"
    /\ down_until = 0 \/ now < down_until
    /\ UNCHANGED vars

Next ==
    \/ Expire
    \/ Done
    \/ \E w \in Workers :
          Pause(w) \/ ObserveExpired(w) \/ ClearIfUnchanged(w) \/ Probe(w)

Fairness ==
    /\ WF_vars(Expire)
    /\ \A w \in Workers : /\ WF_vars(ObserveExpired(w))
                         /\ WF_vars(ClearIfUnchanged(w))

Spec == Init /\ [][Next]_vars /\ Fairness

TypeOK ==
    /\ now \in Times
    /\ down_until \in Expiries
    /\ observed \in [Workers -> Expiries]
    /\ step \in [Workers -> Steps]
    /\ cleared_unobserved \in BOOLEAN

\* A pause published after a worker observed an older expiry is never
\* cleared by that worker's clear. The bad clear is the only writer of
\* cleared_unobserved.
FreshPauseSurvives == ~cleared_unobserved

\* An expiry the clock can reach is cleared or replaced.
ExpiredPauseSettled ==
    \A t \in 1..MaxTime :
        (down_until = t /\ now >= t) ~> (down_until # t)

=============================================================================
