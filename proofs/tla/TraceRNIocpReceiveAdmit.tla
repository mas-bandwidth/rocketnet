---------------------- MODULE TraceRNIocpReceiveAdmit ----------------------
(***************************************************************************)
(* The trace check of RNIocpReceiveAdmit (layer iocp-admit), a copy of     *)
(* TraceRNTraceExample.tla (README-TraceCheck.md).  TraceLog is the record *)
(* sequence tests/tla_trace_iocp.cpp writes over                           *)
(* rocketnet_iocp_receive_admit: one record per step, with the operation   *)
(* the walk called (op: "arrive", one completed datagram, or "drain", main *)
(* taking the bank), its arguments (the datagram's kind), the action the   *)
(* code's verdict names (act: Refuse for a prefilter reason, Bank for      *)
(* ACCEPT, Full for BANK_FULL; Drain) and every model variable after it    *)
(* (state).  Record 1 is "init".  Only the verdict is the code's: the bank, *)
(* the tallies and evicted are the walk's own, kept from the verdict, and  *)
(* rocketnet_iocp_bank_datagram and rocketnet_server_iocp_receive_packets  *)
(* are not called (tests/tla_trace_iocp.cpp).                              *)
(*                                                                         *)
(* A copy changes only EXTENDS, VarsRec, OpActs, Act and the row's reach   *)
(* predicates; tools/tla_trace_check.py compares the rest with the         *)
(* template.                                                               *)
(***************************************************************************)
EXTENDS RNIocpReceiveAdmit, Sequences

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
VarsRec == [bank |-> bank, arrived |-> arrived, refused |-> refused, full |-> full,
            counted |-> counted, delivered |-> delivered, evicted |-> evicted]

\* the actions each operation of the code can be
OpActs == [arrive |-> {"Refuse", "Bank", "Full"}, drain |-> {"Drain"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Refuse" -> Refuse(x[1])
               [] a = "Bank" -> Bank(x[1])
               [] a = "Full" -> Full(x[1])
               [] a = "Drain" -> Drain

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

\* the row's reach predicate: some clean trace fills the bank, the edge
\* where Bank turns to Full
BankFull == Len(bank) = Capacity

=============================================================================
