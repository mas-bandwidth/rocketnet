----------------------------- MODULE RNReceipts -----------------------------
EXTENDS Naturals, Sequences, FiniteSets

\* Standalone receipt layer over the transport's packet event interface.
\* WritePacket(n,pid) commits one opaque record/fragment to a transport packet.
\* OpenPacket(pid) means the peer transport opened it; ReceiveAck(pid) is the
\* transport packet-acked input, not a message ACK. PacketLost(pid) is the
\* transport lost input. A late packet ACK may follow a lost notification.
\* The environment never invents an ACK for an unopened packet. It may delay,
\* reorder, duplicate (stutter), lose, or acknowledge packets after map eviction.
\* No second ACK channel or application-delivery claim exists here. This is
\* Link's lossy event boundary, abstracted rather than coupled to RNChannel.
\* Names id, nums, unrel, smap, WritePacket, OpenPacket and ReceiveAck align
\* with the RNChannel draft. Packet IDs reuse only after transport/map release;
\* uint64 wrap and stale events across connection generations are outside this model.
\*
\* The bounded API workload consumes distinct serials: a nonreceipt type,
\* then two single-fragment receipt messages or one two-fragment block.
\* Each workload exercises the selected source type and size. An unreliable
\* send too big for one packet is promoted to its reliable twin
\* (the promotion rule): 0 to 2, 1 to 4 and 5 to 6. Types 6 and 7 never emit LOSS.
\* Fragments have independent numbers but share their message's serial.
\* Outcomes are ghost emission history; ring is the bounded application queue.
\* Dropping its oldest event is counted and never permits a second emission.
CONSTANTS MaxPid, MapCapacity, ReceiptCapacity, RTO, ReliableTimeout,
          BrokenLastIndexAck, BrokenTwoOutcomes, BrokenReliableLoss

Serials == 1..3
ReceiptSerials == 2..3
Pids == 0..MaxPid
Nums == ReceiptSerials \X (0..1)
Loads == {{n} : n \in Nums}
Events == [serial : ReceiptSerials, status : {"ACKED", "LOSS"}]

VARIABLES receiptType, fragmentCount, nextSerial, live,
          sentNums, acked, smap, active, packetNums, opened, lost,
          openedNums, ackProof, age, evicted, outcomes, ring,
          ringDropped, writes
vars == <<receiptType, fragmentCount, nextSerial, live,
          sentNums, acked, smap, active, packetNums, opened, lost,
          openedNums, ackProof, age, evicted, outcomes, ring,
          ringDropped, writes>>

SendLimit == IF fragmentCount = 1 THEN 3 ELSE 2
PromotionSources == {0,1,5}
Twin(k) == CASE k = 0 -> 2 [] k = 1 -> 4 [] k = 5 -> 6 [] OTHER -> k
Large == fragmentCount = 2
Kind(s) == IF s = 1 THEN 0 ELSE receiptType
EffectiveKind(s) == IF s \in ReceiptSerials /\ Kind(s) \in PromotionSources /\ Large
                      THEN Twin(Kind(s)) ELSE Kind(s)
Parts(s) == {<<s,i>> : i \in 0..(fragmentCount-1)}
Issued == 1..(nextSerial-1)
Unacked == (UNION {Parts(s) : s \in Issued \cap ReceiptSerials}) \ acked
InMap == {smap[i].id : i \in DOMAIN smap}
FreePids == Pids \ (active \cup InMap)
Complete(s) == Parts(s) \subseteq acked
Has(s,status) == \E i \in DOMAIN outcomes[s] : outcomes[s][i] = status
AddRing(s,status) == LET q == Append(ring, [serial |-> s, status |-> status])
                    IN IF Len(q) > ReceiptCapacity THEN Tail(q) ELSE q
RemoveMap(pid) == SelectSeq(smap, LAMBDA e : e.id # pid)
UnreliableIn(ns) == {s \in ReceiptSerials : EffectiveKind(s) = 5 /\
                                            Parts(s) \cap ns # {}}

Init ==
 /\ receiptType \in {5,6,7}
 /\ fragmentCount \in 1..2
 /\ nextSerial = 1 /\ live = TRUE
 /\ sentNums = {} /\ acked = {} /\ smap = <<>>
 /\ active = {} /\ packetNums = [p \in Pids |-> {}]
 /\ opened = {} /\ lost = {} /\ openedNums = {} /\ ackProof = {}
 /\ age = [n \in Nums |-> 0] /\ evicted = {}
 /\ outcomes = [s \in Serials |-> <<>>] /\ ring = <<>> /\ ringDropped = 0
 /\ writes = [s \in ReceiptSerials |-> 0]

\* The API reserves/returns the serial before any packet is written, including
\* all five nonreceipt types (their t values coalesce because subsequent receipt
\* transitions do not inspect a nonreceipt type). Payload is an opaque region record; no copy is modeled.
Send(t) ==
 /\ live /\ nextSerial <= SendLimit
 /\ IF nextSerial = 1 THEN t \in 0..4 ELSE t = receiptType
 /\ nextSerial' = nextSerial + 1
 /\ UNCHANGED <<receiptType, fragmentCount, live, sentNums, acked,
      smap, active, packetNums, opened, lost, openedNums, ackProof, age,
      evicted, outcomes, ring, ringDropped, writes>>

WritePacket(n,pid) ==
 LET s == n[1] IN
 /\ live /\ s \in Issued /\ n \in Parts(s) /\ pid \in FreePids
 /\ outcomes[s] = <<>> /\ Len(smap) < MapCapacity
 /\ IF EffectiveKind(s) = 5 THEN n \notin sentNums ELSE n \in Unacked
 /\ sentNums' = sentNums \cup {n}
 /\ smap' = Append(smap, [id |-> pid, nums |-> {n}, unrel |-> EffectiveKind(s) = 5])
 /\ active' = active \cup {pid}
 /\ packetNums' = [packetNums EXCEPT ![pid] = {n}]
 /\ writes' = [writes EXCEPT ![s] = IF @ < 2 THEN @ + 1 ELSE @]
 /\ UNCHANGED <<receiptType, fragmentCount, nextSerial, live, acked,
      opened, lost, openedNums, ackProof, age, evicted, outcomes, ring, ringDropped>>

OpenPacket(pid) ==
 /\ live /\ pid \in active \ (opened \cup lost)
 /\ opened' = opened \cup {pid}
 /\ openedNums' = openedNums \cup packetNums[pid]
 /\ UNCHANGED <<receiptType, fragmentCount, nextSerial, live, sentNums,
      acked, smap, active, packetNums, lost, ackProof, age,
      evicted, outcomes, ring, ringDropped, writes>>

ReceiveAck(pid) ==
 LET ns == UNION {smap[i].nums : i \in {j \in DOMAIN smap : smap[j].id = pid}} IN
 /\ live /\ pid \in active \cap opened
 /\ acked' = acked \cup ns
 /\ ackProof' = ackProof \cup UnreliableIn(ns)
 /\ smap' = RemoveMap(pid)
 /\ packetNums' = [packetNums EXCEPT ![pid] = {}]
 /\ active' = active \ {pid} /\ opened' = opened \ {pid} /\ lost' = lost \ {pid}
 /\ UNCHANGED <<receiptType, fragmentCount, nextSerial, live, sentNums,
      openedNums, age, evicted, outcomes, ring, ringDropped, writes>>

\* Lost packets remain eligible for a late ACK until the transport retires them.
\* Reliable numbers stay unacked and eligible for a new packet after loss/eviction.
PacketLost(pid) ==
 /\ live /\ pid \in active \ lost
 /\ lost' = lost \cup {pid}
 /\ UNCHANGED <<receiptType, fragmentCount, nextSerial, live, sentNums,
      acked, smap, active, packetNums, opened, openedNums, ackProof, age,
      evicted, outcomes, ring, ringDropped, writes>>

RetireLostPacket(pid) ==
 /\ live /\ pid \in lost
 /\ packetNums' = [packetNums EXCEPT ![pid] = {}]
 /\ active' = active \ {pid} /\ opened' = opened \ {pid} /\ lost' = lost \ {pid}
 /\ UNCHANGED <<receiptType, fragmentCount, nextSerial, live, sentNums,
      acked, smap, openedNums, ackProof, age, evicted,
      outcomes, ring, ringDropped, writes>>

EvictMap ==
 /\ live /\ Len(smap) > 0
 /\ evicted' = evicted \cup UnreliableIn(Head(smap).nums)
 /\ smap' = Tail(smap)
 /\ UNCHANGED <<receiptType, fragmentCount, nextSerial, live, sentNums,
      acked, active, packetNums, opened, lost, openedNums, ackProof, age,
      outcomes, ring, ringDropped, writes>>

Tick ==
 /\ live
 /\ age' = [n \in Nums |-> IF n \in sentNums \ acked /\ age[n] < ReliableTimeout
                                      THEN age[n]+1 ELSE age[n]]
 /\ UNCHANGED <<receiptType, fragmentCount, nextSerial, live, sentNums,
      acked, smap, active, packetNums, opened, lost, openedNums, ackProof,
      evicted, outcomes, ring, ringDropped, writes>>

EmitAck(s) ==
 /\ live /\ s \in Issued /\ Parts(s) \cap sentNums # {}
 /\ ~Has(s,"ACKED") /\ (outcomes[s] = <<>> \/ BrokenTwoOutcomes)
 /\ IF EffectiveKind(s) = 5 THEN s \in ackProof
       ELSE IF BrokenLastIndexAck THEN <<s,fragmentCount-1>> \in acked ELSE Complete(s)
 /\ outcomes' = [outcomes EXCEPT ![s] = Append(@,"ACKED")]
 /\ ring' = AddRing(s,"ACKED")
 /\ ringDropped' = ringDropped + IF Len(ring) = ReceiptCapacity THEN 1 ELSE 0
 /\ UNCHANGED <<receiptType, fragmentCount, nextSerial, live, sentNums,
      acked, smap, active, packetNums, opened, lost, openedNums, ackProof,
      age, evicted, writes>>

\* Per-update deadline sweep, one eligible descriptor at a time. Its tombstone
\* (outcomes) suppresses a late ACK even while the map's reliable references live.
SweepLoss(s) ==
 /\ live /\ s \in Issued /\ outcomes[s] = <<>> /\ ~Complete(s)
 /\ Parts(s) \cap sentNums # {}
 /\ (EffectiveKind(s) = 5 \/ BrokenReliableLoss)
 /\ (age[<<s,0>>] >= RTO \/ s \in evicted)
 /\ outcomes' = [outcomes EXCEPT ![s] = <<"LOSS">>]
 /\ ring' = AddRing(s,"LOSS")
 /\ ringDropped' = ringDropped + IF Len(ring) = ReceiptCapacity THEN 1 ELSE 0
 /\ UNCHANGED <<receiptType, fragmentCount, nextSerial, live, sentNums,
      acked, smap, active, packetNums, opened, lost, openedNums, ackProof,
      age, evicted, writes>>

TakeReceipt ==
 /\ Len(ring) > 0 /\ ring' = Tail(ring)
 /\ UNCHANGED <<receiptType, fragmentCount, nextSerial, live, sentNums,
      acked, smap, active, packetNums, opened, lost, openedNums, ackProof,
      age, evicted, outcomes, ringDropped, writes>>

\* CHANNELS 6.4: a reliable number reaching first-send + reliable_timeout
\* ends the connection even if other packets/traffic still make progress.
Disconnect ==
 /\ live
 /\ \E s \in Issued \cap ReceiptSerials :
      EffectiveKind(s) \in {6,7} /\
      (\E n \in Parts(s) \cap (sentNums \ acked) : age[n] = ReliableTimeout)
 /\ live' = FALSE
 /\ UNCHANGED <<receiptType, fragmentCount, nextSerial, sentNums,
      acked, smap, active, packetNums, opened, lost, openedNums, ackProof,
      age, evicted, outcomes, ring, ringDropped, writes>>

\* A new send/retransmit can need map room even after transport loss retires
\* its packet ID. Only this actual pressure requires eventual eviction.
NeedSend == \E s \in Issued \cap ReceiptSerials :
 outcomes[s] = <<>> /\
 (IF EffectiveKind(s)=5 THEN Parts(s) \ sentNums # {} ELSE Parts(s) \ acked # {})
EvictForSpace == NeedSend /\ Len(smap)=MapCapacity /\ EvictMap
TransportStep(pid) == OpenPacket(pid) \/ ReceiveAck(pid) \/ PacketLost(pid) \/ RetireLostPacket(pid)

Next == (\E t \in 0..7 : Send(t)) \/ Tick \/ EvictMap \/ TakeReceipt \/ Disconnect
     \/ (\E n \in Nums, p \in Pids : WritePacket(n,p))
     \/ (\E p \in Pids : OpenPacket(p) \/ ReceiveAck(p) \/ PacketLost(p) \/ RetireLostPacket(p))
     \/ (\E s \in ReceiptSerials : EmitAck(s) \/ SweepLoss(s))

TypeOK ==
 /\ receiptType \in {5,6,7} /\ fragmentCount \in 1..2
 /\ nextSerial \in 1..4 /\ live \in BOOLEAN
 /\ sentNums \subseteq Nums /\ acked \subseteq sentNums
 /\ smap \in Seq([id:Pids, nums:Loads, unrel:BOOLEAN]) /\ Len(smap) <= MapCapacity
 /\ active \subseteq Pids /\ opened \subseteq active /\ lost \subseteq active
 /\ packetNums \in [Pids -> SUBSET Nums] /\ openedNums \subseteq Nums
 /\ ackProof \subseteq ReceiptSerials /\ evicted \subseteq ReceiptSerials
 /\ age \in [Nums -> 0..ReliableTimeout]
 /\ outcomes \in [Serials -> Seq({"ACKED","LOSS"})]
 /\ ring \in Seq(Events) /\ ringDropped \in 0..4
 /\ writes \in [ReceiptSerials -> 0..2]
ExactlyOneOutcomePerReceiptSerial == \A s \in Serials :
 Len(outcomes[s]) <= 1 /\ (outcomes[s] # <<>> => s \in Issued \cap ReceiptSerials)
LossOnlyForUnreliableReceipt == \A s \in ReceiptSerials : Has(s,"LOSS") => EffectiveKind(s) = 5
PromotedIsTwin == \A s \in ReceiptSerials :
    Kind(s) \in PromotionSources /\ Large => EffectiveKind(s) = Twin(Kind(s))
PromotedNeverLoses == \A s \in ReceiptSerials :
    Kind(s) \in PromotionSources /\ Large => ~Has(s,"LOSS")
PromotionTable == \A k \in PromotionSources : Twin(k) \in {2,4,6}
AckedMeansPacketOpened == /\ acked \subseteq openedNums
                         /\ \A s \in ReceiptSerials :
                              Has(s,"ACKED") /\ EffectiveKind(s)=5 => s \in ackProof
BlockAckedOnlyWhenAllFragmentsAcked == \A s \in ReceiptSerials :
 Has(s,"ACKED") /\ EffectiveKind(s) \in {2,4,6,7} => Complete(s)
ReceiptRingBounded == Len(ring) <= ReceiptCapacity
UnreliableNeverResent == \A s \in ReceiptSerials : EffectiveKind(s)=5 => writes[s] <= 1
ReceiptProgress == \A s \in ReceiptSerials : (s \in Issued) ~> (outcomes[s] # <<>> \/ ~live)

\* The transport eventually resolves each occupied slot by packet ACK or
\* loss/retirement. No fairness condition forces a lost packet to open, or
\* requires any successful ACK. This includes persistent packet loss and
\* is weaker than fair Link delivery; expiry/reliable timeout still resolves.
Fairness == /\ WF_vars(Tick) /\ WF_vars(Disconnect) /\ WF_vars(EvictForSpace)
            /\ \A n \in Nums, p \in Pids : SF_vars(WritePacket(n,p))
            /\ \A p \in Pids : WF_vars(TransportStep(p))
            /\ \A s \in ReceiptSerials : WF_vars(EmitAck(s)) /\ WF_vars(SweepLoss(s))
Spec == Init /\ [][Next]_vars /\ Fairness
=============================================================================
