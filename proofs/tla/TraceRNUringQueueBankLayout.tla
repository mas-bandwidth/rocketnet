--------------------- MODULE TraceRNUringQueueBankLayout ---------------------
(***************************************************************************)
(* The trace check of RNUringQueueBankLayout, copied from the template     *)
(* TraceRNTraceExample.tla (README-TraceCheck.md).  TraceLog is the record *)
(* sequence tests/tla_trace_bank.cpp writes over                           *)
(* sdk/rocketnet_uring_send_queue.cpp: one record per call, with the call  *)
(* (op), its arguments and what it gave back (args), the model action its  *)
(* own result says it took (act), and every model variable after it        *)
(* (state).  A start that returned a pointer is Start with that pointer    *)
(* and id, one that returned NULL is StartFull; a finish or abort that     *)
(* returned true is Finish, false FinishRefused; a turn is Flush with the  *)
(* queues it turned.  Everything but EXTENDS, VarsRec, OpActs, Act and the *)
(* reach predicate is the template's text.                                 *)
(***************************************************************************)
EXTENDS RNUringQueueBankLayout, Sequences

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
VarsRec ==
    [prod |-> prod, cnt |-> cnt, gid |-> gid, held |-> held, owner |-> owner,
     demand |-> demand, flushes |-> flushes, oob |-> oob, shared |-> shared,
     foreign |-> foreign, routeBad |-> routeBad]

\* the actions each operation of the code can be
OpActs == [start |-> {"Start", "StartFull"}, finish |-> {"Finish", "FinishRefused"},
           abort |-> {"Finish", "FinishRefused"}, turn |-> {"Flush"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Start" -> Start(x[1], x[2], x[3], x[4])
               [] a = "StartFull" -> StartFull(x[1], x[2], x[3])
               [] a = "Finish" -> Finish(x[1], x[2])
               [] a = "FinishRefused" -> FinishRefused(x[1], x[2])
               [] a = "Flush" -> Flush(x[1])

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

\* the row's reach predicate: some clean trace holds the last slot of a bank,
\* whose pointer is one stride before the next bank
LastSlot == \E t \in Threads : held[t] # None /\ held[t].slot = S - 1

=============================================================================
