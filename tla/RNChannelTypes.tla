--------------------------- MODULE RNChannelTypes ---------------------------
EXTENDS Naturals, Sequences, FiniteSets, TLC

\* Standalone message-stream interface for codes 1, 2, 4, composed with a
\* minimal code-3 receiver. Payloads are opaque identities, never serialized.
\* Enqueue assigns per-channel indices; WritePacket maps a fresh transport
\* packet id to one message. OpenPacket is a transport-delivered record;
\* ReceiveAck consumes ONLY transport packet ids. LosePacket supplies a lost
\* packet id. No message-level ACK or application-delivery ACK exists.
\* The bounded transport permits one original and one resend per reliable
\* message, with arbitrary reorder and loss of originals. Resends are not
\* lost. This is a bounded fair-transport assumption, not a transport proof.
\* Fixed capacities represent preallocated descriptors; no allocator, byte
\* layout, copy-count, bandwidth scheduler or fragment machinery is modelled.
\* The "promote" scenario models the rule that an unreliable send too big for
\* one packet is promoted to its reliable twin: 0 to 2, 1 to 4, 5 to the
\* reliable code-2 path (the receipt stays with the layer above).
CONSTANTS Scenario, Broken, SendCapacity, OrderedCapacity, HoldCapacity
Channels == 1..2
Codes == {0,1,2,3,4,5}
PromotionSources == {0,1,5}
Twin(k) == CASE k = 0 -> 2 [] k = 1 -> 4 [] k = 5 -> 2 [] OTHER -> k
PromotePlanCodes == <<1,0,5,2>>
MaxMessages == 4
Messages == 1..MaxMessages
Pids == 1..(2*MaxMessages)
Plan(codes, channels) == [i \in 1..Len(codes) |-> [code |-> codes[i], channel |-> channels[i]]]
Plans ==
    CASE Scenario = "mixed" -> {Plan(<<1,3,4,2>>, <<1,1,1,1>>)}
      [] Scenario = "stale" -> {Plan(<<4,1,3,2>>, <<1,1,1,1>>)}
      [] Scenario = "channels" -> {Plan(<<3,1,3,4>>, <<1,1,2,2>>)}
      [] Scenario = "backwards" -> {Plan(<<1,1,4,2>>, <<1,1,1,1>>)}
      [] Scenario = "reset" -> {Plan(<<3,1,3,4>>, <<1,1,1,1>>)}
      [] Scenario = "twice" -> {Plan(<<2,2,3,1>>, <<1,1,1,1>>)}
      [] Scenario = "hold" -> {Plan(<<3,1,4,2>>, <<1,1,1,1>>)}
      [] Scenario = "promote" -> {Plan(PromotePlanCodes, <<1,1,1,1>>),
                                  Plan(<<1,3,4,2>>, <<1,1,1,1>>)}
      [] OTHER -> {Plan(<<1,3,4,2>>, <<1,1,1,1>>),
                   Plan(<<4,1,3,2>>, <<1,1,1,1>>),
                   Plan(<<1,1,4,2>>, <<1,1,1,1>>),
                   Plan(<<3,1,3,4>>, <<1,1,1,1>>),
                   Plan(<<3,1,4,2>>, <<1,1,1,1>>),
                   Plan(<<2,2,3,1>>, <<1,1,1,1>>),
                   Plan(<<3,1,3,4>>, <<1,1,2,2>>),
                   Plan(<<1,0,5,2>>, <<1,1,1,1>>)}
Reliable(code) == code \in {2,3,4}
Sequenced(code) == code \in {1,4}
Values(s) == {s[i] : i \in DOMAIN s}
PacketMessage(pid) == ((pid-1) \div 2)+1
Packet(m, attempt) == 2*(m-1)+attempt
Min(s) == CHOOSE n \in s : \A k \in s : n <= k
Max(s) == CHOOSE n \in s : \A k \in s : n >= k

VARIABLES plan, queue, ordered_write, sequenced_write, written, numbers,
          opened, lost, acked, resend, receiver, orderedInputs,
          referenceTrace, connected, reason
vars == <<plan, queue, ordered_write, sequenced_write, written, numbers,
          opened, lost, acked, resend, receiver, orderedInputs,
          referenceTrace, connected, reason>>

\* The first three sends of the promote plan are too big for one record, so
\* each unreliable one is promoted to its reliable twin (the promotion rule). The
\* promote scenario also runs the mixed plan with nothing large, so the main
\* configuration keeps checking it.
Large(m) == Scenario = "promote" /\ m \in {1,2,3} /\
            [i \in DOMAIN plan |-> plan[i].code] = PromotePlanCodes

\* Message ids stand for stable reliable numbers after first packet write.
\* numbers records their independent late assignment order; code 1 takes none.
\* A promoted message carries its twin's wire code (0 to 2, 1 to 4, 5 to 2).
Source(m) == queue[m].code
Code(m) == IF Large(m) /\ Source(m) \in PromotionSources THEN Twin(Source(m)) ELSE Source(m)
Channel(m) == queue[m].channel
Order(m) == queue[m].ordering
Sequence(m) == queue[m].sequencing
Newer(a,b) == Order(a) > Order(b) \/ (Order(a) = Order(b) /\ Sequence(a) > Sequence(b))
\* r.trace is the application's one receive queue, pumped by each receive: every
\* code's and every channel's deliveries in delivery order, never filtered
\* by channel (sdk/rocketnet_message_server.cpp, rn_message_object_next). The per-channel
\* ordered and sequenced traces below are its projections.
EmptyReceiver == [ordered_read |-> [c \in Channels |-> 0],
    sequenced_watermark |-> [c \in Channels |-> 0],
    heldOrdered |-> {}, heldSequenced |-> {}, seen |-> {}, trace |-> <<>>]
OrderedTrace(r,c) == SelectSeq(r.trace, LAMBDA m : Code(m) = 3 /\ Channel(m) = c)
SequencedTrace(r,c) == SelectSeq(r.trace, LAMBDA m : Sequenced(Code(m)) /\ Channel(m) = c)
ReliableTrace(r) == SelectSeq(r.trace, LAMBDA m : Code(m) = 2)

\* One delivery function, shared by direct arrival and held-message drain.
DeliverMessage(r,m,held) ==
    IF Code(m) = 3 THEN
      [r EXCEPT !.trace = Append(@,m), !.ordered_read[Channel(m)] = Order(m)+1,
        !.sequenced_watermark[Channel(m)] =
            IF Broken = "HeapDrainNoReset" /\ held THEN @ ELSE 0]
    ELSE IF Sequenced(Code(m)) THEN
      IF Broken = "SequencedBackwards" \/ Sequence(m) >= r.sequenced_watermark[Channel(m)]
      THEN [r EXCEPT !.trace = Append(@,m), !.sequenced_watermark[Channel(m)] = Sequence(m)+1]
      ELSE r
    ELSE [r EXCEPT !.trace = Append(@,m)]

\* Drain consumes a held sequenced value before the ordered successor of
\* that epoch, and repeats. Fuel is a finite recursion bound, not a guard
\* that silently drops work: each recursion removes one held record.
RECURSIVE Drain(_,_,_)
\* Bind the receiver value once per recursive call. This singleton mapping is
\* extensionally the same function; it avoids repeated lazy evaluation of
\* nested record updates while draining several held records. No transition,
\* bound, arrival order, loss action or fairness assumption changes.
Drain(input,c,fuel) ==
    CHOOSE output \in {
        LET stale == {m \in r.heldSequenced : Channel(m)=c /\ Order(m)<r.ordered_read[c]}
            s == [r EXCEPT !.heldSequenced = @ \ stale]
            readyS == {m \in s.heldSequenced : Channel(m)=c /\ Order(m)=s.ordered_read[c]}
            readyO == {m \in s.heldOrdered : Channel(m)=c /\ Order(m)=s.ordered_read[c]}
        IN IF fuel=0 THEN s
           ELSE IF readyS # {} THEN
             LET m == Min(readyS)
                 t == [s EXCEPT !.heldSequenced = @ \ {m}]
             IN Drain(TLCEval(DeliverMessage(t,m,TRUE)), c, fuel-1)
           ELSE IF readyO # {} THEN
             LET m == Min(readyO)
                 t == [s EXCEPT !.heldOrdered = @ \ {m}]
             IN Drain(TLCEval(DeliverMessage(t,m,TRUE)), c, fuel-1)
           ELSE s
      : r \in {input} } : TRUE

Dispatch(r,m) ==
    IF Reliable(Code(m)) /\ m \in r.seen /\ Broken # "ReliableTwice" THEN r
    ELSE LET s == IF Reliable(Code(m)) THEN [r EXCEPT !.seen = @ \cup {m}] ELSE r
         IN IF Code(m)=2 THEN DeliverMessage(s,m,FALSE)
            ELSE IF Order(m)<s.ordered_read[Channel(m)] THEN s
            ELSE IF Order(m)=s.ordered_read[Channel(m)] THEN
              Drain(TLCEval(DeliverMessage(s,m,FALSE)), Channel(m), MaxMessages+1)
            ELSE IF Code(m)=3 THEN [s EXCEPT !.heldOrdered = @ \cup {m}]
            ELSE LET held == {n \in s.heldSequenced : Channel(n)=Channel(m)}
                     newest == held = {} \/ (\A n \in held : Newer(m,n))
                 IN IF Broken="HoldAllSequenced" THEN [s EXCEPT !.heldSequenced = @ \cup {m}]
                    ELSE IF newest THEN [s EXCEPT !.heldSequenced = (@ \ held) \cup {m}]
                    ELSE s

\* Independent ordered-only receiver. Its input is ordered packet arrivals
\* with sequenced traffic erased. It never reads mixed receiver state or
\* uses DeliverMessage/Drain: it derives the contiguous prefix independently.
ReferenceOrder(m) == Cardinality({n \in 1..(m-1) : Code(n)=3 /\ Channel(n)=Channel(m)})
RECURSIVE OrderedReference(_,_,_)
OrderedReference(inputs,c,index) ==
    LET matches == {m \in inputs : Channel(m)=c /\ ReferenceOrder(m)=index}
    IN IF matches={} THEN <<>>
       ELSE <<Min(matches)>> \o OrderedReference(inputs,c,index+1)

Init ==
    /\ plan \in Plans /\ queue= <<>>
    /\ ordered_write=[c \in Channels |-> 0] /\ sequenced_write=ordered_write
    /\ written={} /\ numbers= <<>> /\ opened={} /\ lost={}
    /\ acked={} /\ resend={} /\ receiver=EmptyReceiver /\ orderedInputs={}
    /\ referenceTrace=[c \in Channels |-> <<>>]
    /\ connected=TRUE /\ reason="NONE"

Enqueue ==
    /\ connected /\ Len(queue)<MaxMessages /\ Len(queue)<SendCapacity
    /\ LET m == Len(queue)+1 c == plan[m].channel k == plan[m].code
       IN /\ queue'=Append(queue,[code |-> k, channel |-> c,
                  ordering |-> ordered_write[c], sequencing |-> sequenced_write[c]])
          /\ ordered_write'=IF k=3 THEN [ordered_write EXCEPT ![c]=@+1] ELSE ordered_write
          /\ sequenced_write'=IF k=3 THEN [sequenced_write EXCEPT ![c]=0]
                               ELSE IF Sequenced(k) THEN [sequenced_write EXCEPT ![c]=@+1]
                               ELSE sequenced_write
    /\ UNCHANGED <<connected,reason>>
    /\ UNCHANGED <<plan,written,numbers,opened,lost,acked,resend,
                    receiver,orderedInputs,referenceTrace>>

WritePacket(m,attempt) ==
    /\ connected /\ m \in DOMAIN queue /\ attempt \in 1..2
    /\ Packet(m,attempt) \notin written
    /\ IF attempt=1 THEN \A n \in 1..(m-1) : Packet(n,1) \in written
       ELSE Reliable(Code(m)) /\ Packet(m,1) \in written /\ m \notin acked
    /\ written'=written \cup {Packet(m,attempt)}
    /\ numbers'=IF Reliable(Code(m)) /\ m \notin Values(numbers) THEN Append(numbers,m) ELSE numbers
    /\ resend'=IF Reliable(Code(m)) THEN resend \cup {m} ELSE resend
    /\ UNCHANGED <<plan,queue,ordered_write,sequenced_write,opened,lost,
                    acked,receiver,orderedInputs,referenceTrace,connected,reason>>

OpenPacket(pid) ==
    /\ connected /\ pid \in written \ (opened \cup lost)
    /\ opened'=opened \cup {pid}
    /\ LET m == PacketMessage(pid)
           r == TLCEval(Dispatch(receiver,m))
           overflow == Cardinality(r.heldOrdered)>OrderedCapacity \/
               (Broken # "HoldAllSequenced" /\ Cardinality(r.heldSequenced)>HoldCapacity)
       IN /\ receiver'=IF overflow THEN [r EXCEPT !.heldOrdered={}, !.heldSequenced={} ] ELSE r
          /\ connected'=~overflow
          /\ resend'=IF overflow THEN {} ELSE resend
          /\ reason'=IF overflow THEN "MESSAGE_MEMORY_EXHAUSTED" ELSE reason
          /\ orderedInputs'=IF Code(m)=3 THEN orderedInputs \cup {m} ELSE orderedInputs
    /\ referenceTrace'=[c \in Channels |-> OrderedReference(orderedInputs',c,0)]
    /\ UNCHANGED <<plan,queue,ordered_write,sequenced_write,written,numbers,
                    lost,acked>>

ReceiveAck(q) ==
    /\ connected /\ q # {} /\ q \subseteq opened
    /\ \E pid \in q : Reliable(Code(PacketMessage(pid))) /\ PacketMessage(pid) \notin acked
    /\ LET delivered == {PacketMessage(pid) : pid \in q}
           reliable == {m \in delivered : Reliable(Code(m))}
       IN /\ acked'=acked \cup reliable /\ resend'=resend \ reliable
    /\ UNCHANGED <<plan,queue,ordered_write,sequenced_write,written,numbers,
                    opened,lost,receiver,orderedInputs,referenceTrace,connected,reason>>
LosePacket(pid) ==
    /\ connected /\ pid \in written \ (opened \cup lost) /\ pid % 2 = 1
    /\ lost'=lost \cup {pid}
    /\ UNCHANGED <<plan,queue,ordered_write,sequenced_write,written,numbers,
                    opened,acked,resend,receiver,orderedInputs,referenceTrace,connected,reason>>

MemoryExhausted ==
    /\ connected /\ Len(queue)=SendCapacity /\ Len(queue)<MaxMessages
    /\ connected'=FALSE /\ reason'="MESSAGE_MEMORY_EXHAUSTED"
    /\ resend'={} /\ receiver'=[receiver EXCEPT !.heldOrdered={}, !.heldSequenced={}]
    /\ UNCHANGED <<plan,queue,ordered_write,sequenced_write,written,numbers,
                    opened,lost,acked,orderedInputs,referenceTrace>>

WriteAny == \E m \in Messages, attempt \in 1..2 : WritePacket(m,attempt)
OpenAny == \E pid \in Pids : OpenPacket(pid)
AckAny == \E pid \in Pids : ReceiveAck({pid})
LoseAny == \E pid \in Pids : LosePacket(pid)
Next == MemoryExhausted \/ Enqueue \/ WriteAny \/ OpenAny \/ AckAny \/ LoseAny
SafetySpec == Init /\ [][Next]_vars
\* All enabled actions consume a finite id/cursor. Weak fairness of this
\* disjunction therefore drains every reliable send/arrival/packet ACK.
Spec == SafetySpec /\ WF_vars(Next)

TypeOK ==
    /\ plan \in Plans /\ Len(queue)<=MaxMessages
    /\ queue \in Seq([code:Codes, channel:Channels, ordering:0..MaxMessages, sequencing:0..MaxMessages])
    /\ ordered_write \in [Channels -> 0..MaxMessages]
    /\ sequenced_write \in [Channels -> 0..MaxMessages]
    /\ receiver.ordered_read \in [Channels -> 0..MaxMessages]
    /\ receiver.sequenced_watermark \in [Channels -> 0..MaxMessages]
    /\ receiver.heldOrdered \subseteq DOMAIN queue
    /\ receiver.heldSequenced \subseteq DOMAIN queue
    /\ receiver.trace \in Seq(Messages) /\ Len(receiver.trace)<=2*MaxMessages
    /\ referenceTrace \in [Channels -> Seq(Messages)]
    /\ written \subseteq Pids /\ opened \subseteq written /\ lost \subseteq written
    /\ opened \cap lost={}
    /\ Values(numbers) \subseteq {m \in DOMAIN queue : Reliable(Code(m))}
    /\ Len(numbers)=Cardinality(Values(numbers))
    /\ acked \subseteq Values(numbers) /\ resend \subseteq Values(numbers)
    /\ receiver.seen \subseteq Values(numbers)
    /\ Values(receiver.trace) \subseteq DOMAIN queue
    /\ connected \in BOOLEAN /\ reason \in {"NONE","MESSAGE_MEMORY_EXHAUSTED"}
BuffersBounded ==
    /\ Cardinality(receiver.heldOrdered)<=OrderedCapacity
    /\ Cardinality(receiver.heldSequenced)<=HoldCapacity
    /\ \A c \in Channels : Cardinality({m \in receiver.heldSequenced : Channel(m)=c})<=1
    /\ Len(numbers)<=SendCapacity /\ Cardinality(resend)<=SendCapacity
    /\ Cardinality(written)<=2*MaxMessages
SequencedNeverBackwards == \A c \in Channels :
    LET t == SequencedTrace(receiver,c) IN \A i,j \in DOMAIN t :
        i<j /\ Order(t[i])=Order(t[j]) => Sequence(t[i])<=Sequence(t[j])
NoSequencedDuplicate == \A c \in Channels :
    LET t == SequencedTrace(receiver,c) IN Len(t)=Cardinality(Values(t))
PromotedIsTwin == \A m \in DOMAIN queue :
    Large(m) /\ Source(m) \in PromotionSources => Code(m) = Twin(Source(m))
PromotedNeverUnreliable == \A m \in DOMAIN queue :
    Large(m) /\ Source(m) \in {0,5} /\ m \in {PacketMessage(pid) : pid \in opened} =>
        m \in Values(receiver.trace)
ReliableExactlyOnce ==
    LET t == ReliableTrace(receiver)
        arrived == {PacketMessage(pid) : pid \in opened}
    IN /\ Len(t)=Cardinality(Values(t))
       /\ Values(t)={m \in arrived : Code(m)=2}
OrderedExactlyOnceInOrder == \A c \in Channels :
    LET t == OrderedTrace(receiver,c) IN \A i \in DOMAIN t : Order(t[i])=i-1
OrderedTraceUnchangedBySequenced == \A c \in Channels :
    OrderedTrace(receiver,c)=referenceTrace[c]
SequencedNewestWins == \A c \in Channels :
    LET t == SequencedTrace(receiver,c) IN \A i,j \in DOMAIN t :
        i<j => (Order(t[i])<Order(t[j]) \/
                  (Order(t[i])=Order(t[j]) /\ Sequence(t[i])<=Sequence(t[j])))
EpochWatermark == \A c \in Channels :
    LET now == {m \in Values(SequencedTrace(receiver,c)) : Order(m)=receiver.ordered_read[c]}
    IN receiver.sequenced_watermark[c] = IF now={} THEN 0 ELSE 1+Max({Sequence(m) : m \in now})
TransportAckClearsResend == connected => resend=Values(numbers) \ acked
AckedImpliesTransportArrival == \A m \in acked : \E pid \in opened : PacketMessage(pid)=m
EveryReliableAckedAndDelivered == <> (~connected \/ (Len(queue)=MaxMessages /\
    \A m \in DOMAIN queue : Reliable(Code(m)) =>
      m \in acked /\ (Code(m) \in {2,3} => m \in Values(receiver.trace))))
=============================================================================
