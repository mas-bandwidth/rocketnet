--------------------------- MODULE RNUpdateClock ---------------------------
(***************************************************************************)
(* The update-time clamp shared by the client transport, the server        *)
(* transport and the message object (rocketnet_clamp_update_time_resume in *)
(* sdk/rocketnet_client_transport.cpp).  One raw caller sample per update: *)
(*                                                                         *)
(*   * a forward sample before any backwards step returns the caller's     *)
(*     time unchanged, so the common path is bit-identical;                *)
(*   * a backwards sample keeps the clamped time, records the raw sample   *)
(*     and counts one misuse;                                              *)
(*   * every later forward sample of d advances the clamped time by d;     *)
(*   * a NaN or infinite sample changes neither time and counts a misuse.  *)
(*                                                                         *)
(* Defect = "stall" restores the rule this fix removes: the clamp stands   *)
(* still until the raw time passes the clamped time again.                 *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS Start,     \* the first raw sample, small
          Limit,     \* the largest raw sample explored
          GoodLimit, \* the largest clamped time explored
          MaxStep,   \* the largest raw step, forward or backward
          Defect     \* "none" or "stall"

ASSUME Start \in Nat /\ Limit \in Nat /\ GoodLimit \in Nat /\ MaxStep \in Nat
ASSUME Start <= Limit /\ Limit <= GoodLimit
ASSUME MaxStep > 0
ASSUME Defect \in {"none", "stall"}

VARIABLES raw,      \* the caller's raw sample, the last one accepted
          good,     \* the clamped time the protocol reads
          seen      \* a backwards sample has happened

vars == <<raw, good, seen>>

Init == raw = Start /\ good = Start /\ seen = FALSE

\* A forward raw sample. Before the first backwards step the clamped time
\* is the raw one and the returned time is exactly the caller's; after it
\* the clamped time advances by the raw step d. The stall defect keeps the
\* clamped time until the raw sample reaches it.
Forward(d) ==
    /\ d \in 1..MaxStep
    /\ raw + d <= Limit
    /\ raw' = raw + d
    /\ good' = IF raw' >= good
                  THEN raw'
                  ELSE IF Defect = "stall" THEN good ELSE good + d
    /\ good' <= GoodLimit
    /\ seen' = seen

\* A backwards raw sample: the clamped time stands, the raw sample is
\* recorded, and the misuse is counted (the counter is outside the model).
Backward(d) ==
    /\ d \in 1..MaxStep
    /\ d <= raw
    /\ raw' = raw - d
    /\ good' = good
    /\ seen' = TRUE

\* A NaN or infinite sample: neither time changes, and the misuse is
\* counted (the counter is outside the model).
NonFinite ==
    /\ UNCHANGED <<raw, good, seen>>

Next == \E d \in 1..MaxStep : Forward(d) \/ Backward(d)
        \/ NonFinite

Spec == Init /\ [][Next]_vars

TypeOK == raw \in 0..Limit /\ good \in 0..GoodLimit /\ seen \in BOOLEAN

\* The offset from the raw sample is exactly zero until the first backwards
\* sample, so the common path returns the caller's own bits.
OffsetZeroUntilStep == ~seen => good = raw

\* The clamped time never decreases.
Monotone == [][good' >= good]_vars

\* After any backwards sample, each later forward raw sample of d advances
\* the clamped time by exactly d, unless the sample reaches the clamped time
\* and returns the caller's own value (the offset is zero again).
ResumesExactly == [][(seen /\ raw' > raw) =>
                        good' - good = raw' - raw \/ (raw' >= good /\ good' = raw')]_vars
=============================================================================
