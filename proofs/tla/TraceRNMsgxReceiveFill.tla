----------------------- MODULE TraceRNMsgxReceiveFill -----------------------
(***************************************************************************)
(* The trace check of RNMsgxReceiveFill (layer msgxreceivefill), a copy of *)
(* TraceRNTraceExample.tla (README-TraceCheck.md).  TraceLog is the record *)
(* sequence tests/tla_trace_msgx_receive_fill.cpp writes over the real     *)
(* sdk/rocketnet_msgx_receive_fill.h with a fake kernel: one record per    *)
(* step, with the operation the walk called (op: "step", one call of       *)
(* rocketnet_msgx_receive_drain_step; "poll", "round" and "sleep", the     *)
(* worker loop around it; "call", one rocketnet_msgx_receive_call of main  *)
(* with its busy flag and capacity; "stop", main's stop; "exit", the       *)
(* worker's exit; "count", rocketnet_msgx_receive_stop_count; "arrive" and *)
(* "error", the kernel), its                                               *)
(* arguments (the socket, the error's kind, the count a read took), the    *)
(* action the code's result names (act) and every model variable after it *)
(* (state).  Record 1 is "init".                                           *)
(*                                                                         *)
(* A copy changes only EXTENDS, VarsRec, OpActs, Act and the row's reach   *)
(* predicates; tools/tla_trace_check.py compares the rest with the         *)
(* template.                                                               *)
(***************************************************************************)
EXTENDS RNMsgxReceiveFill, Sequences

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
VarsRec == [kq |-> kq, errs |-> errs, cnt |-> cnt, committed |-> committed, worker |-> worker,
            main |-> main, tail |-> tail, free |-> free, mainCursor |-> mainCursor,
            mainLimit |-> mainLimit, tailCursor |-> tailCursor, tailEnd |-> tailEnd,
            pc |-> pc, cur |-> cur, reads |-> reads, roundRead |-> roundRead,
            arrived |-> arrived, kdrops |-> kdrops, consumed |-> consumed,
            fullDrops |-> fullDrops, errors |-> errors, stopping |-> stopping,
            exited |-> exited, counted |-> counted, stopped |-> stopped,
            committedTop |-> committedTop, late |-> late]

\* the actions each operation of the code can be
OpActs == [arrive |-> {"Arrive", "KernelDrop"},
           error |-> {"Error"},
           poll |-> {"Poll"},
           step |-> {"MoveFull", "HoldFull", "BudgetSpent", "Read", "ReadEmpty", "ReadWeather", "ReadFailed"},
           round |-> {"RoundCommit", "RoundKeep"},
           sleep |-> {"Sleep"},
           call |-> {"Call"},
           stop |-> {"MainStop"},
           exit |-> {"Exit"},
           count |-> {"StopCount"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Arrive" -> Arrive(x[1])
               [] a = "KernelDrop" -> KernelDrop(x[1])
               [] a = "Error" -> Error(x[1], x[2])
               [] a = "Poll" -> Poll
               [] a = "MoveFull" -> MoveFull
               [] a = "HoldFull" -> HoldFull
               [] a = "BudgetSpent" -> BudgetSpent
               [] a = "Read" -> Read(x[1])
               [] a = "ReadEmpty" -> ReadEmpty
               [] a = "ReadWeather" -> ReadWeather
               [] a = "ReadFailed" -> ReadFailed
               [] a = "RoundCommit" -> RoundCommit
               [] a = "RoundKeep" -> RoundKeep
               [] a = "Sleep" -> Sleep
               [] a = "Call" -> Call(x[1], x[2])
               [] a = "MainStop" -> MainStop
               [] a = "Exit" -> Exit
               [] a = "StopCount" -> StopCount

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

\* the row's reach predicates: some clean trace meets a room smaller than a
\* batch with more queued than the room (the edge where the plant's read
\* differs), and some clean trace holds a full bank
ShortRoom == /\ pc = "read"
             /\ cur \in SocketIds
             /\ 0 < Capacity - cnt[worker]
             /\ Capacity - cnt[worker] < Batch
             /\ kq[cur] > Capacity - cnt[worker]
Held == pc = "held"

\* and some clean trace meets a budget that leaves less than both the batch
\* and the room with more queued (where the read is capped by the budget)
BudgetCaps == /\ pc = "read"
              /\ cur \in SocketIds
              /\ reads < Budget
              /\ Budget - reads < Batch
              /\ Budget - reads < Capacity - cnt[worker]
              /\ kq[cur] > Budget - reads

\* and some clean trace has main read the bank the worker is still filling
\* (the handoff's point: committed datagrams returned before the worker
\* leaves the bank), and some call is cut by its capacity
ReadsShared == main = worker /\ mainCursor > 0
CallCut == tailCursor < tailEnd \/ mainCursor < mainLimit

=============================================================================
