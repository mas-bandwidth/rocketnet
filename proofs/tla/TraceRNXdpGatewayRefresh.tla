---------------------- MODULE TraceRNXdpGatewayRefresh ----------------------
(***************************************************************************)
(* The trace check of RNXdpGatewayRefresh (README-TraceCheck.md).          *)
(* TraceLog is the record sequence tests/tla_trace_xdp_gateway_refresh.cpp *)
(* writes from the library's rocketnet_xdp_gateway_refresh and             *)
(* rocketnet_xdp_reply_mac (sdk/rocketnet_xdp_frame.h): one record per     *)
(* model step, [op, act, args, state], record 1 "init".                    *)
(*                                                                         *)
(* One refresh call that changes the gateway MAC is three model steps:     *)
(* Refresh (to "resolve"), Resolve and WriteLow.  The walk takes the       *)
(* Refresh state inside the resolve callback and the WriteLow state after  *)
(* the call.  The Resolve state, the table with its first half written,   *)
(* is never observable: the code writes the six bytes with one memcpy.     *)
(* Its record is hidden: its state is <<>>, and TraceStep takes whatever   *)
(* the model's Resolve gives (Matches).  Resolve is deterministic given    *)
(* the state, so a hidden record never leaves TLC a choice that the next   *)
(* record could reject on one branch and accept on another.  A hidden      *)
(* record is used for a deterministic action only.                         *)
(*                                                                         *)
(* TraceStep allows only the recorded action, and only when the operation  *)
(* can be that action (OpActs).  A state that names fewer or other fields  *)
(* than VarsRec never matches.  When no step matches, TraceReject sets     *)
(* rejected, so TraceConsistent fails; record ti + 1 is the first code     *)
(* step that is not the model step it names.  At the last record the trace *)
(* stutters (TraceDone).                                                   *)
(***************************************************************************)
EXTENDS RNXdpGatewayRefresh, Sequences

CONSTANT TraceLog

\* checked once, when TLC starts
ASSUME /\ Len(TraceLog) >= 2
       /\ \A n \in DOMAIN TraceLog : DOMAIN TraceLog[n] = {"op", "act", "args", "state"}

VARIABLES ti,        \* the index of the record the state matches
          rejected   \* the record after ti is not a model step

\* every model variable by the field name the harness writes
VarsRec == [phase |-> phase, now |-> now, scheduled |-> scheduled, nextTime |-> nextTime,
            table |-> table, pendingLow |-> pendingLow, resolver |-> resolver,
            changedAt |-> changedAt, changes |-> changes, readHigh |-> readHigh,
            staged |-> staged]

\* the actions each operation of the code can be
OpActs == [refresh |-> {"Refresh", "Resolve", "WriteLow"},
           copy_high |-> {"ReadHigh"},
           copy_low |-> {"ReadLow"},
           change_gateway |-> {"ChangeGateway"},
           join |-> {"Join"}]

\* the action a record names; a name this CASE lacks stops TLC with an error
Act(a, x) == CASE a = "Refresh" -> Refresh
               [] a = "Resolve" -> Resolve
               [] a = "WriteLow" -> WriteLow
               [] a = "ReadHigh" -> ReadHigh
               [] a = "ReadLow" -> ReadLow
               [] a = "ChangeGateway" -> ChangeGateway
               [] a = "Join" -> Join

\* a hidden record (state <<>>) takes the model's step; any other must equal it
Matches(r) == DOMAIN r.state = {} \/ VarsRec' = r.state

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
          /\ Act(r.act, r.args)
          /\ Matches(r)
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
