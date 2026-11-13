----------------------- MODULE TraceRNXdpReceiveRing -----------------------
(***************************************************************************)
(* The trace check of RNXdpReceiveRing, copied from the template           *)
(* TraceRNTraceExample.tla (README-TraceCheck.md).  TraceLog is the record *)
(* sequence tests/tla_trace_xdp_receive_ring.cpp writes over the XDP       *)
(* worker's receive code (sdk/rocketnet_xdp_receive_pass.h and             *)
(* rocketnet_xdp_receive_ring_finish), on libxdp's ring views over rings   *)
(* the walk lays out: one record per call, with the call (op), its         *)
(* arguments (args: none, no action here takes a parameter; what a call    *)
(* gives back, the descriptors a peek returned and the frames a take       *)
(* holds, is in the state as peeked and pending), the model action its     *)
(* result says it took (act), and every model variable after it (state).   *)
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
(* Init fills the ring in frame order, as the server does; a flush's order *)
(* is free in the model and pinned by the next record's fq.  Everything    *)
(* but EXTENDS, VarsRec, OpActs, Act and the reach predicates is the       *)
(* template's text.                                                        *)
(***************************************************************************)
EXTENDS RNXdpReceiveRing, Sequences

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
    [fq |-> fq, kcache |-> kcache, kused |-> kused, ring |-> ring, ahead |-> ahead,
     cprod |-> cprod, phase |-> phase, start |-> start, peeked |-> peeked, taken |-> taken,
     pending |-> pending, staged |-> staged, took |-> took, defect |-> defect]

\* the actions each operation of the code can be
OpActs == [kernel_receive |-> {"KernelReceive"}, kernel_refresh |-> {"KernelRefresh"},
           kernel_misdeliver |-> {"KernelMisdeliver"}, peek |-> {"Peek"}, take |-> {"Take"},
           finish |-> {"Finish"}, flush |-> {"Refill"}, drain |-> {"Drain"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "KernelReceive" -> KernelReceive
               [] a = "KernelRefresh" -> KernelRefresh
               [] a = "KernelMisdeliver" -> KernelMisdeliver
               [] a = "Peek" -> Peek
               [] a = "Take" -> Take
               [] a = "Finish" -> Finish
               [] a = "Refill" -> Refill
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

\* the row's reach predicates, the edges some clean trace must take: the
\* staging buffer full, so the peek is cut to no room; the RX ring full; and
\* a flush that found no room, the worker holding frames between passes
StagingFull == staged = StageCap

RxRingFull == Len(ring) = RingSize

HeldBetweenPasses == phase = "idle" /\ pending # {}

=============================================================================
