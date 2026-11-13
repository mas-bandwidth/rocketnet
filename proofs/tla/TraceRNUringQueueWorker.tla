----------------------- MODULE TraceRNUringQueueWorker -----------------------
(***************************************************************************)
(* The trace check of RNUringQueueWorker: a record of the code's state     *)
(* after each transition, written by tests/tla_trace_queue_worker.cpp's    *)
(* seeded random walk, is a behaviour of the spec.  TraceLog is that       *)
(* record sequence; each record carries every model variable, the          *)
(* kernel's answers included: TLC explores every branch, so a choice a     *)
(* record leaves open is rejected on the branch it did not take.           *)
(* TraceInit matches the first record against Init; TraceStep takes one    *)
(* model step whose successor matches the next record.  When no model step *)
(* matches, TraceReject sets rejected, so TraceConsistent fails and TLC    *)
(* prints the behaviour: its last ti is the last record that matched, and  *)
(* record ti + 1 is the first that is not a model step.  At the last       *)
(* record the trace stutters (TraceDone).  README-TraceCheck.md has the    *)
(* mechanism; this is TraceRNUringQueueSend with this model's VarsRec.     *)
(***************************************************************************)
(***************************************************************************)
EXTENDS RNUringQueueWorker

CONSTANT TraceLog

VARIABLES ti,        \* the index of the record the state matches
          rejected   \* the record after ti is not a model step

\* every model variable by the field name the harness writes
VarsRec ==
    [quit |-> quit, seen |-> seen, st |-> st, handed |-> handed, ready |-> ready,
     held |-> held, u |-> u, k |-> k, cOk |-> cOk, cAgain |-> cAgain, cRef |-> cRef,
     blocked |-> blocked, poll |-> poll, recv |-> recv, wakeRead |-> wakeRead,
     wakeCount |-> wakeCount, agains |-> agains, againsBefore |-> againsBefore,
     refusals |-> refusals]

\* a record is the model's variables exactly: record equality is equal fields
\* and equal values, so a record with a field more or less never matches
TraceInit ==
    /\ Init
    /\ ti = 1
    /\ rejected = (VarsRec # TraceLog[1])

TraceStep ==
    /\ ~rejected
    /\ ti < Len(TraceLog)
    /\ Next
    /\ VarsRec' = TraceLog[ti + 1]
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
