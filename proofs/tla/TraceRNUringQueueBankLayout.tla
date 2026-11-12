--------------------- MODULE TraceRNUringQueueBankLayout ---------------------
(***************************************************************************)
(* The trace check of RNUringQueueBankLayout: a record of the code's state *)
(* after each transition, written by tests/tla_trace.cpp's seeded random   *)
(* walk over sdk/rocketnet_uring_send_queue.cpp, is a behaviour of the     *)
(* spec.  TraceLog is that record sequence; each record carries every      *)
(* model variable and nothing else (a record that names fewer fields is    *)
(* rejected: an under-logged harness fails red, never green).  TraceInit   *)
(* matches the first record against Init; TraceStep takes one model step   *)
(* whose successor matches the next record.  When no model step matches,   *)
(* TraceReject sets rejected, so TraceConsistent fails and TLC prints the  *)
(* behaviour: its last ti is the last record that matched, and record      *)
(* ti + 1 is the first that is not a model step.  At the last record the  *)
(* trace stutters (TraceDone).  README-TraceCheck.md has the mechanism.    *)
(***************************************************************************)
EXTENDS RNUringQueueBankLayout, Sequences

CONSTANT TraceLog

VARIABLES ti,        \* the index of the record the state matches
          rejected   \* the record after ti is not a model step

\* every model variable by the field name the harness writes
VarsRec ==
    [prod |-> prod, cnt |-> cnt, gid |-> gid, held |-> held, owner |-> owner,
     demand |-> demand, flushes |-> flushes, oob |-> oob, shared |-> shared,
     foreign |-> foreign, routeBad |-> routeBad]

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

\* the trace module's own steps (it extends the spec, so Next is the spec's)
TraceNext == TraceStep \/ TraceReject \/ TraceDone

TraceSpec == TraceInit /\ [][TraceNext]_<<vars, ti, rejected>>

\* every record of the trace is a model step from the one before
TraceConsistent == ~rejected

=============================================================================
