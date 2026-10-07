--------------------------- MODULE Channels ---------------------------
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS N, Window, Traffic, LinkCapacity, TicketGenerations, Retry, Deduplicate, RespectOrder
Channels == 1..N
Messages == 1..Len(Traffic)
UNRELIABLE == 0
RELIABLE_ORDERED == 3
Reliabilities == 0..7
EnabledReliabilities == {UNRELIABLE, RELIABLE_ORDERED}
IsReliable(m) == Traffic[m].reliability = RELIABLE_ORDERED
Channel(m) == Traffic[m].channel
Ordering(m) == Cardinality({j \in 1..(m-1) :
                              IsReliable(j) /\ Channel(j) = Channel(m)})
\* Wire-compatible future types: no transition silently aliases a stub.
StubReliabilities == Reliabilities \ EnabledReliabilities
StubSend(code) == code \in StubReliabilities /\ FALSE
DescriptorType == [channel : Channels, reliability : Reliabilities,
                   ordering : 0..Len(Traffic), sequencing : 0..Len(Traffic),
                   receipt : BOOLEAN, block : BOOLEAN]
Descriptor(m) == [channel |-> Channel(m), reliability |-> Traffic[m].reliability,
                  ordering |-> Ordering(m), sequencing |-> 0,
                  receipt |-> FALSE, block |-> FALSE]
Packets == [message : Messages, generation : 0..(TicketGenerations-1)]

ASSUME /\ N > 0 /\ Window > 0 /\ LinkCapacity > 0
       /\ TicketGenerations \in {1, 2}
       /\ Traffic \in Seq([channel : Channels, reliability : EnabledReliabilities])
       /\ Len(Traffic) > 0
       /\ Retry \in BOOLEAN /\ Deduplicate \in BOOLEAN /\ RespectOrder \in BOOLEAN

VARIABLES accepted, queue, written, acked, packetMap, ackPending,
          generation, transportSeen, receiveBase, receivedBits, held, orderedWrite, orderedRead, sequencedWrite, sequencedWatermark,
          heldSequenced, delivered, dataWire, dataSent, dataPulse
vars == <<accepted, queue, written, acked, packetMap, ackPending,
          generation, transportSeen, receiveBase, receivedBits, held, orderedWrite, orderedRead, sequencedWrite, sequencedWatermark,
          heldSequenced, delivered, dataWire, dataSent, dataPulse>>
endpoint == <<accepted, queue, written, acked, packetMap, ackPending,
              generation, transportSeen, receiveBase, receivedBits, held, orderedWrite, orderedRead, sequencedWrite, sequencedWatermark,
              heldSequenced, delivered>>
D == INSTANCE Link WITH wire <- dataWire, sent <- dataSent, pulse <- dataPulse,
     Packets <- Packets, Capacity <- LinkCapacity, MaxDelay <- 0,
     Duplication <- TRUE, Reordering <- TRUE, Pattern <- "arbitrary",
     BurstStart <- 1, BurstLen <- 1

Current(m) == [message |-> m, generation |-> generation[m]]
OnWire(m) == \E i \in DOMAIN dataWire : dataWire[i].pkt.message = m
HasAck(m) == \E p \in ackPending : p.message = m
Mapped(m) == \E p \in packetMap : p.message = m
Outstanding == {m \in written : IsReliable(m) /\ m \notin acked}
Number(m) == Cardinality({j \in written : IsReliable(j) /\ j < m})
NextNumber == Cardinality({j \in written : IsReliable(j)})
Floor == IF Outstanding = {} THEN NextNumber
         ELSE CHOOSE n \in {Number(m) : m \in Outstanding} :
                    \A k \in {Number(m) : m \in Outstanding} : n <= k
Elems(s) == {s[i] : i \in DOMAIN s}

\* Each receive action is one complete endpoint update: drain to completion
\* before ACK processing can let the sender reuse a window slot (P6).
RECURSIVE Drain(_, _, _)
Drain(h, r, d) ==
    IF \E m \in h : Ordering(m) = r[Channel(m)]
    THEN LET m == CHOOSE x \in h : Ordering(x) = r[Channel(x)]
         IN Drain(h \ {m}, [r EXCEPT ![Channel(m)] = @ + 1], Append(d, m))
    ELSE [held |-> h, read |-> r, delivered |-> d]

\* The bounded connection-wide deduplication state also admits future
\* unordered reliable records, independently of channel dispatch.
RECURSIVE AdvanceReceive(_, _)
AdvanceReceive(b, bits) ==
    IF b \in bits THEN AdvanceReceive(b + 1, bits \ {b})
    ELSE [base |-> b, bits |-> bits]

Init ==
    /\ accepted = 0 /\ queue = {} /\ written = {} /\ acked = {}
    /\ packetMap = {} /\ ackPending = {}
    /\ generation = [m \in Messages |-> 0]
    /\ transportSeen = {} /\ receiveBase = 0 /\ receivedBits = {}
    /\ held = {} /\ orderedWrite = [c \in Channels |-> 0]
    /\ orderedRead = [c \in Channels |-> 0]
    /\ sequencedWrite = [c \in Channels |-> 0]
    /\ sequencedWatermark = [c \in Channels |-> 0]
    /\ heldSequenced = [c \in Channels |-> 0]
    /\ delivered = <<>> /\ D!Init

Admit ==
    /\ accepted < Len(Traffic) /\ Cardinality(queue) < Window
    /\ accepted' = accepted + 1 /\ queue' = queue \cup {accepted + 1}
    /\ orderedWrite' = IF IsReliable(accepted + 1)
         THEN [orderedWrite EXCEPT ![Channel(accepted + 1)] = @ + 1]
         ELSE orderedWrite
    /\ UNCHANGED <<written, acked, packetMap, ackPending, generation, transportSeen, receiveBase, receivedBits, held,
          orderedRead, sequencedWrite, sequencedWatermark, heldSequenced,
          delivered, dataWire, dataSent, dataPulse>>

Send(m) ==
    /\ m \in queue
    /\ ~Mapped(m) /\ ~OnWire(m) /\ ~HasAck(m)
    /\ IF m \in written THEN Retry /\ IsReliable(m)
       ELSE /\ \A j \in queue : j < m => j \in written
            /\ IsReliable(m) => NextNumber < Floor + Window
    /\ D!Send(Current(m))
    /\ transportSeen' = transportSeen \ {Current(m)}
    /\ written' = written \cup {m}
    /\ packetMap' = IF IsReliable(m) THEN packetMap \cup {Current(m)} ELSE packetMap
    /\ queue' = IF IsReliable(m) THEN queue ELSE queue \ {m}
    /\ UNCHANGED <<accepted, acked, ackPending, generation, receiveBase, receivedBits, held, orderedWrite, orderedRead,
                    sequencedWrite, sequencedWatermark, heldSequenced, delivered>>

Receive(p) ==
    /\ \E i \in DOMAIN dataWire :
         /\ dataWire[i].pkt = p
         /\ dataWire' = D!Remove(dataWire, i)
    /\ dataPulse' = 1 - dataPulse
    /\ UNCHANGED dataSent
    /\ transportSeen' = IF IsReliable(p.message) THEN transportSeen
                         ELSE transportSeen \cup {p}
    /\ LET m == p.message
           seen == IF IsReliable(m)
                   THEN Number(m) < receiveBase \/ Number(m) \in receivedBits
                   ELSE p \in transportSeen
           result == Drain(held \cup {m}, orderedRead, delivered)
           rx == AdvanceReceive(receiveBase, receivedBits \cup {Number(m)})
       IN /\ IF IsReliable(m) /\ ~seen
             THEN /\ receiveBase' = rx.base /\ receivedBits' = rx.bits
             ELSE UNCHANGED <<receiveBase, receivedBits>>
          /\ ackPending' = IF IsReliable(m) THEN ackPending \cup {p} ELSE ackPending
          /\ IF Deduplicate /\ seen
             THEN UNCHANGED <<held, orderedRead, delivered>>
             ELSE IF ~IsReliable(m) \/ ~RespectOrder
                  THEN /\ delivered' = Append(delivered, m)
                       /\ UNCHANGED <<held, orderedRead>>
                  ELSE /\ held' = result.held /\ orderedRead' = result.read
                       /\ delivered' = result.delivered
    /\ UNCHANGED <<accepted, queue, written, acked, packetMap, generation, orderedWrite,
                    sequencedWrite, sequencedWatermark, heldSequenced>>

Ack(p) ==
    /\ p \in ackPending
    /\ ackPending' = ackPending \ {p}
    /\ acked' = IF p \in packetMap THEN acked \cup {p.message} ELSE acked
    /\ queue' = IF p \in packetMap THEN queue \ {p.message} ELSE queue
    /\ packetMap' = packetMap \ {p}
    /\ UNCHANGED <<accepted, written, generation, transportSeen, receiveBase, receivedBits, held, orderedWrite, orderedRead,
          sequencedWrite, sequencedWatermark, heldSequenced, delivered,
          dataWire, dataSent, dataPulse>>

LoseAck(p) ==
    /\ p \in ackPending /\ ackPending' = ackPending \ {p}
    /\ UNCHANGED <<accepted, queue, written, acked, packetMap, generation, transportSeen, receiveBase, receivedBits,
          held, orderedWrite, orderedRead, sequencedWrite, sequencedWatermark, heldSequenced,
          delivered, dataWire, dataSent, dataPulse>>

\* Expiry is after the old transport attempt has drained. Fresh concrete
\* packet IDs are alpha-renamed after retirement; no live ID is recycled.
\* One name is a quotient of the two-name check: no state except generation
\* retains the old name, and fairness/properties concern logical messages.
\* Overlapping retries/map eviction are outside this abstraction.
Expire(m) ==
    /\ Mapped(m) /\ ~OnWire(m) /\ ~HasAck(m)
    /\ packetMap' = {p \in packetMap : p.message # m}
    /\ generation' = [generation EXCEPT ![m] = (@ + 1) % TicketGenerations]
    /\ UNCHANGED <<accepted, queue, written, acked, ackPending, transportSeen, receiveBase, receivedBits, held, orderedWrite,
          orderedRead, sequencedWrite, sequencedWatermark, heldSequenced,
          delivered, dataWire, dataSent, dataPulse>>

Environment == D!Environment /\ UNCHANGED endpoint
Next == Admit \/ (\E m \in Messages : Send(m) \/ Expire(m))
        \/ (\E p \in Packets : Receive(p) \/ Ack(p) \/ LoseAck(p))
        \/ Environment
SafetySpec == Init /\ [][Next]_vars
ReceiveMessage(m) == \E p \in Packets : p.message = m /\ Receive(p)
AckMessage(m) == \E p \in Packets : p.message = m /\ Ack(p)
\* Strong fairness on receive-any-position is fair loss per message: a
\* message offered infinitely often is eventually received, even if every
\* particular attempt could be dropped. This does not promise delivery of
\* an unreliable message sent only once.
Fairness == /\ WF_vars(Admit)
            /\ \A m \in Messages :
                 /\ SF_vars(Send(m)) /\ WF_vars(Expire(m))
                 /\ SF_vars(ReceiveMessage(m)) /\ SF_vars(AckMessage(m))
Spec == SafetySpec /\ Fairness

TypeOK ==
    /\ accepted \in 0..Len(Traffic)
    /\ queue \subseteq 1..accepted /\ written \subseteq 1..accepted
    /\ acked \subseteq {m \in written : IsReliable(m)}
    /\ packetMap \subseteq Packets /\ ackPending \subseteq Packets
    /\ generation \in [Messages -> 0..(TicketGenerations-1)]
    /\ transportSeen \subseteq Packets
    /\ receiveBase \in 0..Len(Traffic)
    /\ receivedBits \subseteq 0..(Len(Traffic)-1)
    /\ \A m \in Messages : Descriptor(m) \in DescriptorType
    /\ held \subseteq {m \in written : IsReliable(m)}
    /\ orderedWrite \in [Channels -> 0..Len(Traffic)]
    /\ orderedRead \in [Channels -> 0..Len(Traffic)]
    /\ sequencedWrite = [c \in Channels |-> 0]
    /\ sequencedWatermark = [c \in Channels |-> 0]
    /\ heldSequenced = [c \in Channels |-> 0]
    /\ delivered \in Seq(Messages) /\ D!TypeOK
NoDuplicateDelivery == Len(delivered) = Cardinality(Elems(delivered))
OrderedDelivery ==
    \A i \in DOMAIN delivered : IsReliable(delivered[i]) =>
        \A m \in 1..(delivered[i]-1) :
          (IsReliable(m) /\ Channel(m) = Channel(delivered[i])) =>
             \E j \in 1..(i-1) : delivered[j] = m
Bounded == /\ Cardinality(queue) <= Window
           /\ Cardinality(Outstanding) <= Window
           /\ Cardinality(packetMap) <= Window
           /\ Cardinality(ackPending) <= 2 * Len(Traffic)
           /\ Cardinality(held) <= Window
           /\ Cardinality(receivedBits) <= Window
           /\ \A n \in receivedBits : receiveBase < n /\ n < receiveBase + Window
           /\ Cardinality(transportSeen) <= 2 * Len(Traffic)
           /\ D!InFlightBounded
NoSlotCollision == \A m, n \in held :
                     Number(m) % Window = Number(n) % Window => m = n
AckedMeansReceived == acked \subseteq (held \cup Elems(delivered))
MapSound == /\ \A p \in packetMap : p.message \in Outstanding
            /\ \A m \in Messages : Cardinality({p \in packetMap : p.message = m}) <= 1
DeliveredWereSent == Elems(delivered) \subseteq written
EveryReliableDelivered == \A m \in Messages :
    IsReliable(m) => (accepted >= m ~> m \in Elems(delivered))
EveryReliableAcked == \A m \in Messages :
    IsReliable(m) => (accepted >= m ~> m \in acked)
=============================================================================
