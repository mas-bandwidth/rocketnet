---------------------------- MODULE TraceRNChannel ----------------------------
(***************************************************************************)
(* The trace check of RNChannel, a copy of the template                    *)
(* TraceRNTraceExample.tla (README-TraceCheck.md).  TraceLog is the record *)
(* sequence tests/tla_trace_channel.cpp writes: one per step, with the     *)
(* operation the walk called (op), the walk's arguments to it (args), the  *)
(* model action the code's own result says it took (act), and every model *)
(* variable after it (state).  Record 1 is "init".                         *)
(*                                                                         *)
(* TraceStep allows only the recorded action with the recorded arguments,  *)
(* and only when the operation can be that action (OpActs): a step that    *)
(* did one thing but landed in a state another action allows is rejected.  *)
(* The step must be a Next step too, so Next's ranges and guards around an *)
(* action (o \in Honest, d \in BOOLEAN) hold for the recorded arguments.   *)
(* A state that names fewer or other fields than VarsRec never matches,    *)
(* and a record that is not [op, act, args, state] fails the ASSUME: an    *)
(* under-logged harness fails red, never green.                            *)
(*                                                                         *)
(* A copy changes only EXTENDS, VarsRec, OpActs, Act and the row's reach   *)
(* predicates; tools/tla_trace_check.py compares the rest with this text   *)
(* and checks the shapes of those.                                         *)
(*                                                                         *)
(* When no step matches, TraceReject sets rejected, so TraceConsistent     *)
(* fails; the behaviour TLC prints ends at ti, the last record that        *)
(* matched, and record ti + 1 is the first code step that is not a model   *)
(* step.  At the last record the trace stutters (TraceDone).  Nothing here *)
(* quantifies over the log in a state formula, and nothing reads TraceLog  *)
(* but Trace (below).                                                      *)
(***************************************************************************)
EXTENDS RNChannel, Sequences

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

\* every model variable, by the field name the harness writes
VarsRec == [written |-> written, waitedForRoom |-> waitedForRoom, roomReturned |-> roomReturned, refused |-> refused,
            uDropped |-> uDropped, queued |-> queued, admitted |-> admitted, acked |-> acked, smap |-> smap,
            forgotten |-> forgotten, reserved |-> reserved, ordered_write |-> ordered_write,
            sequenced_write |-> sequenced_write, ordered_read |-> ordered_read,
            sequenced_watermark |-> sequenced_watermark, sentTrace |-> sentTrace, trace |-> trace,
            rxQueue |-> rxQueue, chunks |-> chunks, received |-> received, live |-> live, batch |-> batch,
            phase |-> phase, pending |-> pending, duplicateReady |-> duplicateReady, whole |-> whole,
            placed |-> placed, collision |-> collision, uSent |-> uSent, uCount |-> uCount, seen |-> seen,
            probeWrite |-> probeWrite, probeRead |-> probeRead, resetOK |-> resetOK, taken |-> taken,
            notReceived |-> notReceived, misused |-> misused, deliveredCount |-> deliveredCount,
            fwdWire |-> fwdWire, fwdSent |-> fwdSent, fwdPulse |-> fwdPulse, revWire |-> revWire,
            revSent |-> revSent, revPulse |-> revPulse]

\* the actions each operation of the code can be: a write pass numbers
\* messages, writes a packet or drops the unreliable send for the budget, as
\* the code's numbering, its written packet and its counter say; a payload
\* opened is one OpenPacket and then one DeliverMessage per delivery the code
\* returned; the rest are one action each
OpActs == [probe_write |-> {"GhostSequencedWrite"}, probe_read |-> {"GhostSequencedRead"},
           send_ordered |-> {"Enqueue"},
           write_pass |-> {"NumberRecord", "WritePacket", "DropUnreliableForBudget"},
           open_payload |-> {"OpenPacket", "DeliverMessage"},
           fix_batch |-> {"FixBatch"}, send_ack |-> {"SendAck"}, end_update |-> {"EndUpdate"},
           acked_ids |-> {"ReceiveAck"},
           fwd_drop |-> {"FwdEnvironment"}, fwd_duplicate |-> {"FwdEnvironment"}, fwd_reorder |-> {"FwdEnvironment"},
           rev_drop |-> {"RevEnvironment"}, rev_duplicate |-> {"RevEnvironment"}, rev_reorder |-> {"RevEnvironment"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "GhostSequencedWrite" -> GhostSequencedWrite
               [] a = "GhostSequencedRead" -> GhostSequencedRead
               [] a = "Enqueue" -> Enqueue
               [] a = "NumberRecord" -> NumberRecord
               [] a = "WritePacket" -> WritePacket(x[1], x[2], x[3])
               [] a = "DropUnreliableForBudget" -> DropUnreliableForBudget
               [] a = "OpenPacket" -> OpenPacket(x[1])
               [] a = "DeliverMessage" -> DeliverMessage(x[1])
               [] a = "FixBatch" -> FixBatch
               [] a = "SendAck" -> SendAck
               [] a = "EndUpdate" -> EndUpdate
               [] a = "ReceiveAck" -> ReceiveAck(x[1])
               [] a = "FwdEnvironment" -> FwdEnvironment
               [] a = "RevEnvironment" -> RevEnvironment

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

\* the row's reach predicates: some clean trace must open the block's second
\* chunk before its first; pack the two first small messages in one packet;
\* hold one reliable number in two live sent-map entries (a resend under a
\* fresh packet id); receive message 3's number while message 2 is short
\* (message 3 held behind it); deliver every message in order; deliver the
\* unreliable send, and drop it for the budget; and carry two copies of one
\* packet on the forward wire
ChunksReordered == 3 \in received /\ 2 \notin received
SmallPacked == \E i \in DOMAIN smap : smap[i].nums = {0, 1}
Resent == \E i, j \in DOMAIN smap : i # j /\ smap[i].nums \cap smap[j].nums # {}
HeldBehind == 4 \in received /\ ~({2, 3} \subseteq received)
AllDelivered == queued = 4 /\ \A c \in Channels : ChannelTrace(c) = sentTrace[c]
UnreliableDelivered == uCount = 1
UnreliableDropped == uDropped = 1
WireDuplicate == \E i, j \in DOMAIN fwdWire : i # j /\ fwdWire[i].pkt = fwdWire[j].pkt

=============================================================================
