---------------------------- MODULE RNTraceExample ----------------------------
(***************************************************************************)
(* The example layer of the trace check (README-TraceCheck.md): a bounded  *)
(* claim counter, the code in tests/tla_trace_example.cpp.  A claim of k   *)
(* places takes them when k are free, or is refused and counted; a drain   *)
(* frees every place and returns how many it freed.  Claim and Refuse      *)
(* take an argument and Drain the value it returns, so the template shows  *)
(* that a record's args bind both what a call takes and what it gives      *)
(* back: a drain that returns the wrong count is rejected.  It is checked  *)
(* only through its trace: TraceRNTraceExample.tla, run by                 *)
(* tools/tla_trace_check.py --layer example, whose plant (a claim granted  *)
(* one place past the limit) must fail TraceConsistent at that record.     *)
(* Claim and Refuse take k only from 1..MaxClaim and Drain n only from     *)
(* 0..Limit, by Next's binders: the checker ties a record's arguments to   *)
(* them, since Claim(0) at count 0 makes Drain(0)'s change and Refuse(3)   *)
(* at count = Limit makes Refuse(1)'s.                                     *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS Limit,     \* places
          MaxClaim   \* the most places one claim asks for

ASSUME Limit \in Nat /\ MaxClaim \in Nat \ {0}

VARIABLES count,     \* places taken
          refused    \* claims refused

vars == <<count, refused>>

Init == count = 0 /\ refused = 0

Claim(k) == count + k <= Limit /\ count' = count + k /\ UNCHANGED refused

Refuse(k) == count + k > Limit /\ refused' = refused + 1 /\ UNCHANGED count

Drain(n) == n = count /\ count' = 0 /\ UNCHANGED refused

Next == \/ \E k \in 1..MaxClaim : Claim(k)
        \/ \E k \in 1..MaxClaim : Refuse(k)
        \/ \E n \in 0..Limit : Drain(n)

Spec == Init /\ [][Next]_vars

InBounds == count <= Limit

=============================================================================
