----------------------- MODULE TraceRNXdpReceiveRing -----------------------
(***************************************************************************)
(* The trace check of RNXdpReceiveRing (README-TraceCheck.md).  TraceLog   *)
(* is the record sequence tests/tla_trace_xdp_receive_ring.cpp writes over *)
(* the XDP worker's receive code (sdk/rocketnet_xdp_receive_pass.h and     *)
(* rocketnet_xdp_receive_ring_finish), on libxdp's ring views over rings   *)
(* the walk lays out: one record per step, [op, act, args, state], with    *)
(* the operation the walk called, the model action the code's result says  *)
(* it took, the walk's arguments to it (none: no action here takes a       *)
(* parameter) and every model variable after it.  Record 1 is "init".      *)
(*                                                                         *)
(* The operations:                                                         *)
(*   kernel_receive, kernel_refresh, kernel_misdeliver  the walk as the    *)
(*            kernel (no library call): KernelReceive, KernelRefresh,      *)
(*            KernelMisdeliver                                             *)
(*   peek     rocketnet_xdp_receive_peek: Peek                             *)
(*   take     rocketnet_xdp_receive_take: Take                             *)
(*   finish   rocketnet_xdp_receive_end, then rocketnet_xdp_fill_flush:    *)
(*            Finish                                                       *)
(*   flush    rocketnet_xdp_fill_flush at the top of a pass: Refill        *)
(*   drain    the application takes the staged packets: Drain              *)
(*                                                                         *)
(* The model's Init and its flushes leave the fill ring's order free       *)
(* (Orders).  TraceInit takes fq from record 1 before Init checks it, so  *)
(* the initial states are the one the code made, never one per order      *)
(* (the others would each be a rejection).  A flush's order is pinned by   *)
(* the next record's fq: a successor in another order is no step.         *)
(*                                                                         *)
(* A record whose state is <<>> is a step inside one call that the walk   *)
(* cannot observe (Matches); it is sound only for an action with one       *)
(* successor from each state.  This layer writes none.                     *)
(*                                                                         *)
(* TraceStep allows only the recorded action, and only when the operation  *)
(* can be that action (OpActs).  When no step matches, TraceReject sets    *)
(* rejected, so TraceConsistent fails; record ti + 1 is the first code     *)
(* step that is not a model step.  The log is read through Records, a      *)
(* definition, which TLC evaluates once; TraceLog, an overridden constant, *)
(* it rebuilds at every reference.                                         *)
(***************************************************************************)
EXTENDS RNXdpReceiveRing, Sequences

CONSTANT TraceLog

Records == TraceLog

\* checked once, when TLC starts
ASSUME /\ Len(Records) >= 2
       /\ \A n \in DOMAIN Records : DOMAIN Records[n] = {"op", "act", "args", "state"}

VARIABLES ti,        \* the index of the record the state matches
          rejected   \* the record after ti is not a model step

\* every model variable by the field name the harness writes
VarsRec == [fq |-> fq, kcache |-> kcache, kused |-> kused, ring |-> ring, ahead |-> ahead,
            cprod |-> cprod, phase |-> phase, start |-> start, peeked |-> peeked, taken |-> taken,
            pending |-> pending, staged |-> staged, took |-> took, defect |-> defect]

\* the actions each operation of the code can be
OpActs == [kernel_receive |-> {"KernelReceive"},
           kernel_refresh |-> {"KernelRefresh"},
           kernel_misdeliver |-> {"KernelMisdeliver"},
           peek |-> {"Peek"},
           take |-> {"Take"},
           finish |-> {"Finish"},
           flush |-> {"Refill"},
           drain |-> {"Drain"}]

\* the action a record names; a name this CASE lacks stops TLC with an error
Act(a, x) == CASE a = "KernelReceive" -> KernelReceive
               [] a = "KernelRefresh" -> KernelRefresh
               [] a = "KernelMisdeliver" -> KernelMisdeliver
               [] a = "Peek" -> Peek
               [] a = "Take" -> Take
               [] a = "Finish" -> Finish
               [] a = "Refill" -> Refill
               [] a = "Drain" -> Drain

\* the successor is the record's state, or the record is a hidden step
Matches(r) == IF DOMAIN r.state = {} THEN TRUE ELSE VarsRec' = r.state

TraceInit ==
    /\ fq = Records[1].state.fq
    /\ Init
    /\ ti = 1
    /\ rejected = (Records[1].op # "init" \/ Records[1].act # "init" \/ VarsRec # Records[1].state)

TraceStep ==
    /\ ~rejected
    /\ ti < Len(Records)
    /\ LET r == Records[ti + 1]
       IN /\ r.op \in DOMAIN OpActs
          /\ r.act \in OpActs[r.op]
          /\ Act(r.act, r.args)
          /\ Matches(r)
    /\ ti' = ti + 1
    /\ UNCHANGED rejected

TraceReject ==
    /\ ~rejected
    /\ ti < Len(Records)
    /\ ~ENABLED TraceStep
    /\ rejected' = TRUE
    /\ UNCHANGED <<vars, ti>>

TraceDone ==
    /\ rejected \/ ti = Len(Records)
    /\ UNCHANGED <<vars, ti, rejected>>

TraceSpec == TraceInit /\ [][TraceStep \/ TraceReject \/ TraceDone]_<<vars, ti, rejected>>

\* every record of the trace is the model step it names, from the one before
TraceConsistent == ~rejected

=============================================================================
