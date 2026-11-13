----------------------- MODULE TraceRNIocpSendLanes -----------------------
(***************************************************************************)
(* The trace check of RNIocpSendLanes (layer iocp-lanes), a copy of        *)
(* TraceRNTraceExample.tla (README-TraceCheck.md).  TraceLog is the record *)
(* sequence tests/tla_trace_iocp_lanes.cpp writes over the code's lane     *)
(* free queues, destination hash and carve: one record per step, with the  *)
(* operation the walk called (op: "acquire", start_packet's pop from the   *)
(* destination's lane; "post", a lane's bank posted whole; "release", a    *)
(* completion's release), its arguments (the destination, the lane the     *)
(* hash returned and the operation the pop returned; the send thread and   *)
(* the lane it posted; the operation released), the action the call's result names (act: Start or *)
(* StartRefused by the pop's result; Post; Complete) and every model       *)
(* variable after it (state).  The cfg's DestLaneAt is the code's hash of  *)
(* each destination.  Record 1 is "init".                                  *)
(*                                                                         *)
(* A copy changes only EXTENDS, VarsRec, OpActs, Act and the row's reach   *)
(* predicates; tools/tla_trace_check.py compares the rest with the         *)
(* template.                                                               *)
(***************************************************************************)
EXTENDS RNIocpSendLanes, Sequences

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
VarsRec == [free |-> free, bank |-> bank, inflight |-> inflight, started |-> started,
            posted |-> posted, postedBy |-> postedBy]

\* the actions each operation of the code can be
OpActs == [acquire |-> {"Start", "StartRefused"}, post |-> {"Post"}, release |-> {"Complete"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Start" -> Start(x[1], x[2], x[3])
               [] a = "StartRefused" -> StartRefused(x[1], x[2])
               [] a = "Post" -> Post(x[1], x[2])
               [] a = "Complete" -> Complete(x[1])

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

\* the row's reach predicate: some clean trace empties a lane's free queue,
\* the edge where Start turns to StartRefused
LaneEmpty == \E l \in LaneIds : free[l] = << >>

\* the row's reach predicate: some clean trace has every send thread post a
\* lane, so the thread-to-lane arithmetic is walked by every thread
EveryThreadPosts == \A t \in ThreadIds : \E l \in LaneIds : t \in postedBy[l]

=============================================================================
