------------------------ MODULE TraceRNSendGatherCall ------------------------
(***************************************************************************)
(* The trace check of RNSendGatherCall, copied from the template           *)
(* TraceRNTraceExample.tla (README-TraceCheck.md).  TraceLog is the record *)
(* sequence tests/tla_trace_gather.cpp writes over                         *)
(* sdk/rocketnet_send_gather.cpp: one record per step, with the step (op), *)
(* its arguments and what it gave back (args), the model action (act),     *)
(* and every model variable after it (state).  A stage writes one slot of  *)
(* the bank; a gather is one call of rocketnet_send_gather, with the order *)
(* it wrote and the runs it returned.  Everything but EXTENDS, VarsRec,    *)
(* OpActs, Act and the reach predicates is the template's text.            *)
(***************************************************************************)
EXTENDS RNSendGatherCall, Sequences

CONSTANT TraceLog

\* the log as a definition of no arguments, which TLC evaluates once when
\* it starts; every formula below reads Trace, never TraceLog. Read through
\* the substituted constant, TLC rebuilds the whole log at each access, and a
\* 3200-record trace takes 68 s instead of 3 s (bench-nyc-6, 2026-11-12)
Trace == TraceLog

\* checked once, when TLC starts
ASSUME /\ Len(TraceLog) >= 2
       /\ \A n \in DOMAIN TraceLog : DOMAIN TraceLog[n] = {"op", "act", "args", "state"}

VARIABLES ti,        \* the index of the record the state matches
          rejected   \* the record after ti is not a model step

\* every model variable by the field name the harness writes
VarsRec == [dest |-> dest, size |-> size]

\* the actions each operation of the code can be
OpActs == [stage |-> {"Stage"}, gather |-> {"Gather"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Stage" -> Stage(x[1], x[2], x[3])
               [] a = "Gather" -> Gather(x[1], x[2])

TraceInit ==
    /\ Init
    /\ ti = 1
    /\ rejected = (Trace[1].op # "init" \/ Trace[1].act # "init" \/ Trace[1].args # <<>>
                   \/ VarsRec # Trace[1].state)

TraceStep ==
    /\ ~rejected
    /\ ti < Len(Trace)
    /\ LET r == Trace[ti + 1]
       IN /\ r.op \in DOMAIN OpActs
          /\ r.act \in OpActs[r.op]
          /\ Act(r.act, r.args)
          /\ Next
          /\ VarsRec' = r.state
    /\ ti' = ti + 1
    /\ UNCHANGED rejected

TraceReject ==
    /\ ~rejected
    /\ ti < Len(Trace)
    /\ ~ENABLED TraceStep
    /\ rejected' = TRUE
    /\ UNCHANGED <<vars, ti>>

TraceDone ==
    /\ rejected \/ ti = Len(Trace)
    /\ UNCHANGED <<vars, ti, rejected>>

TraceSpec == TraceInit /\ [][TraceStep \/ TraceReject \/ TraceDone]_<<vars, ti, rejected>>

\* every record of the trace is the model step it names, from the one before
TraceConsistent == ~rejected

\* the row's reach predicates: some clean trace stages more packets to one
\* destination than a run holds, so the segment cap cuts its runs, and
\* three to one destination that no run can hold together, so the byte cap
\* cuts them
SegCapEdge == \E d \in Dests : Cardinality({s \in Slots : dest[s] = d /\ size[s] > 0}) > MaxSeg

ByteCapEdge == \E d \in Dests : Cardinality({s \in Slots : dest[s] = d /\ size[s] * 3 > MaxBytes}) >= 3

=============================================================================
