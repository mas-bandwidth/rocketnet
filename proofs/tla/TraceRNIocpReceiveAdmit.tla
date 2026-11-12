---------------------- MODULE TraceRNIocpReceiveAdmit ----------------------
(***************************************************************************)
(* The trace check of RNIocpReceiveAdmit: a record of the code's state     *)
(* after each step, written by tests/tla_trace_iocp.cpp's seeded random    *)
(* walk over rocketnet_iocp_receive_admit (layer iocp-admit), is a         *)
(* behaviour of the spec.  TraceLog is that record sequence; each record   *)
(* carries every model variable and nothing else (a record that names      *)
(* fewer fields is rejected).  TraceInit matches the first record against  *)
(* Init; TraceStep takes one model step whose successor matches the next   *)
(* record.  When no model step matches, TraceReject sets rejected, so      *)
(* TraceConsistent fails and TLC prints the behaviour: its last ti is the  *)
(* last record that matched.  At the last record the trace stutters        *)
(* (TraceDone).  README-TraceCheck.md has the mechanism.                   *)
(***************************************************************************)
EXTENDS RNIocpReceiveAdmit, Sequences

CONSTANT TraceLog

VARIABLES ti,        \* the index of the record the state matches
          rejected   \* the record after ti is not a model step

\* every model variable by the field name the harness writes
VarsRec ==
    [bank |-> bank, arrived |-> arrived, refused |-> refused, full |-> full,
     counted |-> counted, delivered |-> delivered, evicted |-> evicted]

\* a record names exactly the model's variables, each equal to the state's
Matches(r) == DOMAIN r = DOMAIN VarsRec /\ \A f \in DOMAIN r : VarsRec[f] = r[f]

TraceInit ==
    /\ Init
    /\ ti = 1
    /\ rejected = ~Matches(TraceLog[1])

TraceStep ==
    /\ ~rejected
    /\ ti < Len(TraceLog)
    /\ Next
    /\ DOMAIN TraceLog[ti + 1] = DOMAIN VarsRec
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
