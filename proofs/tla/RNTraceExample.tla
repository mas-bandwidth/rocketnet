---------------------------- MODULE RNTraceExample ----------------------------
(***************************************************************************)
(* The example layer of the trace check (README-TraceCheck.md): a bounded  *)
(* claim counter, the code in tests/tla_trace_example.cpp.  A claim takes  *)
(* one of Limit places, or is refused and counted when none is left; a     *)
(* drain frees every place.  It is checked only through its trace:         *)
(* TraceRNTraceExample.tla, run by tools/tla_trace_check.py --layer        *)
(* example, whose plant (a claim granted at the limit) must fail           *)
(* TraceConsistent.                                                        *)
(***************************************************************************)
EXTENDS Naturals

CONSTANT Limit

ASSUME Limit \in Nat

VARIABLES count,     \* places taken
          refused    \* claims refused

vars == <<count, refused>>

Init == count = 0 /\ refused = 0

Claim == count < Limit /\ count' = count + 1 /\ UNCHANGED refused

Refuse == count >= Limit /\ refused' = refused + 1 /\ UNCHANGED count

Drain == count' = 0 /\ UNCHANGED refused

Next == Claim \/ Refuse \/ Drain

Spec == Init /\ [][Next]_vars

InBounds == count <= Limit

=============================================================================
