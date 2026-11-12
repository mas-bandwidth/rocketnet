------------------------ MODULE TraceRNUringQueueSend ------------------------
(***************************************************************************)
(* The trace check of RNUringQueueSend: a record of the code's state after *)
(* each transition, written by the unit harness's seeded random walk, is a *)
(* behaviour of the spec.  TraceLog is that record sequence; each record   *)
(* carries every model variable, the environment's choices (the kernel's   *)
(* completion results) included: TLC explores every branch, so a choice a *)
(* record leaves open is rejected on the branch it did not take.           *)
(* TraceInit matches the first record against Init; TraceStep takes one    *)
(* model step whose successor matches the next record.  When no model step *)
(* matches, TraceReject sets rejected, so TraceConsistent fails and TLC    *)
(* prints the behaviour: its last ti is the last record that matched, and  *)
(* record ti + 1 is the first that is not a model step.  At the last       *)
(* record the trace stutters                                               *)
(* (TraceDone).  README-TraceCheck.md has the mechanism; the next layer's  *)
(* trace module is this one with its own VarsRec.                          *)
(***************************************************************************)
EXTENDS RNUringQueueSend

CONSTANT TraceLog

VARIABLES ti,        \* the index of the record the state matches
          rejected   \* the record after ti is not a model step

\* every model variable by the field name the harness writes
VarsRec ==
    [producer |-> producer, ready |-> ready, worker |-> worker, free |-> free,
     num |-> num, st |-> st, dest |-> dest, pkt |-> pkt, runs |-> runs, cq |-> cq,
     pend |-> pend, status |-> status, nextId |-> nextId, eagains |-> eagains,
     demand |-> demand, flushes |-> flushes, kernelWrite |-> kernelWrite]

Matches(r) == \A f \in DOMAIN r : VarsRec[f] = r[f]

TraceInit ==
    /\ Init
    /\ Matches(TraceLog[1])
    /\ ti = 1
    /\ rejected = FALSE

TraceStep ==
    /\ ~rejected
    /\ ti < Len(TraceLog)
    /\ Next
    /\ \A f \in DOMAIN TraceLog[ti + 1] : VarsRec'[f] = TraceLog[ti + 1][f]
    /\ ti' = ti + 1
    /\ UNCHANGED rejected

TraceReject ==
    /\ ~rejected
    /\ ti < Len(TraceLog)
    /\ ~ENABLED TraceStep
    /\ rejected' = TRUE
    /\ UNCHANGED <<vars, ti>>

TraceDone ==
    /\ rejected \/ ti = Len(TraceLog)
    /\ UNCHANGED <<vars, ti, rejected>>

TraceSpec == TraceInit /\ [][TraceStep \/ TraceReject \/ TraceDone]_<<vars, ti, rejected>>

\* every record of the trace is a model step from the one before
TraceConsistent == ~rejected

=============================================================================
