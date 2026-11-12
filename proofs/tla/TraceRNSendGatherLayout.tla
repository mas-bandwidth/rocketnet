---------------------- MODULE TraceRNSendGatherLayout ----------------------
(***************************************************************************)
(* The trace check of RNSendGatherLayout.  The gather is one pass of a     *)
(* single worker over a bank, deterministic in its input, so its trace is  *)
(* a case per call: the bank the code was handed (each slot's destination *)
(* and size) and what the code returned (order and runs, runs numbered     *)
(* from 1 as the model numbers them).  tests/tla_trace_gather.cpp writes   *)
(* TraceLog from sdk/rocketnet_send_gather.cpp on seeded random banks.     *)
(* TLC starts the model once per case from that case's bank and runs it   *)
(* to the end; TraceConsistent says the model's order and runs there are   *)
(* the code's.  Every field of a case is required: a case that names      *)
(* fewer is rejected (README-TraceCheck.md: an under-logged harness fails  *)
(* red, never green).                                                      *)
(***************************************************************************)
EXTENDS RNSendGatherLayout

CONSTANT TraceLog

\* a case the model cannot start from is an error, never a case skipped
ASSUME \A n \in DOMAIN TraceLog :
    /\ DOMAIN TraceLog[n].dest = 0..(S - 1) /\ DOMAIN TraceLog[n].size = 0..(S - 1)
    /\ \A s \in 0..(S - 1) : TraceLog[n].dest[s] \in Dests /\ TraceLog[n].size[s] \in Sizes

VARIABLE ci          \* the case this behaviour runs

CaseFields == {"dest", "size", "order", "runs"}

\* the case's bank first, so TLC takes it rather than enumerating every bank
TraceInit ==
    /\ ci \in DOMAIN TraceLog
    /\ DOMAIN TraceLog[ci] = CaseFields
    /\ dest = TraceLog[ci].dest
    /\ size = TraceLog[ci].size
    /\ Init

TraceSpec == TraceInit /\ [][Next /\ UNCHANGED ci]_<<vars, ci>> /\ WF_<<vars, ci>>(Next /\ UNCHANGED ci)

\* every case names every field, and the model ends where the code did
TraceConsistent ==
    /\ \A n \in DOMAIN TraceLog : DOMAIN TraceLog[n] = CaseFields
    /\ (pc = "done" =>
           /\ ~oob
           /\ order = TraceLog[ci].order
           /\ runs = TraceLog[ci].runs)

\* every case reaches the end, so TraceConsistent is checked on it
TraceEnds == <>(pc = "done")

=============================================================================
