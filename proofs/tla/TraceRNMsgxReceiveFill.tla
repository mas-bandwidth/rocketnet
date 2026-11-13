----------------------- MODULE TraceRNMsgxReceiveFill -----------------------
(***************************************************************************)
(* The trace check of RNMsgxReceiveFill (layer msgxreceivefill), a copy of *)
(* TraceRNTraceExample.tla (README-TraceCheck.md).  TraceLog is the record *)
(* sequence tests/tla_trace_msgx_receive_fill.cpp writes over the real     *)
(* sdk/rocketnet_msgx_receive_fill.h with a fake kernel: one record per    *)
(* step, with the operation the walk called (op: "step", one call of       *)
(* rocketnet_msgx_receive_drain_step; "poll", "round" and "sleep", the     *)
(* worker loop around it; "consume" and "claim", main, the claim through   *)
(* rocketnet_msgx_receive_claim; "stop", main's stop through             *)
(* rocketnet_msgx_receive_stop; "exit", the worker's exit; "arrive" and    *)
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
VarsRec == [kq |-> kq, errs |-> errs, cnt |-> cnt, worker |-> worker, ready |-> ready,
            main |-> main, free |-> free, pc |-> pc, cur |-> cur, reads |-> reads,
            arrived |-> arrived, kdrops |-> kdrops, consumed |-> consumed,
            fullDrops |-> fullDrops, errors |-> errors, stopping |-> stopping,
            exited |-> exited, stopped |-> stopped]

\* the actions each operation of the code can be
OpActs == [arrive |-> {"Arrive", "KernelDrop"},
           error |-> {"Error"},
           poll |-> {"Poll"},
           step |-> {"PublishFull", "HoldFull", "BudgetSpent", "Read", "ReadEmpty", "ReadWeather", "ReadFailed"},
           round |-> {"RoundPublish", "RoundKeep"},
           sleep |-> {"Sleep"},
           consume |-> {"Consume"},
           claim |-> {"Claim"},
           stop |-> {"MainStop"},
           exit |-> {"Exit"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Arrive" -> Arrive(x[1])
               [] a = "KernelDrop" -> KernelDrop(x[1])
               [] a = "Error" -> Error(x[1], x[2])
               [] a = "Poll" -> Poll
               [] a = "PublishFull" -> PublishFull
               [] a = "HoldFull" -> HoldFull
               [] a = "BudgetSpent" -> BudgetSpent
               [] a = "Read" -> Read(x[1])
               [] a = "ReadEmpty" -> ReadEmpty
               [] a = "ReadWeather" -> ReadWeather
               [] a = "ReadFailed" -> ReadFailed
               [] a = "RoundPublish" -> RoundPublish
               [] a = "RoundKeep" -> RoundKeep
               [] a = "Sleep" -> Sleep
               [] a = "Consume" -> Consume
               [] a = "Claim" -> Claim
               [] a = "MainStop" -> MainStop
               [] a = "Exit" -> Exit

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

=============================================================================
