--------------------- MODULE TraceRNPathProbeRetryLayout ---------------------
(***************************************************************************)
(* The trace check of RNPathProbeRetryLayout: TraceLog is the record       *)
(* sequence tests/tla_trace_path_probe_retry.cpp writes, one record a call *)
(* of rocketnet_link_mtu_retry_next (sdk/rocketnet_link_mtu.h), with the   *)
(* operation (schedule), its argument (the call's time after the last      *)
(* slot), the action (Schedule) and every model variable after it.  The    *)
(* rest is the template, TraceRNTraceExample.tla.                          *)
(***************************************************************************)
EXTENDS RNPathProbeRetryLayout, Sequences

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
VarsRec == [retry |-> retry, slot |-> slot, now |-> now, started |-> started]

\* the actions each operation of the code can be
OpActs == [schedule |-> {"Schedule"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Schedule" -> Schedule(x[1])

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

\* the row's reach predicate: some clean trace must reach the count's
\* saturation, where the gap is the cap and passed slots are jumped
AtCap == started /\ retry = Saturate

=============================================================================
