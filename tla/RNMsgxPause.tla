----------------------------- MODULE RNMsgxPause -----------------------------
(***************************************************************************)
(* The macOS msgx pump pause and re-probe of sdk/rocketnet_platform.cpp:   *)
(* rocketnet_msgx_pause, rocketnet_msgx_active and the batch calls that    *)
(* follow it.                                                              *)
(*                                                                         *)
(* One socket carries one shared word, msgx_down_until: 0 when the batch   *)
(* pump is live, or the time a pause expires.  A refused batch call        *)
(* (sendmsg_x or recvmsg_x with an errno that is not weather) stores       *)
(* now + retry unconditionally.  Before every batch call a worker loads    *)
(* the word.  A pause that has not expired sends the caller to the         *)
(* per-packet path.  An expired pause is cleared by a compare-and-swap     *)
(* that succeeds only while the word still holds the value this worker     *)
(* observed; a failed swap reloads the word and tests it again.  The       *)
(* first batch call after a clear is the re-probe.                         *)
(*                                                                         *)
(* The server's TX and RX workers run these steps on one socket at once,   *)
(* so the rule checked is: a pause the other worker publishes after this   *)
(* worker observed an older, expired value is never removed by this        *)
(* worker's clear.                                                          *)
(*                                                                         *)
(* The ghost removed (not in the code) is the value the word held when    *)
(* the last clear ran.  A pause published after a worker observed an       *)
(* older expiry is fresh: its expiry is later than the time of the clear.  *)
(* Two pauses stored at the same instant carry the same expiry; the        *)
(* compare-and-swap cannot tell them apart and need not, because a clear   *)
(* of either removes an expired value.                                     *)
(*                                                                         *)
(* Rule switch, TRUE only in the reversed witness:                         *)
(*   StoreZero  the expired pause is cleared by an unconditional store of  *)
(*              0 instead of the compare-and-swap.                          *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS Workers,   \* the threads that run batch calls on one socket
          MaxTime,   \* time is bounded to 0..MaxTime ticks
          Retry,     \* ROCKETNET_MSGX_RETRY_SECONDS, in ticks
          StoreZero  \* the broken clear, TRUE only in the reversed witness

ASSUME MaxTime >= 1 /\ Retry >= 1 /\ StoreZero \in BOOLEAN

Times == 0..(MaxTime + Retry)

VARIABLES now,      \* rocketnet_platform_time, in ticks
          down,     \* msgx_down_until: 0, or the expiry of a pause
          removed,  \* ghost: the value the word held when the last clear ran
          pc,       \* per worker: idle, loaded, expired or probe
          seen      \* per worker: the value it loaded

vars == <<now, down, removed, pc, seen>>

Steps == {"idle", "loaded", "expired", "probe"}

TypeOK ==
    /\ now \in 0..MaxTime
    /\ down \in Times
    /\ removed \in Times
    /\ pc \in [Workers -> Steps]
    /\ seen \in [Workers -> Times]

Init ==
    /\ now = 0
    /\ down = 0
    /\ removed = 0
    /\ pc = [w \in Workers |-> "idle"]
    /\ seen = [w \in Workers |-> 0]

\* Time advances one tick: a pause whose expiry is reached is expired.
Expire ==
    /\ now < MaxTime
    /\ now' = now + 1
    /\ UNCHANGED <<down, removed, pc, seen>>

\* rocketnet_msgx_active loads msgx_down_until.  0 is the live pump: the
\* batch call follows at once.
Load(w) ==
    /\ pc[w] = "idle"
    /\ seen' = [seen EXCEPT ![w] = down]
    /\ pc' = [pc EXCEPT ![w] = IF down = 0 THEN "probe" ELSE "loaded"]
    /\ UNCHANGED <<now, down, removed>>

\* A pause that has not expired: rocketnet_msgx_active returns false and
\* the caller takes the per-packet path.
StayPaused(w) ==
    /\ pc[w] = "loaded"
    /\ now < seen[w]
    /\ pc' = [pc EXCEPT ![w] = "idle"]
    /\ UNCHANGED <<now, down, removed, seen>>

\* The observed pause has expired (where the test hook
\* rocketnet_msgx_before_expiry_clear runs, before the swap).
ObserveExpired(w) ==
    /\ pc[w] = "loaded"
    /\ now >= seen[w]
    /\ pc' = [pc EXCEPT ![w] = "expired"]
    /\ UNCHANGED <<now, down, removed, seen>>

\* The compare-and-swap: clear only the value this worker observed.  On a
\* failed swap the word is reloaded into seen and tested again (0 is live).
ClearIfUnchanged(w) ==
    /\ ~StoreZero
    /\ pc[w] = "expired"
    /\ IF down = seen[w]
          THEN /\ down' = 0
               /\ removed' = down
               /\ pc' = [pc EXCEPT ![w] = "probe"]
               /\ UNCHANGED seen
          ELSE /\ seen' = [seen EXCEPT ![w] = down]
               /\ pc' = [pc EXCEPT ![w] = IF down = 0 THEN "probe" ELSE "loaded"]
               /\ UNCHANGED <<down, removed>>
    /\ UNCHANGED now

\* The reversed witness: an unconditional store of 0.
ClearUnconditional(w) ==
    /\ StoreZero
    /\ pc[w] = "expired"
    /\ down' = 0
    /\ removed' = down
    /\ pc' = [pc EXCEPT ![w] = "probe"]
    /\ UNCHANGED <<now, seen>>

\* The batch call (the re-probe after a clear) succeeds, or is refused for
\* weather, which never pauses.
Probe(w) ==
    /\ pc[w] = "probe"
    /\ pc' = [pc EXCEPT ![w] = "idle"]
    /\ UNCHANGED <<now, down, removed, seen>>

\* rocketnet_msgx_pause: the batch call was refused; store now + retry
\* unconditionally, whatever the word holds.
Pause(w) ==
    /\ pc[w] = "probe"
    /\ down' = now + Retry
    /\ pc' = [pc EXCEPT ![w] = "idle"]
    /\ UNCHANGED <<now, removed, seen>>

Worker(w) ==
    \/ Load(w)
    \/ StayPaused(w)
    \/ ObserveExpired(w)
    \/ ClearIfUnchanged(w)
    \/ ClearUnconditional(w)
    \/ Probe(w)
    \/ Pause(w)

Next == Expire \/ \E w \in Workers : Worker(w)

Spec == Init /\ [][Next]_vars

SpecFair == Spec /\ \A w \in Workers : WF_vars(Worker(w))

-----------------------------------------------------------------------------
\* No clear ever removes a pause that has not expired: the value a clear
\* removed was at most the time of the clear (time never runs backwards,
\* so checking it against the current time is the same check).  A pause
\* the other worker published after this worker observed an older expiry
\* expires later than now and so is never removed by this worker's clear.
FreshPauseSurvives == removed <= now

\* A worker about to clear holds a value that had expired when it looked.
ClearOnlyExpired == \A w \in Workers : pc[w] = "expired" => 0 < seen[w] /\ seen[w] <= now

\* Liveness: a pause that has expired is eventually cleared or replaced
\* by a fresh one.
Expired == down > 0 /\ now >= down

ExpiredPauseResolved == Expired ~> ~Expired
=============================================================================
