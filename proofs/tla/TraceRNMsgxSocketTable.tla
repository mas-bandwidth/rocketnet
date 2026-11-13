------------------------ MODULE TraceRNMsgxSocketTable ------------------------
(***************************************************************************)
(* The trace check of RNMsgxSocketTable (README-TraceCheck.md is the       *)
(* mechanism, TraceRNTraceExample.tla the template).  TraceLog is the      *)
(* record sequence tests/tla_trace_msgx.cpp writes: a seeded random walk   *)
(* over the real sdk/rocketnet_msgx_socket_table.h with a fake kernel, a   *)
(* fake receive poller and fake fill threads and holders, one call or      *)
(* fake step per record, with every model variable after it.               *)
(*                                                                         *)
(* A call of main's is one record, and the walk sees the table between     *)
(* calls: so a forget is ForgetCall and a sweep is SweepCall, the call-    *)
(* level actions built from the same step operators as the fine-grained    *)
(* Forget, Retire, ForgetDereg, SweepBegin, RetireDead and Close, which    *)
(* the model keeps so holders interleave inside a call, and which no walk  *)
(* takes (the row's untraced list).  The index is logged as the entries in *)
(* use, the variable the model keeps.                                      *)
(***************************************************************************)
EXTENDS RNMsgxSocketTable, Sequences

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
    [word |-> word, deadRep |-> deadRep, state |-> state, fd |-> fd, gen |-> gen, addr |-> addr,
     snap |-> snap, pass |-> pass, deadReports |-> deadReports, freeSlots |-> freeSlots,
     numFree |-> numFree, retired |-> retired, numRetired |-> numRetired, numOpen |-> numOpen,
     index |-> index, coolAddr |-> coolAddr, coolUntil |-> coolUntil, counters |-> counters,
     update |-> update, opensLeft |-> opensLeft, mpc |-> mpc, ms |-> ms, mi |-> mi, mk |-> mk,
     kfd |-> kfd, kdead |-> kdead, poller |-> poller, pkts |-> pkts, cur |-> cur, rep |-> rep,
     rxEv |-> rxEv, rxFd |-> rxFd, crossed |-> crossed, onClosed |-> onClosed, attempts |-> attempts]

\* the actions each operation of the code can be
OpActs == [mark |-> {"Mark"}, forget |-> {"ForgetCall"}, update |-> {"BeginUpdate"},
           sweep |-> {"SweepCall"}, stamp |-> {"Stamp"}, holder_begin |-> {"HolderBegin"},
           holder_end |-> {"HolderEnd"}, report |-> {"ReportDead"}, resolve |-> {"TxResolve"},
           send |-> {"TxSend"}, poll |-> {"RxPoll"}, rx_resolve |-> {"RxResolve"},
           rx_read |-> {"RxRead"}, env_open |-> {"EnvOpen"}, env_close |-> {"EnvClose"},
           disconnect |-> {"Disconnect"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Mark" -> Mark(x[1])
               [] a = "ForgetCall" -> ForgetCall(x[1])
               [] a = "BeginUpdate" -> BeginUpdate
               [] a = "SweepCall" -> SweepCall
               [] a = "Stamp" -> Stamp(x[1], x[2])
               [] a = "HolderBegin" -> HolderBegin(x[1])
               [] a = "HolderEnd" -> HolderEnd(x[1])
               [] a = "ReportDead" -> ReportDead(x[1])
               [] a = "TxResolve" -> TxResolve(x[1], x[2])
               [] a = "TxSend" -> TxSend(x[1])
               [] a = "RxPoll" -> RxPoll(x[1])
               [] a = "RxResolve" -> RxResolve
               [] a = "RxRead" -> RxRead
               [] a = "EnvOpen" -> EnvOpen
               [] a = "EnvClose" -> EnvClose(x[1])
               [] a = "Disconnect" -> Disconnect(x[1])

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

\* the row's reach predicates: some clean trace must show the index probing
\* past an entry's home and wrapping past the last entry, a retired socket
\* held in quarantine by a holder's open pass, and a socket retired on a
\* holder's dead report
IndexProbed == \E e \in DOMAIN index : Home(addr[index[e]]) # e
IndexWrapped == \E e \in DOMAIN index : Home(addr[index[e]]) > e
Held == \E s \in Slots : state[s] = RETIRED /\ ~Quiescent(MainV, s)
DeadRetired == counters[C_RETIRED_DEAD] > 0

=============================================================================
