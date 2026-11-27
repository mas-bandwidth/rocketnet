----------------------- MODULE RNPathProbeRetryLayout -----------------------
(***************************************************************************)
(* The re-try schedule of a ruled-out path probe size, with the code's     *)
(* arithmetic: rocketnet_link_mtu_retry_next and                           *)
(* rocketnet_link_mtu_retry_gap_seconds in sdk/rocketnet_link_mtu.h, which *)
(* the client calls at each rule-out (rocketnet_client_probe_wait).  The   *)
(* walk itself, and when a re-try happens at all, is RNPathProbe.tla's      *)
(* (ClientRuleOut, ClientRetry); this module is the times.                 *)
(*                                                                         *)
(* The design (sdk/rocketnet_link_mtu.h): times are measured from the      *)
(* size's first rule-out, at 0 here.  The first Flat re-tries are First   *)
(* apart, then each gap is twice the one before, never above Cap; in the  *)
(* code First = 10 s, Flat = 3 and Cap = 300 s, so 10, 20, 30, then 50,   *)
(* 90, 170, 330, then 300 s apart.  DesignOffset(j) is the j-th re-try's  *)
(* time.  A re-try ruled out again waits for the first time of the        *)
(* schedule after it ended; the slots that passed while it ran are not    *)
(* taken.                                                                  *)
(*                                                                         *)
(* The code's state is the count of re-tries scheduled, retry, which       *)
(* saturates at Saturate (the first re-try whose gap is Cap), and the time *)
(* of the last one, slot.  A call at time now (the rule-out, slot = 0 and *)
(* retry = 0, or the end of a re-try that was ruled out again, at or after *)
(* its slot) advances both until slot is after now: each turn counts one *)
(* more re-try (to Saturate) and adds the gap the code's loop computes     *)
(* for that count; at Saturate, a run of passed slots of Cap each is      *)
(* skipped in one jump.  Schedule(d) is one call, d after the last slot.  *)
(*                                                                         *)
(* The rules: the next re-try is exactly the design's first time after    *)
(* now (NextIsTheDesigns), and the count is the design's index of it,      *)
(* capped at Saturate (CountSaturates).  Reversed witnesses (Broken):      *)
(*   "nocap"      the gap is not capped: the code's loop doubles past Cap  *)
(*                (NextIsTheDesigns);                                      *)
(*   "early"      the gap doubles one re-try early (NextIsTheDesigns);     *)
(*   "wrap"       the count does not saturate but is a counter of          *)
(*                CounterMod values that wraps, so the gaps start over     *)
(*                from First (NextIsTheDesigns, CountSaturates);           *)
(*   "noskip"     a call adds one gap whatever the time, so a re-try that *)
(*                ran past a slot is due again at once (NextIsTheDesigns). *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS
    First,          \* the first gaps (ROCKETNET_LINK_MTU_RETRY_FIRST_SECONDS)
    Flat,           \* how many re-tries have the first gap (ROCKETNET_LINK_MTU_RETRY_FLAT_COUNT)
    Cap,            \* the largest gap (ROCKETNET_LINK_MTU_RETRY_MAX_SECONDS)
    Saturate,       \* the count stops here (ROCKETNET_LINK_MTU_RETRY_SATURATE)
    CounterMod,     \* "wrap" only: the values of a counter that wraps
    MaxRun,         \* the longest a re-try runs past its slot before a call
    Horizon,        \* exploration bound on slot
    Broken          \* "none", or the reversed witness

ASSUME /\ First \in Nat \ {0} /\ Flat \in Nat /\ Cap \in Nat /\ Cap >= First
       /\ Saturate \in Nat \ {0} /\ CounterMod \in Nat \ {0}
       /\ MaxRun \in Nat /\ Horizon \in Nat
       /\ Broken \in {"none", "nocap", "early", "wrap", "noskip"}

Min2(a, b) == IF a < b THEN a ELSE b

\* the design: the gap before re-try j (1 for the first), and the time of
\* re-try j from the rule-out; from CapIndex, the first re-try whose gap is
\* Cap, every gap is Cap, so the time is a sum in closed form there (the
\* doubling is searched over a few dozen re-tries, in order, and stops at
\* the first that reaches Cap)
DesignGap(j) == IF j <= Flat THEN First ELSE Min2(First * 2^(j - Flat), Cap)
CapIndex == CHOOSE k \in (Flat + 1)..(Flat + 64) : First * 2^(k - Flat) >= Cap
RECURSIVE DesignOffset(_)
DesignOffset(j) ==
    IF j = 0 THEN 0
    ELSE IF j > CapIndex THEN DesignOffset(CapIndex) + (j - CapIndex) * Cap
    ELSE DesignOffset(j - 1) + DesignGap(j)

\* the design's re-tries within the bound, and the index of a time on it
Bound == CapIndex + (Horizon + MaxRun + Cap) \div Cap + 1
DesignTimes == {DesignOffset(j) : j \in 1..Bound}
IndexOf(t) == CHOOSE j \in 1..Bound : DesignOffset(j) = t

\* the code's gap, rocketnet_link_mtu_retry_gap_seconds: gap = First, then
\* for i from Flat while i < retry and gap < Cap, gap doubles; capped at Cap.
\* "early" starts the doubling at Flat - 1; "nocap" neither stops at Cap nor
\* caps the result
RECURSIVE Loop(_, _, _)
Loop(i, r, g) == IF i < r /\ (g < Cap \/ Broken = "nocap") THEN Loop(i + 1, r, 2 * g) ELSE g
CodeGap(r) == LET g == Loop(IF Broken = "early" THEN Flat - 1 ELSE Flat, r, First)
              IN IF g < Cap \/ Broken = "nocap" THEN g ELSE Cap

\* one turn of the count: to Saturate and no further; "wrap" counts on in a
\* counter of CounterMod values
Saturated(r) == r >= Saturate /\ Broken # "wrap"
Count(r) == IF Broken = "wrap" THEN (r + 1) % CounterMod
            ELSE IF r < Saturate THEN r + 1 ELSE r

\* rocketnet_link_mtu_retry_next at time n from <<retry, slot>>: the do-while
\* loop, with the jump over passed slots at Saturate; "noskip" takes one turn
RECURSIVE Advance(_, _, _)
Advance(r, t, n) ==
    LET jump == Saturated(r) /\ t + Cap <= n /\ Broken # "noskip"
        t0 == IF jump THEN t + ((n - t) \div Cap) * Cap ELSE t
        r1 == IF jump THEN r ELSE Count(r)
        t1 == t0 + CodeGap(r1)
    IN IF t1 <= n /\ Broken # "noskip" THEN Advance(r1, t1, n) ELSE <<r1, t1>>

VARIABLES
    retry,      \* the code's count of re-tries scheduled (probe_retries)
    slot,       \* the time of the last scheduled re-try, from the rule-out (probe_retry_time)
    now,        \* the time of the last call, from the rule-out
    started     \* the rule-out's own call has been made

vars == <<retry, slot, now, started>>

\* the size's first rule-out: the count at zero, the time at the rule-out
Init ==
    /\ retry = 0
    /\ slot = 0
    /\ now = 0
    /\ started = FALSE

\* one call: at the rule-out itself (the first, at 0), or at the end of a
\* re-try ruled out again, d after its slot
\* (now' first: where the step is already fixed, as in the trace check,
\* only the d that matches it computes Advance)
Schedule(d) ==
    /\ slot <= Horizon
    /\ d \in 0..MaxRun
    /\ (~started => d = 0)
    /\ now' = slot + d
    /\ LET next == Advance(retry, slot, slot + d)
       IN /\ retry' = next[1]
          /\ slot' = next[2]
    /\ started' = TRUE

\* past the bound, nothing more
Done ==
    /\ slot > Horizon
    /\ UNCHANGED vars

Next == (\E d \in 0..MaxRun : Schedule(d)) \/ Done

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ retry \in Nat
    /\ slot \in Nat
    /\ now \in Nat
    /\ started \in BOOLEAN

\* the next re-try is the design's first time after now
NextIsTheDesigns ==
    started => /\ slot > now
               /\ slot \in DesignTimes
               /\ \A t \in DesignTimes : t > now => t >= slot

\* the count is the design's index of the next re-try, to Saturate
CountSaturates ==
    started /\ slot \in DesignTimes => retry = Min2(IndexOf(slot), Saturate)

\* the arithmetic itself: the code's gap is the design's, and at Saturate it
\* is Cap (a check of the constants, with the state)
GapIsTheDesigns == \A j \in 1..Saturate + 2 : CodeGap(Min2(j, Saturate)) = DesignGap(j)

=============================================================================
