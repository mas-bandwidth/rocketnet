----------------------------- MODULE RNMsgxPause -----------------------------
EXTENDS Naturals

\* One socket's TX and RX workers share an atomic pause expiry. Time is a
\* monotone bounded abstraction of rocketnet_platform_time; a retry is positive.
\* A failed compare-and-swap reloads the actual value into the local observation.
CONSTANTS Workers, MaxTime, Retry, CompareBeforeClear
ASSUME /\ Workers # {} /\ MaxTime >= Retry /\ Retry > 0
       /\ CompareBeforeClear \in BOOLEAN

VARIABLES now, down_until, observed, step, clearedFresh
vars == <<now, down_until, observed, step, clearedFresh>>

Init == /\ now = 0 /\ down_until = 0
        /\ observed = [w \in Workers |-> 0]
        /\ step = [w \in Workers |-> "ready"]
        /\ clearedFresh = FALSE

\* An eligible batch begins a probe. Its syscall may succeed or refuse.
Probe(w) == /\ step[w] = "ready" /\ down_until = 0
            /\ step' = [step EXCEPT ![w] = "probe"]
            /\ UNCHANGED <<now, down_until, observed, clearedFresh>>
FinishProbe(w) == /\ step[w] = "probe"
                  /\ step' = [step EXCEPT ![w] = "ready"]
                  /\ UNCHANGED <<now, down_until, observed, clearedFresh>>

\* A refused syscall publishes its retry deadline, even if another worker
\* has already loaded an expired deadline and is about to clear it.
Pause(w) == /\ step[w] = "probe"
            /\ down_until' = now + Retry
            /\ step' = [step EXCEPT ![w] = "ready"]
            /\ UNCHANGED <<now, observed, clearedFresh>>

\* Environment action: time advances, permitting a previously paused call.
Expire == /\ now < MaxTime /\ now' = now + 1
          /\ UNCHANGED <<down_until, observed, step, clearedFresh>>

ObserveExpired(w) == /\ step[w] = "ready"
                     /\ down_until > 0 /\ down_until <= now
                     /\ observed' = [observed EXCEPT ![w] = down_until]
                     /\ step' = [step EXCEPT ![w] = "clear"]
                     /\ UNCHANGED <<now, down_until, clearedFresh>>

ClearIfUnchanged(w) ==
    /\ step[w] = "clear"
    /\ LET clears == ~CompareBeforeClear \/ down_until = observed[w]
       IN /\ down_until' = IF clears THEN 0 ELSE down_until
          \* History detects actual erasure of a newer deadline, independently
          \* of the rule switch. A newer pause need not still be unexpired.
          /\ clearedFresh' = (clearedFresh \/ (clears /\ down_until > observed[w]))
          /\ observed' = [observed EXCEPT ![w] = IF clears THEN @ ELSE down_until]
          /\ step' = [step EXCEPT ![w] =
                IF clears \/ down_until = 0 THEN "probe"
                ELSE IF down_until > now THEN "ready" ELSE "clear"]
    /\ UNCHANGED now

\* A call during a live pause returns inactive, without changing shared state.
PollPaused(w) == /\ step[w] = "ready" /\ down_until > now
                 /\ UNCHANGED vars

Next == Expire \/ (\E w \in Workers :
    Probe(w) \/ FinishProbe(w) \/ Pause(w) \/ ObserveExpired(w) \/
    ClearIfUnchanged(w) \/ PollPaused(w))
Spec == Init /\ [][Next]_vars
SpecFair == Spec /\ WF_vars(Expire) /\ (\A w \in Workers :
    WF_vars(Probe(w)) /\ WF_vars(FinishProbe(w)) /\
    WF_vars(ObserveExpired(w)) /\ WF_vars(ClearIfUnchanged(w)))

TypeOK == /\ now \in 0..MaxTime /\ down_until \in 0..(MaxTime + Retry)
          /\ observed \in [Workers -> 0..(MaxTime + Retry)]
          /\ step \in [Workers -> {"ready", "clear", "probe"}]
          /\ clearedFresh \in BOOLEAN
FreshPauseSurvives == ~clearedFresh
ExpiredClearedOrReplaced == \A expiry \in 1..(MaxTime + Retry) :
    (down_until = expiry /\ now >= expiry) ~> (down_until # expiry)
=============================================================================
