---------------------------- MODULE RNUpdateClock ----------------------------
(***************************************************************************)
(* The update clock: the caller's time, clamped once per update, in the   *)
(* client transport, the server transport and the message object          *)
(* (sdk/rocketnet_client_transport.cpp rocketnet_advance_update_time,     *)
(* beside rocketnet_clamp_update_time).                                    *)
(*                                                                         *)
(* One owner.  raw is the caller's time at the last update, before the    *)
(* clamp; good is the clamped time every deadline reads.  Time is an      *)
(* integer here; the code's doubles take the same steps.                   *)
(*                                                                         *)
(* ForwardStep: a finite time at or above raw.  The rule advances good by *)
(* the raw step, so after a backwards step the owner resumes at once.     *)
(* While no backwards step has happened raw and good are equal, and the   *)
(* code returns the caller's time unchanged (SeedIdentity).               *)
(*                                                                         *)
(* BackwardStep: a finite time below raw.  good holds, raw records the    *)
(* new time, one misuse is counted (API misuse, sdk/rocketnet.h).          *)
(*                                                                         *)
(* NonFiniteSample: NaN or an infinity.  Neither value changes; one       *)
(* misuse is counted.                                                      *)
(*                                                                         *)
(* BrokenStall selects the rule before the update clock: good becomes the *)
(* caller's time only when that time is at or above good, so after a      *)
(* backwards step of D the owner stands still for D, and keep-alives,     *)
(* resends and the dead-connection timeout with it.  It must violate      *)
(* ResumesExactly.                                                         *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    Start,          \* the create time, the seed of both raw and good
    MaxTime,        \* the instance's bound on the caller's time
    MaxStep,        \* the largest step, forwards or backwards, per update
    MaxUpdates,     \* the instance's bound on the number of updates
    BrokenStall     \* TRUE: the rule before the update clock

ASSUME Start \in 0..MaxTime
ASSUME MaxStep >= 1 /\ MaxUpdates >= 1
ASSUME BrokenStall \in BOOLEAN

Kinds == { "seed", "forward", "backward", "nonfinite" }

VARIABLES
    raw,            \* the caller's time at the last update
    good,           \* the clamped time
    stepped,        \* ghost: a backwards step has happened
    kind,           \* ghost: the last update's kind
    rawDelta,       \* ghost: the last update's raw step (forward only)
    goodDelta,      \* ghost: the last update's advance of good
    previousGood,   \* ghost: good before the last update
    misuses,        \* the misuse count
    updates         \* the number of updates so far

vars == << raw, good, stepped, kind, rawDelta, goodDelta, previousGood, misuses, updates >>

TypeOK ==
    /\ raw \in 0..MaxTime
    /\ good \in Nat
    /\ stepped \in BOOLEAN
    /\ kind \in Kinds
    /\ rawDelta \in Nat
    /\ goodDelta \in Nat
    /\ previousGood \in Nat
    /\ misuses \in Nat
    /\ updates \in 0..MaxUpdates

Init ==
    /\ raw = Start
    /\ good = Start
    /\ stepped = FALSE
    /\ kind = "seed"
    /\ rawDelta = 0
    /\ goodDelta = 0
    /\ previousGood = Start
    /\ misuses = 0
    /\ updates = 0

\* the clamped time a forward caller time t gives
Advance(t) ==
    IF BrokenStall
    THEN IF t >= good THEN t ELSE good
    ELSE good + ( t - raw )

\* under BrokenStall a forward raw step still below good is a misuse
ForwardMisuse(t) == IF BrokenStall /\ t < good THEN 1 ELSE 0

ForwardStep ==
    /\ updates < MaxUpdates
    /\ \E d \in 0..MaxStep :
          /\ raw + d <= MaxTime
          /\ raw' = raw + d
          /\ good' = Advance(raw + d)
          /\ rawDelta' = d
          /\ goodDelta' = Advance(raw + d) - good
          /\ misuses' = misuses + ForwardMisuse(raw + d)
    /\ previousGood' = good
    /\ kind' = "forward"
    /\ updates' = updates + 1
    /\ UNCHANGED stepped

BackwardStep ==
    /\ updates < MaxUpdates
    /\ \E d \in 1..MaxStep :
          /\ raw >= d
          /\ raw' = raw - d
    /\ good' = good
    /\ stepped' = TRUE
    /\ rawDelta' = 0
    /\ goodDelta' = 0
    /\ previousGood' = good
    /\ misuses' = misuses + 1
    /\ kind' = "backward"
    /\ updates' = updates + 1

NonFiniteSample ==
    /\ updates < MaxUpdates
    /\ raw' = raw
    /\ good' = good
    /\ rawDelta' = 0
    /\ goodDelta' = 0
    /\ previousGood' = good
    /\ misuses' = misuses + 1
    /\ kind' = "nonfinite"
    /\ updates' = updates + 1
    /\ UNCHANGED stepped

Next == ForwardStep \/ BackwardStep \/ NonFiniteSample

Spec == Init /\ [][Next]_vars

\* the clamped time never decreases
NeverDecreases == good >= previousGood

\* after any backwards step, each forward raw step of d advances good by d
ResumesExactly == ( kind = "forward" /\ stepped ) => goodDelta = rawDelta

\* until the first backwards step the caller's time is returned unchanged
SeedIdentity == ~stepped => good = raw

=============================================================================
