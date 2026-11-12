------------------------- MODULE TraceRNTraceExample -------------------------
(***************************************************************************)
(* The trace check of RNTraceExample, and the template for every step      *)
(* layer's Trace<Spec>.tla (README-TraceCheck.md).  TraceLog is the record *)
(* sequence tests/tla_trace_example.cpp writes: one record per step, with  *)
(* the operation the walk called (op), the model action the code's own     *)
(* result says it took (act), and every model variable after it (state).   *)
(* Record 1 is "init".                                                     *)
(*                                                                         *)
(* TraceStep allows only the recorded action, and only when the operation  *)
(* can be that action (OpActs): a step that did one thing but landed in a  *)
(* state another action allows is rejected.  A state that names fewer or   *)
(* other fields than VarsRec never matches, and a record that is not       *)
(* [op, act, state] fails the ASSUME: an under-logged harness fails red,   *)
(* never green.  When no step matches, TraceReject sets rejected, so       *)
(* TraceConsistent fails; the behaviour TLC prints ends at ti, the last    *)
(* record that matched, and record ti + 1 is the first code step that is   *)
(* not a model step.  At the last record the trace stutters (TraceDone).   *)
(* Nothing here quantifies over TraceLog in a state formula: that costs    *)
(* the whole log in every state.                                           *)
(***************************************************************************)
EXTENDS RNTraceExample, Sequences

CONSTANT TraceLog

\* checked once, when TLC starts
ASSUME /\ Len(TraceLog) >= 2
       /\ \A n \in DOMAIN TraceLog : DOMAIN TraceLog[n] = {"op", "act", "state"}

VARIABLES ti,        \* the index of the record the state matches
          rejected   \* the record after ti is not a model step

\* every model variable by the field name the harness writes
VarsRec == [count |-> count, refused |-> refused]

\* the actions each operation of the code can be
OpActs == [claim |-> {"Claim", "Refuse"}, drain |-> {"Drain"}]

\* the action a record names; a name this CASE lacks stops TLC with an error
Act(a) == CASE a = "Claim" -> Claim
            [] a = "Refuse" -> Refuse
            [] a = "Drain" -> Drain

TraceInit ==
    /\ Init
    /\ ti = 1
    /\ rejected = (TraceLog[1].op # "init" \/ TraceLog[1].act # "init" \/ VarsRec # TraceLog[1].state)

TraceStep ==
    /\ ~rejected
    /\ ti < Len(TraceLog)
    /\ LET r == TraceLog[ti + 1]
       IN /\ r.op \in DOMAIN OpActs
          /\ r.act \in OpActs[r.op]
          /\ Act(r.act)
          /\ VarsRec' = r.state
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

\* every record of the trace is the model step it names, from the one before
TraceConsistent == ~rejected

=============================================================================
