----------------------------- MODULE RNChannel -----------------------------
EXTENDS Naturals, Integers, Sequences, FiniteSets, TLC

\* Bounded message-layer refinement of RNWindow's NumberRecord, WritePacket,
\* OpenPacket, ReceiveAck, and receive/process update boundary (CHANNELS 6).
\* Link is unchanged. Payload units are opaque model values, never serialized.
CONSTANTS W, SendCapacity, P, MaxPid, Capacity, MaxDelay, Duplication,
          AckCapacity, Reordering, FwdPattern, RevPattern, BurstStart, BurstLen,
          BrokenOutOfOrder, BrokenTwice, BrokenDropOnRetransmit,
          BrokenPartialChunk, BrokenChunkBlocksSmall, BrokenNoSequencedReset,
          ProbeSequenced

Channels == 1..2
Messages == 0..3
Nums == 0..4
Channel(m) == IF m = 1 THEN 2 ELSE 1
Parts(m) == CASE m = 0 -> {0} [] m = 1 -> {1}
                  [] m = 2 -> {2,3} [] OTHER -> {4}
Message(n) == CASE n = 0 -> 0 [] n = 1 -> 1 [] n \in {2,3} -> 2 [] OTHER -> 3
Index(n) == IF n = 3 THEN 1 ELSE 0
Order(m) == CASE m \in {0,1} -> 0 [] m = 2 -> 1 [] OTHER -> 2
Payload(n) == <<Message(n), Index(n)>>
Min(S) == CHOOSE x \in S : \A y \in S : x <= y
Number(m,i) == Min(Parts(m)) + i
Pids == 0..MaxPid
Loads == {s \in SUBSET Nums : s # {} /\ Cardinality(s) <= 2 /\
                            (s \cap {2,3} # {} => Cardinality(s) = 1)}
Packets == [id : Pids, nums : Loads \cup {{}}, unrel : BOOLEAN]
AckPackets == (SUBSET Pids) \ {{}}

VARIABLES written, waitedForRoom, roomReturned, refused, uDropped, queued, admitted, acked, smap, forgotten, reserved,
          ordered_write, sequenced_write, ordered_read, sequenced_watermark,
          sentTrace, trace, rxQueue, chunks, received, live, batch, phase, pending,
          duplicateReady, whole, placed, collision, uSent, uCount, seen,
          probeWrite, probeRead, resetOK,
          fwdWire, fwdSent, fwdPulse, revWire, revSent, revPulse

vars == <<written, waitedForRoom, roomReturned, refused, uDropped, queued, admitted, acked, smap,
      forgotten, reserved, ordered_write, sequenced_write, ordered_read, sequenced_watermark, sentTrace,
      trace, rxQueue, chunks, received, live, batch, phase, pending, duplicateReady, whole, placed, collision,
      uSent, uCount, seen, probeWrite, probeRead, resetOK, fwdWire, fwdSent, fwdPulse, revWire, revSent,
      revPulse>>

Fwd == INSTANCE Link WITH Packets <- Packets, Pattern <- FwdPattern,
       wire <- fwdWire, sent <- fwdSent, pulse <- fwdPulse
Rev == INSTANCE Link WITH Packets <- AckPackets, Pattern <- RevPattern,
       Capacity <- AckCapacity,
       wire <- revWire, sent <- revSent, pulse <- revPulse
R == IF Nums \ received = {} THEN 5 ELSE Min(Nums \ received)
RBits == {n \in received : n >= R}
Numbered == UNION {Parts(m) : m \in admitted}
Unacked == Numbered \ acked
NextNum == IF admitted = {} THEN 0 ELSE Cardinality(Numbered)
WindowBase(a) == LET u == IF Numbered \ a = {} THEN NextNum ELSE Min(Numbered \ a)
                 IN IF u \in {2,3} THEN 2 ELSE u
U == IF Unacked = {} THEN NextNum ELSE Min(Unacked)
UPrime == WindowBase(acked)
RoomForLarge(a) == 3 < WindowBase(a) + W
Outstanding == {m \in 0..queued-1 : ~(Parts(m) \subseteq acked)}
InMap == {smap[i].id : i \in DOMAIN smap}
InWires == {fwdWire[i].pkt.id : i \in DOMAIN fwdWire} \cup
           UNION {revWire[i].pkt : i \in DOMAIN revWire}
FreePids == Pids \ (InMap \cup InWires \cup pending)
Complete(m) == Parts(m) \subseteq received
Ready(m) ==
 /\ m < queued
 /\ (Complete(m) \/ (BrokenPartialChunk /\ m = 2 /\ 3 \in received))
 /\ (Order(m) >= ordered_read[Channel(m)] \/ m \in duplicateReady)
 /\ (BrokenOutOfOrder \/ Order(m) = ordered_read[Channel(m)] \/ m \in duplicateReady)
ReadySet == {m \in Messages : Ready(m)}
\* The application's one receive queue, pumped by each receive: rxQueue is the
\* update's deliveries in delivery order, every channel's together, never
\* filtered by channel (sdk/rocketnet_message_server.cpp, rn_message_object_next); EndUpdate
\* ends the update's views and moves the queue into trace, what the
\* application has received on each channel in earlier updates. Trace(c) is
\* everything received on channel c, the queue's projection included.
OnChannel(q, c) == SelectSeq(q, LAMBDA x : Channel(x) = c)
Trace(c) == trace[c] \o OnChannel(rxQueue, c)

Init ==
 /\ written = {} /\ waitedForRoom = FALSE /\ roomReturned = FALSE
 /\ refused = FALSE /\ uDropped = 0
 /\ queued = 0 /\ admitted = {} /\ acked = {} /\ smap = <<>>
 /\ forgotten = {} /\ reserved = 0
 /\ ordered_write = [c \in Channels |-> 0]
 /\ sequenced_write = [c \in Channels |-> 0]
 /\ ordered_read = [c \in Channels |-> 0]
 /\ sequenced_watermark = [c \in Channels |-> 0]
 /\ sentTrace = [c \in Channels |-> <<>>] /\ trace = sentTrace /\ rxQueue = <<>>
 /\ chunks = [m \in Messages |-> [i \in 0..Cardinality(Parts(m))-1 |-> <<-1,-1>>]]
 /\ received = {} /\ live = {} /\ batch = {} /\ phase = "receive"
 /\ pending = {} /\ duplicateReady = {} /\ whole = TRUE
 /\ placed = TRUE /\ collision = FALSE /\ uSent = FALSE /\ uCount = 0
 /\ seen = {} /\ probeWrite = FALSE /\ probeRead = FALSE /\ resetOK = TRUE
 /\ Fwd!Init /\ Rev!Init

Enqueue ==
 /\ (~ProbeSequenced \/ probeWrite)
 /\ queued < 4 /\ Cardinality(Outstanding) < SendCapacity
 /\ queued' = queued + 1
 /\ waitedForRoom' = (waitedForRoom \/ (queued = 2 /\ ~RoomForLarge(acked)))
 /\ ordered_write' = [ordered_write EXCEPT ![Channel(queued)] = @ + 1]
 /\ sequenced_write' = IF BrokenNoSequencedReset THEN sequenced_write
                        ELSE [sequenced_write EXCEPT ![Channel(queued)] = 0]
 /\ sentTrace' = [sentTrace EXCEPT ![Channel(queued)] = Append(@, queued)]
 /\ resetOK' = (resetOK /\ sequenced_write'[Channel(queued)] = 0)
 /\ UNCHANGED <<written, roomReturned, refused, uDropped, admitted, acked, smap, forgotten, reserved,
      ordered_read, sequenced_watermark, trace, rxQueue, chunks, received, live, batch, phase, pending,
      duplicateReady, whole, placed, collision, uSent, uCount, seen, probeWrite, probeRead, fwdWire,
      fwdSent, fwdPulse, revWire, revSent, revPulse>>

NumberRecord ==
 /\ Cardinality(admitted) < queued
 /\ LET m == Cardinality(admitted) IN
      /\ \A n \in Parts(m) : n < UPrime + W
      /\ admitted' = admitted \cup {m}
 /\ UNCHANGED <<written, waitedForRoom, roomReturned, refused, uDropped, queued, acked, smap, forgotten,
      reserved, ordered_write, sequenced_write, ordered_read, sequenced_watermark, sentTrace, trace, rxQueue,
      chunks, received, live, batch, phase, pending, duplicateReady, whole, placed, collision, uSent,
      uCount, seen, probeWrite, probeRead, resetOK, fwdWire, fwdSent, fwdPulse, revWire, revSent,
      revPulse>>

ReserveBeforeRoom ==
 /\ BrokenChunkBlocksSmall /\ 2 < queued /\ 2 \notin admitted
 /\ waitedForRoom /\ ~RoomForLarge(acked) /\ Unacked # {} /\ reserved = 0 /\ reserved' = W
 /\ UNCHANGED <<written, waitedForRoom, roomReturned, refused, uDropped, queued, admitted, acked, smap,
      forgotten, ordered_write, sequenced_write, ordered_read, sequenced_watermark, sentTrace, trace, rxQueue,
      chunks, received, live, batch, phase, pending, duplicateReady, whole, placed, collision, uSent,
      uCount, seen, probeWrite, probeRead, resetOK, fwdWire, fwdSent, fwdPulse, revWire, revSent,
      revPulse>>

WritePacket(s, pid, unreliable) ==
 /\ phase = "receive" /\ FreePids # {} /\ pid = Min(FreePids)
 /\ s \subseteq Unacked \ forgotten /\ (s = {} \/ s \in Loads)
 /\ (s \cap {2,3,4} # {} => reserved = 0)
 /\ \A n \in s \ written : ((Numbered \ written) \cap 0..n) \subseteq s
 /\ written' = written \cup s
 /\ (unreliable => ~uSent /\ s \cap {2,3} = {}) /\ (s # {} \/ unreliable)
 /\ Fwd!Send([id |-> pid, nums |-> s, unrel |-> unreliable])
 /\ smap' = IF s = {} THEN smap ELSE
       LET a == Append(smap, [id |-> pid, nums |-> s])
       IN IF Len(a) > P THEN Tail(a) ELSE a
 /\ forgotten' = IF BrokenDropOnRetransmit /\ fwdWire' = fwdWire
                   THEN forgotten \cup s ELSE forgotten
 /\ uSent' = (uSent \/ unreliable)
 \* Fresh-packet alpha-renaming: reuse only after map, wires and ACKs quiesce.
 /\ seen' = seen \ {pid}
 /\ UNCHANGED << waitedForRoom, roomReturned, refused, uDropped, queued, admitted, acked, reserved,
      ordered_write, sequenced_write, ordered_read, sequenced_watermark, sentTrace, trace, rxQueue, chunks,
      received, live, batch, phase, pending, duplicateReady, whole, placed, collision, uCount,
      probeWrite, probeRead, resetOK, revWire, revSent, revPulse>>

OpenPacket(p) ==
 /\ (~ProbeSequenced \/ probeRead)
 /\ phase = "receive" /\ Fwd!Deliver(p)
 /\ LET fresh == IF p.id \in seen THEN {} ELSE p.nums \ received
        again == IF BrokenTwice /\ p.id \notin seen
                    THEN {Message(n) : n \in p.nums \cap received} ELSE {}
    IN /\ received' = received \cup fresh
       /\ live' = live \cup fresh
       /\ chunks' = [m \in Messages |-> [i \in 0..Cardinality(Parts(m))-1 |->
             IF Number(m,i) \in fresh THEN Payload(Number(m,i)) ELSE chunks[m][i]]]
       /\ collision' = (collision \/
             (\E n \in fresh, k \in live : n # k /\ n % W = k % W))
       /\ duplicateReady' = duplicateReady \cup again
 /\ pending' = pending \cup {p.id}
 /\ uCount' = uCount + IF p.unrel /\ p.id \notin seen THEN 1 ELSE 0
 /\ seen' = seen \cup {p.id}
 /\ UNCHANGED <<written, waitedForRoom, roomReturned, refused, uDropped, queued, admitted, acked, smap,
      forgotten, reserved, ordered_write, sequenced_write, ordered_read, sequenced_watermark, sentTrace,
      trace, rxQueue, batch, phase, whole, placed, uSent, probeWrite, probeRead, resetOK, revWire, revSent,
      revPulse>>

DeliverMessage(m) ==
 /\ phase = "receive" /\ m \in ReadySet
 /\ rxQueue' = Append(rxQueue, m)
 /\ ordered_read' = [ordered_read EXCEPT ![Channel(m)] = Order(m)+1]
 /\ sequenced_watermark' = IF BrokenNoSequencedReset THEN sequenced_watermark
                            ELSE [sequenced_watermark EXCEPT ![Channel(m)] = 0]
 /\ resetOK' = (resetOK /\ sequenced_watermark'[Channel(m)] = 0)
 /\ batch' = batch \cup (Parts(m) \cap received)
 /\ whole' = (whole /\ Complete(m))
 /\ placed' = (placed /\ (\A n \in Parts(m) : chunks[Message(n)][Index(n)] = Payload(n)))
 /\ duplicateReady' = duplicateReady \ {m}
 /\ UNCHANGED <<written, waitedForRoom, roomReturned, refused, uDropped, queued, admitted, acked, smap,
      forgotten, reserved, ordered_write, sequenced_write, sentTrace, trace, chunks, received, live, phase,
      pending, collision, uSent, uCount, seen, probeWrite, probeRead, fwdWire, fwdSent, fwdPulse,
      revWire, revSent, revPulse>>

FixBatch ==
 /\ phase = "receive" /\ ReadySet = {} /\ phase' = "process"
 /\ UNCHANGED <<written, waitedForRoom, roomReturned, refused, uDropped, queued, admitted, acked, smap,
      forgotten, reserved, ordered_write, sequenced_write, ordered_read, sequenced_watermark, sentTrace,
      trace, rxQueue, chunks, received, live, batch, pending, duplicateReady, whole, placed, collision, uSent,
      uCount, seen, probeWrite, probeRead, resetOK, fwdWire, fwdSent, fwdPulse, revWire, revSent,
      revPulse>>

SendAck ==
 /\ phase = "process" /\ pending # {} /\ Rev!Send(pending)
 /\ pending' = {}
 /\ UNCHANGED <<written, waitedForRoom, roomReturned, refused, uDropped, queued, admitted, acked, smap,
      forgotten, reserved, ordered_write, sequenced_write, ordered_read, sequenced_watermark, sentTrace,
      trace, rxQueue, chunks, received, live, batch, phase, duplicateReady, whole, placed, collision, uSent,
      uCount, seen, probeWrite, probeRead, resetOK, fwdWire, fwdSent, fwdPulse>>

EndUpdate ==
 /\ phase = "process" /\ phase' = "receive"
 /\ live' = live \ batch /\ batch' = {}
 /\ trace' = [c \in Channels |-> trace[c] \o OnChannel(rxQueue, c)]
 /\ rxQueue' = <<>>
 /\ chunks' = [m \in Messages |-> [i \in 0..Cardinality(Parts(m))-1 |->
       IF Number(m,i) \in batch THEN <<-1,-1>> ELSE chunks[m][i]]]
 /\ UNCHANGED <<written, waitedForRoom, roomReturned, refused, uDropped, queued, admitted, acked, smap,
      forgotten, reserved, ordered_write, sequenced_write, ordered_read, sequenced_watermark, sentTrace,
      received, pending, duplicateReady, whole, placed, collision, uSent, uCount, seen,
      probeWrite, probeRead, resetOK, fwdWire, fwdSent, fwdPulse, revWire, revSent, revPulse>>

ReceiveAck(q) ==
 /\ Rev!Deliver(q)
 /\ acked' = acked \cup UNION {smap[i].nums : i \in {j \in DOMAIN smap : smap[j].id \in q}}
 /\ roomReturned' = (roomReturned \/ (waitedForRoom /\ RoomForLarge(acked')))
 /\ smap' = SelectSeq(smap, LAMBDA x : x.id \notin q)
 /\ UNCHANGED <<written, waitedForRoom, refused, uDropped, queued, admitted, forgotten, reserved,
      ordered_write, sequenced_write, ordered_read, sequenced_watermark, sentTrace, trace, rxQueue, chunks,
      received, live, batch, phase, pending, duplicateReady, whole, placed, collision, uSent, uCount,
      seen, probeWrite, probeRead, resetOK, fwdWire, fwdSent, fwdPulse>>

GhostSequencedWrite ==
 /\ ProbeSequenced /\ ~probeWrite /\ probeWrite' = TRUE
 /\ sequenced_write' = [sequenced_write EXCEPT ![1] = 1]
 /\ UNCHANGED <<written, waitedForRoom, roomReturned, refused, uDropped, queued, admitted, acked, smap,
      forgotten, reserved, ordered_write, ordered_read, sequenced_watermark, sentTrace, trace, rxQueue, chunks,
      received, live, batch, phase, pending, duplicateReady, whole, placed, collision, uSent, uCount,
      seen, probeRead, resetOK, fwdWire, fwdSent, fwdPulse, revWire, revSent, revPulse>>

GhostSequencedRead ==
 /\ ProbeSequenced /\ ~probeRead /\ probeRead' = TRUE
 /\ sequenced_watermark' = [sequenced_watermark EXCEPT ![1] = 1]
 /\ UNCHANGED <<written, waitedForRoom, roomReturned, refused, uDropped, queued, admitted, acked, smap,
      forgotten, reserved, ordered_write, sequenced_write, ordered_read, sentTrace, trace, rxQueue, chunks,
      received, live, batch, phase, pending, duplicateReady, whole, placed, collision, uSent, uCount,
      seen, probeWrite, resetOK, fwdWire, fwdSent, fwdPulse, revWire, revSent, revPulse>>

FwdEnvironment ==
 /\ Fwd!Environment
 /\ UNCHANGED <<written, waitedForRoom, roomReturned, refused, uDropped, queued, admitted, acked, smap,
      forgotten, reserved, ordered_write, sequenced_write, ordered_read, sequenced_watermark, sentTrace,
      trace, rxQueue, chunks, received, live, batch, phase, pending, duplicateReady, whole, placed, collision,
      uSent, uCount, seen, probeWrite, probeRead, resetOK, revWire, revSent, revPulse>>

RevEnvironment ==
 /\ Rev!Environment
 /\ UNCHANGED <<written, waitedForRoom, roomReturned, refused, uDropped, queued, admitted, acked, smap,
      forgotten, reserved, ordered_write, sequenced_write, ordered_read, sequenced_watermark, sentTrace,
      trace, rxQueue, chunks, received, live, batch, phase, pending, duplicateReady, whole, placed, collision,
      uSent, uCount, seen, probeWrite, probeRead, resetOK, fwdWire, fwdSent, fwdPulse>>


RefuseSend ==
 /\ queued < 4 /\ Cardinality(Outstanding) = SendCapacity
 /\ ~refused /\ refused' = TRUE
 /\ UNCHANGED <<written, waitedForRoom, roomReturned, uDropped, queued, admitted, acked, smap, forgotten,
      reserved, ordered_write, sequenced_write, ordered_read, sequenced_watermark, sentTrace, trace, rxQueue,
      chunks, received, live, batch, phase, pending, duplicateReady, whole, placed, collision, uSent,
      uCount, seen, probeWrite, probeRead, resetOK, fwdWire, fwdSent, fwdPulse, revWire, revSent,
      revPulse>>

DropUnreliableForBudget ==
 /\ ~uSent /\ uSent' = TRUE /\ uDropped' = 1
 /\ UNCHANGED <<written, waitedForRoom, roomReturned, refused, queued, admitted, acked, smap, forgotten,
      reserved, ordered_write, sequenced_write, ordered_read, sequenced_watermark, sentTrace, trace, rxQueue,
      chunks, received, live, batch, phase, pending, duplicateReady, whole, placed, collision, uCount,
      seen, probeWrite, probeRead, resetOK, fwdWire, fwdSent, fwdPulse, revWire, revSent, revPulse>>

WriteAny == \E s \in Loads \cup {{}}, pid \in Pids, u \in BOOLEAN : WritePacket(s,pid,u)
WriteOldest == \E s \in Loads, pid \in Pids : U \in s /\ WritePacket(s,pid,FALSE)
WriteOldestThrough == WriteOldest /\ fwdWire' # fwdWire
OpenHead == fwdWire # <<>> /\ OpenPacket(Head(fwdWire).pkt)
DeliverAny == \E m \in Messages : DeliverMessage(m)
ReceiveAckAny == revWire # <<>> /\ ReceiveAck(Head(revWire).pkt)
ReceiveUsefulAck == ReceiveAckAny /\ acked' # acked
SendAckThrough == SendAck /\ revWire' # revWire
UpdateLoop == FixBatch \/ EndUpdate
Next == RefuseSend \/ DropUnreliableForBudget \/ Enqueue \/ NumberRecord \/ ReserveBeforeRoom \/ WriteAny \/ OpenHead
     \/ DeliverAny \/ FixBatch \/ SendAck \/ EndUpdate \/ ReceiveAckAny
     \/ GhostSequencedWrite \/ GhostSequencedRead \/ FwdEnvironment \/ RevEnvironment

\* Same compact fairness as RNWindow: fair RTO/room/budget, packet opening,
\* and useful transport ACKs before sent-map eviction. No application ACK.
ProbeAny == GhostSequencedWrite \/ GhostSequencedRead
\* Each local action advances a bounded monotone cursor. None can run
\* infinitely to starve another, so one weak-fair disjunction suffices.
LocalProgress == ProbeAny \/ Enqueue \/ NumberRecord \/ DeliverAny
Fairness == /\ WF_vars(LocalProgress)
 /\ SF_vars(WriteOldest) /\ SF_vars(WriteOldestThrough)
 /\ SF_vars(OpenHead) /\ WF_vars(UpdateLoop)
 /\ SF_vars(SendAck) /\ SF_vars(SendAckThrough) /\ SF_vars(ReceiveAckAny)
 /\ (([]<> <<SendAckThrough>>_vars) => ([]<> <<ReceiveUsefulAck>>_vars))
Spec == Init /\ [][Next]_vars /\ Fairness

\* Safety-only view: arbitrary loss ignores pattern counters; pulse serves
\* temporal action fairness only. All protocol and wire state is retained.
SafetyView == <<written, waitedForRoom, roomReturned, refused, uDropped, queued, admitted, acked, smap,
      forgotten, reserved, ordered_write, sequenced_write, ordered_read, sequenced_watermark, sentTrace,
      [c \in Channels |-> Trace(c)], chunks, received, live, batch, phase, pending, duplicateReady, whole, placed, collision,
      uSent, uCount, seen, probeWrite, probeRead, resetOK, fwdWire, revWire>>

SafetySpec == Init /\ [][Next]_vars

OrderedExactlyOnceInOrder == \A c \in Channels :
 IF Len(Trace(c)) <= Len(sentTrace[c])
 THEN Trace(c) = SubSeq(sentTrace[c], 1, Len(Trace(c))) ELSE FALSE
WholeMessagesOnly == whole
ChunksPlacedByIndex == placed /\ \A n \in live : chunks[Message(n)][Index(n)] = Payload(n)
UnreliableAtMostOnce == uCount <= 1
TypeOK == /\ queued \in 0..4 /\ admitted \subseteq Messages
 /\ written \subseteq Numbered /\ acked \subseteq written /\ received \subseteq Numbered
 /\ live \subseteq received /\ batch \subseteq live
 /\ smap \in Seq([id : Pids, nums : Loads])
 /\ phase \in {"receive","process"} /\ pending \subseteq Pids /\ seen \subseteq Pids
 /\ ordered_write \in [Channels -> 0..3] /\ ordered_read \in [Channels -> 0..3]
 /\ sequenced_write \in [Channels -> 0..1] /\ sequenced_watermark \in [Channels -> 0..1]
 /\ uDropped \in 0..1 /\ refused \in BOOLEAN
 /\ reserved \in {0,W} /\ forgotten \subseteq written
 /\ waitedForRoom \in BOOLEAN /\ roomReturned \in BOOLEAN
 /\ sentTrace \in [Channels -> Seq(Messages)] /\ trace \in [Channels -> Seq(Messages)] /\ rxQueue \in Seq(Messages)
 /\ \A m \in Messages : \A i \in 0..Cardinality(Parts(m))-1 :
      chunks[m][i] \in {<<-1,-1>>} \cup {Payload(n) : n \in Nums}
 /\ Fwd!TypeOK /\ Rev!TypeOK
BuffersBounded == /\ Cardinality(Outstanding) <= SendCapacity
 /\ Cardinality(Unacked) <= W /\ (\A n \in RBits : n < R + W) /\ Cardinality(live) <= W
 /\ Cardinality(pending) <= MaxPid+1 /\ Cardinality(seen) <= MaxPid+1
 /\ \A n \in Nums \ live : chunks[Message(n)][Index(n)] = <<-1,-1>>
 /\ Len(smap) <= P /\ reserved <= W
 /\ Fwd!InFlightBounded /\ Rev!InFlightBounded
NoSlotCollision == ~collision /\ \A n,k \in live : n # k => n % W # k % W
\* Single-run projection plus coupling: the ghost never enters sentTrace or
\* application delivery; both ordered transitions reset their epoch counter.
OrderedTraceUnchangedBySequenced == OrderedExactlyOnceInOrder /\ resetOK
EveryOrderedMessageDelivered == <>(\A c \in Channels : Trace(c) = sentTrace[c] /\ queued = 4)
NoHeadOfLineDeadlock == roomReturned ~> (2 \in {Trace(1)[i] : i \in DOMAIN Trace(1)} /\ 3 \in {Trace(1)[i] : i \in DOMAIN Trace(1)})
=============================================================================
