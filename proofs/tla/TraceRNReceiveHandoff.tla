----------------------- MODULE TraceRNReceiveHandoff -----------------------
(***************************************************************************)
(* The trace check of RNReceiveHandoff, copied from the template           *)
(* TraceRNTraceExample.tla (README-TraceCheck.md).  TraceLog is the record *)
(* sequence tests/tla_trace_receive_handoff.cpp writes over the gso        *)
(* server's receive handoff (sdk/rocketnet_server_gso_linux.cpp over      *)
(* sdk/rocketnet_receive_handoff.h), or tests/tla_trace_receive_handoff_  *)
(* xdp.cpp over the xdp server's (sdk/rocketnet_server_socket_linux.cpp), *)
(* on a one-queue test fixture: one                                       *)
(* record per step, with the step (op), its arguments and what it gave     *)
(* back (args), the model action (act), and every model variable after it  *)
(* (state).  A bank is a worker pass staging n datagrams, an end_pass the  *)
(* worker's commit at the pass's end, a call one receive call with its     *)
(* budget, whether the mutex was held against it, and the run it returned. *)
(* Everything but EXTENDS, VarsRec, OpActs, Act and the reach predicates   *)
(* is the template's text.                                                 *)
(***************************************************************************)
EXTENDS RNReceiveHandoff, Sequences

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
VarsRec == [worker |-> worker, main |-> main, tail |-> tail, free |-> free, first |-> first, count |-> count,
           committed |-> committed, mainCursor |-> mainCursor, mainLimit |-> mainLimit, tailCursor |-> tailCursor,
           tailEnd |-> tailEnd, arrived |-> arrived, delivered |-> delivered, committedTop |-> committedTop]

\* the actions each operation of the code can be
OpActs == [bank |-> {"Bank"}, end_pass |-> {"Commit"}, call |-> {"Call"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Bank" -> Bank(x[1])
               [] a = "Commit" -> Commit
               [] a = "Call" -> Call(x[1], x[2], x[3], x[4])

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

\* the row's reach predicates: some clean trace has main reading the
\* buffer the worker fills (the rule's point), the worker banking past a
\* buffer it left to main, a cut call leaving the tail unread, and the
\* worker's buffer full
MainOnWorkerBuffer == main = worker /\ mainLimit > 0

WorkerAhead == main # worker /\ count[worker] > 0

TailPending == tailCursor < tailEnd

WorkerFull == count[worker] = Cap

=============================================================================
