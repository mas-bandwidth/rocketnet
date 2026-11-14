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
\* The "blocks" scenario sends every code-4 message as a block in two
\* fragments (ArriveFragment); a block is complete, and deliverable, only
\* once both have arrived. It checks the one in-turn rule (InTurnOneRule):
\* an in-turn sequenced message, block or record, is delivered as soon as it
\* is complete and newer than the watermark; a newer in-turn message
\* supersedes an incomplete in-turn block; nothing in turn is held as future
\* state. Broken = "BlockInTurnHeld" is the earlier code, which collected an
\* in-turn block in the one future-state slot and compared it there.
\* A send the send capacity cannot take is refused (Refuse): the connection
\* goes on (standard/MESSAGES.md m1.7). The refused start runs the code's
\* steps (sdk/rocketnet_endpoint.cpp rn_endpoint_start): the route reads;
\* an unreliable source (0, 1, 5) writes the transport's cap into
\* max_payload_bytes (maxPayload) before the room is known; the region's
\* fit refuses before any take (rn_channels_start, rn_region_send_take);
\* the connection's send_refused counts it (sendRefused, modulo CountMod as
\* the code's counter wraps). The ghost calls counts the application's
\* starts, so a refused start is a step even when it changes nothing else.
\* TransportCaps is the caps the transport may hand a start. Broken =
\* "SendKick" is v1.0's MESSAGE_MEMORY_EXHAUSTED end from the send path;
\* "CountersAtStart" assigns the channel's ordering and sequencing counters
\* at the start, before the room is known, and keeps them on a refusal;
\* "StaleStepPossible" is the v1.1 StepPossible, written when a refusal was
\* a stutter. No receive step ends the connection either: the code drops
\* and counts (HOLD_FULL, UNRELIABLE_DELIVERY_FULL), and BuffersBounded
\* checks that the holds stay inside their capacities.
\* The "refused" scenario runs the blocks plans and lets an arriving code-1
\* record be refused (OpenPacket's full): no free hold slot (Ru) ahead of
\* its epoch, or a record ahead of its epoch larger than one packet's payload
\* (HOLD_FULL), or the update's unreliable allowance (Du) spent in turn
\* (UNRELIABLE_DELIVERY_FULL). Pool and allowance are spent by other channels
\* and codes, so the environment chooses. A refused record is dropped and
\* counted and the channel is left as it was (standard/MESSAGES.md m6.3);
\* releasing a code-1 hold frees the slot the record then takes, so only a
\* code-4 hold or no hold meets a full pool. An arriving code-3 record may
\* also carry full: its delivery's drain finds Du spent and releases a held
\* code-1 record, counted (Drain's du).
\* SupersederKept: no record that released another is itself refused.
\* Broken = "ReleaseBeforeRefuse" is the earlier code, which released the
\* channel's code-4 hold or in-turn block and then refused the record.
CONSTANTS Scenario, Broken, SendCapacity, OrderedCapacity, HoldCapacity, TransportCaps
ASSUME TransportCaps \subseteq Nat \ {0} /\ TransportCaps # {}
CountMod == 3
UnreliableSources == {0,1,5}
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
      [] Scenario = "blocks" -> {Plan(<<4,3,1,4>>, <<1,1,1,1>>),
                                 Plan(<<4,1,3,4>>, <<1,1,1,1>>)}
      [] Scenario = "refused" -> {Plan(<<4,1,3,4>>, <<1,1,1,1>>),
                                  Plan(<<3,4,1,2>>, <<1,1,1,1>>)}
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
          referenceTrace, connected, reason, missed, maxPayload, sendRefused, calls
vars == <<plan, queue, ordered_write, sequenced_write, written, numbers,
          opened, lost, acked, resend, receiver, orderedInputs,
          referenceTrace, connected, reason, missed, maxPayload, sendRefused, calls>>
\* everything but max_payload_bytes and the refusal counters
Kept == <<plan, queue, ordered_write, sequenced_write, written, numbers,
          opened, lost, acked, resend, receiver, orderedInputs,
          referenceTrace, connected, reason, missed>>
Starts == <<maxPayload, sendRefused, calls>>

\* The first three sends of the promote plan are too big for one record, so
\* each unreliable one is promoted to its reliable twin (the promotion rule). The
\* promote scenario also runs the mixed plan with nothing large, so the main
\* configuration keeps checking it. Read off the plan, so it holds before the
\* send is queued (the start's route reads it) and after.
Large(m) == Scenario = "promote" /\ m \in {1,2,3} /\
            [i \in DOMAIN plan |-> plan[i].code] = PromotePlanCodes

\* The start's route (sdk/rocketnet_endpoint.cpp rn_endpoint_send_route): an
\* unreliable source over the record capacity leaves with out_type set to its
\* reliable twin, written as the code's nested conditional; every other start
\* keeps its source. Enqueue stores the result as the queued send's wire code,
\* so PromotedIsTwin and PromotedNeverUnreliable check the route's result
\* against the rule (Twin), not a definition. Broken = "NoPromotion": the
\* oversize unreliable send keeps its source code; Broken =
\* "PromoteSequencedToReliable": code 1 is routed to 2, losing its sequencing.
Route(k, large) ==
    IF large /\ k \in UnreliableSources /\ Broken # "NoPromotion"
    THEN IF k = 0 THEN 2
         ELSE IF k = 1 THEN (IF Broken = "PromoteSequencedToReliable" THEN 2 ELSE 4)
         ELSE 2
    ELSE k

\* Message ids stand for stable reliable numbers after first packet write.
\* numbers records their independent late assignment order; code 1 takes none.
\* A promoted message carries the wire code its route gave it.
Source(m) == queue[m].source
Code(m) == queue[m].code
Channel(m) == queue[m].channel
Order(m) == queue[m].ordering
Sequence(m) == queue[m].sequencing
Newer(a,b) == Order(a) > Order(b) \/ (Order(a) = Order(b) /\ Sequence(a) > Sequence(b))
\* A code-4 block: two fragments, each its own reliable number, arriving in
\* any order (sdk/rocketnet_channel_sequenced.cpp, rn_sequenced_block).
Block(m) == Scenario \in {"blocks", "refused"} /\ Code(m) = 4
Fragments == 1..2
\* A code-1 record the receiver may refuse for Ru or Du (the "refused" scenario)
Refusable(m) == Scenario = "refused" /\ Code(m) = 1
\* r.trace is the application's one receive queue, pumped by each receive: every
\* code's and every channel's deliveries in delivery order, never filtered
\* by channel (sdk/rocketnet_message_server.cpp, rn_message_object_next). The per-channel
\* ordered and sequenced traces below are its projections.
EmptyReceiver == [ordered_read |-> [c \in Channels |-> 0],
    sequenced_watermark |-> [c \in Channels |-> 0],
    heldOrdered |-> {}, heldSequenced |-> {}, seen |-> {}, trace |-> <<>>,
    inTurn |-> {}, arrived |-> {}, counted |-> {}, arrivedInTurn |-> {},
    released |-> {}, refused |-> {}, superseders |-> {}]
\* r.refused: code-1 records dropped for Ru or Du (HOLD_FULL,
\* UNRELIABLE_DELIVERY_FULL). r.superseders: records that released the
\* channel's hold or in-turn block as newer. Both are ghosts.
\* The receiver's count of sequenced records released without a delivery is
\* Cardinality(released); the set keeps the identity for the accounting check.
\* A released record is superseded by a newer one, or refused by the drain.
DroppedCount(r) == Cardinality(r.released)
OrderedTrace(r,c) == SelectSeq(r.trace, LAMBDA m : Code(m) = 3 /\ Channel(m) = c)
SequencedTrace(r,c) == SelectSeq(r.trace, LAMBDA m : Sequenced(Code(m)) /\ Channel(m) = c)
ReliableTrace(r) == SelectSeq(r.trace, LAMBDA m : Code(m) = 2)
\* r.arrived holds every block fragment that has reached the receiver;
\* r.counted those its collecting block counted since it was anchored (the
\* code's block_have); r.arrivedInTurn those that arrived in their epoch.
Complete(r,m) == ~Block(m) \/ \A f \in Fragments : <<m,f>> \in r.counted
\* The in-turn block still collecting fragments on channel c. The rule keeps
\* it apart from the future-state slot (r.inTurn); the earlier code kept it in
\* that slot, where it is the held state of the current epoch.
InTurnCollecting(r,c) ==
    IF Broken = "BlockInTurnHeld"
    THEN {n \in r.heldSequenced : Channel(n)=c /\ Order(n)=r.ordered_read[c]}
    ELSE {n \in r.inTurn : Channel(n)=c}
\* The superseded in-turn block is released and counted (SEQUENCED_SUPERSEDED).
ReleaseInTurn(r,cur) ==
    IF Broken = "BlockInTurnHeld" THEN [r EXCEPT !.heldSequenced = @ \ cur, !.released = @ \cup cur]
    ELSE [r EXCEPT !.inTurn = @ \ cur, !.released = @ \cup cur]
\* A stale block (its epoch closed, or in turn behind the watermark) is
\* released and counted SEQUENCED_STALE (rn_sequenced_block,
\* rn_channels_drain_sequenced). Broken = "StaleBlockSilent" drops it and
\* counts nothing.
StaleBlocks(r,ms) == IF Broken = "StaleBlockSilent" THEN r ELSE [r EXCEPT !.released = @ \cup ms]
\* The rule's precondition: m is in turn, newer than the watermark, and no
\* other in-turn block at least as new is collecting.
Deliverable(r,m) ==
    LET c == Channel(m) IN
    /\ Sequenced(Code(m)) /\ Order(m) = r.ordered_read[c]
    /\ Sequence(m) >= r.sequenced_watermark[c]
    /\ ~\E n \in InTurnCollecting(r,c) \ {m} : Sequence(n) >= Sequence(m)

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
\* du: the update's unreliable allowance (Du) is spent when the drain reaches
\* a held code-1 record (the "refused" scenario's environment choice on the
\* ordered arrival that opens the epoch): rn_channels_drain_sequenced's
\* rn_sequenced_room refuses, counts UNRELIABLE_DELIVERY_FULL and releases the
\* hold, so the record joins released (a counted drop). A held code-4 record
\* meeting a full delivery table exhausts the region instead; the table is
\* W + Du entries and reliable deliveries take at most W of them, so that is
\* an invariant break of the code, not modelled here.
RECURSIVE Drain(_,_,_,_)
\* Bind the receiver value once per recursive call. This singleton mapping is
\* extensionally the same function; it avoids repeated lazy evaluation of
\* nested record updates while draining several held records. No transition,
\* bound, arrival order, loss action or fairness assumption changes.
Drain(input,c,fuel,du) ==
    CHOOSE output \in {
        LET stale == {m \in r.heldSequenced : Channel(m)=c /\ Order(m)<r.ordered_read[c]}
            staleTurn == {m \in r.inTurn : Channel(m)=c /\ Order(m)<r.ordered_read[c]}
            s == StaleBlocks([r EXCEPT !.heldSequenced = @ \ stale, !.inTurn = @ \ staleTurn,
                  !.released = @ \cup stale], staleTurn)
            turn == {m \in s.heldSequenced : Channel(m)=c /\ Order(m)=s.ordered_read[c]}
            readyS == {m \in turn : Complete(s,m)}
            openS == turn \ readyS
            readyO == {m \in s.heldOrdered : Channel(m)=c /\ Order(m)=s.ordered_read[c]}
            \* DrainStale: the records rn_channels_deliver_sequenced refuses
            \* (the code's predicate: behind the epoch's watermark), released
            \* and counted SEQUENCED_STALE. Unreachable at this model's small
            \* indexes: the watermark is 0 when an epoch opens and a channel
            \* holds one record, so the set is empty; the code reaches the
            \* branch only at the 24-bit half range, which RNSequencingWrap's
            \* OpenEpoch (its refused branch) covers.
            refused == IF Broken = "SequencedBackwards" THEN {}
                       ELSE {m \in readyS : Sequence(m) < s.sequenced_watermark[c]}
            freshS == readyS \ refused
            duFull == {m \in freshS : du /\ Code(m) = 1}
        IN IF fuel=0 THEN s
           \* a held block still short of fragments when its epoch opens is
           \* in turn now: it leaves the future-state slot (the earlier code
           \* kept it there)
           ELSE IF openS # {} /\ Broken # "BlockInTurnHeld" THEN
             Drain(TLCEval([s EXCEPT !.heldSequenced = @ \ openS, !.inTurn = @ \cup openS]), c, fuel-1, du)
           ELSE IF duFull # {} THEN
             LET m == Min(duFull)
                 t == [s EXCEPT !.heldSequenced = @ \ {m}, !.released = @ \cup {m}]
             IN Drain(TLCEval(t), c, fuel-1, du)
           ELSE IF freshS # {} THEN
             LET m == Min(freshS)
                 t == [s EXCEPT !.heldSequenced = @ \ {m}]
             IN Drain(TLCEval(DeliverMessage(t,m,TRUE)), c, fuel-1, du)
           ELSE IF refused # {} THEN
             LET m == Min(refused)
                 t == [s EXCEPT !.heldSequenced = @ \ {m}, !.released = @ \cup {m}]
             IN Drain(TLCEval(t), c, fuel-1, du)
           ELSE IF readyO # {} THEN
             LET m == Min(readyO)
                 t == [s EXCEPT !.heldOrdered = @ \ {m}]
             IN Drain(TLCEval(DeliverMessage(t,m,TRUE)), c, fuel-1, du)
           ELSE s
      : r \in {input} } : TRUE

\* m released something as newer (recorded in the "refused" scenario only, so
\* every other scenario keeps its state space)
Supersede(r,m,gone) ==
    IF Scenario = "refused" /\ gone # {} THEN [r EXCEPT !.superseders = @ \cup {m}] ELSE r
Refuse1(r,m) == [r EXCEPT !.refused = @ \cup {m}]

\* full, for a Refusable (code-1) record: the environment's Ru pool or Du
\* allowance has no room for m, or (ahead of its turn) m is larger than one
\* packet's payload and cannot fit the fixed future-state slot. The fixed code
\* refuses before it releases anything; Broken = "ReleaseBeforeRefuse"
\* releases first. full, for a code-3 record (the "refused" scenario): the
\* allowance is spent when its delivery's drain reaches a held code-1 record
\* (Drain's du). No refusal ends the connection: each is a counted drop.
Dispatch(r,m,full) ==
    IF Reliable(Code(m)) /\ m \in r.seen /\ Broken # "ReliableTwice" THEN r
    ELSE LET s == IF Reliable(Code(m)) THEN [r EXCEPT !.seen = @ \cup {m}] ELSE r
         IN IF Code(m)=2 THEN DeliverMessage(s,m,FALSE)
             ELSE IF Order(m)<s.ordered_read[Channel(m)] THEN
               [s EXCEPT !.released = @ \cup {m}]
             ELSE IF Order(m)=s.ordered_read[Channel(m)] THEN
               IF Sequenced(Code(m)) /\ Broken # "SequencedBackwards" /\
                 Sequence(m) < s.sequenced_watermark[Channel(m)] THEN
                \* in turn but behind the epoch's watermark: counted stale
                \* (rn_sequenced_in_turn, SEQUENCED_STALE). Broken =
                \* "InTurnStaleSilent" drops the record and counts nothing.
                IF Broken = "InTurnStaleSilent" THEN s
                ELSE [s EXCEPT !.released = @ \cup {m}]
               ELSE IF Sequenced(Code(m)) THEN
                \* one in-turn rule: an in-turn block at least as new
                \* supersedes this record; an older one is released
                LET cur == InTurnCollecting(s,Channel(m))
                IN IF \E n \in cur : Sequence(n) >= Sequence(m)
                   THEN [s EXCEPT !.released = @ \cup {m}]
                   \* Du spent: dropped (UNRELIABLE_DELIVERY_FULL), the block kept
                   ELSE IF full /\ Broken = "ReleaseBeforeRefuse"
                   THEN Refuse1(Supersede(ReleaseInTurn(s,cur),m,cur),m)
                   ELSE IF full THEN Refuse1(s,m)
                   ELSE Drain(TLCEval(DeliverMessage(Supersede(ReleaseInTurn(s,cur),m,cur),m,FALSE)), Channel(m), MaxMessages+1, FALSE)
               ELSE Drain(TLCEval(DeliverMessage(s,m,FALSE)), Channel(m), MaxMessages+1, full)
             ELSE IF Code(m)=3 THEN [s EXCEPT !.heldOrdered = @ \cup {m}]
             ELSE LET held == {n \in s.heldSequenced : Channel(n)=Channel(m)}
                      newest == held = {} \/ (\A n \in held : Newer(m,n))
                      replaced == Supersede([s EXCEPT !.heldSequenced = (@ \ held) \cup {m},
                                                      !.released = @ \cup held], m, held)
                 IN IF Broken="HoldAllSequenced" THEN [s EXCEPT !.heldSequenced = @ \cup {m}]
                    \* oversize, or Ru full where no slot would be freed
                    \* (releasing a code-1 hold frees the slot m then takes;
                    \* each plan has one code-1 message, so a refused record
                    \* never meets a code-1 hold): dropped (HOLD_FULL), the
                    \* hold kept
                    ELSE IF full /\ newest /\ Broken = "ReleaseBeforeRefuse"
                    THEN Refuse1([replaced EXCEPT !.heldSequenced = @ \ {m}], m)
                    ELSE IF full THEN Refuse1(s,m)
                    ELSE IF ~newest THEN [s EXCEPT !.released = @ \cup {m}]
                    ELSE replaced

\* One block fragment, r.arrived already counting it. In turn, the rule: the
\* block is checked against the watermark and the in-turn block, never
\* against the future-state slot, and delivered once complete. Ahead of its
\* turn (and, for the earlier code, in turn too) it is the one held state.
DispatchFragment(r0,m,f) ==
    LET c == Channel(m)
        anchor == [r0 EXCEPT !.counted = (@ \ ({m} \X Fragments)) \cup {<<m,f>>}]
        more == [r0 EXCEPT !.counted = @ \cup {<<m,f>>}]
    IN IF Order(m) < r0.ordered_read[c] THEN StaleBlocks(r0, {m})
       ELSE IF Order(m) = r0.ordered_read[c] /\ Sequence(m) < r0.sequenced_watermark[c] THEN
         StaleBlocks([r0 EXCEPT !.inTurn = @ \ {m}], {m})
       ELSE IF Order(m) = r0.ordered_read[c] /\ Broken # "BlockInTurnHeld" THEN
         LET cur == {n \in r0.inTurn : Channel(n)=c}
         IN IF m \in cur THEN
              IF Complete(more,m) THEN Drain(TLCEval(DeliverMessage([more EXCEPT !.inTurn = @ \ {m}],m,FALSE)), c, MaxMessages+1, FALSE)
              ELSE more
            \* superseded by the in-turn block (SEQUENCED_SUPERSEDED)
            ELSE IF \E n \in cur : Sequence(n) >= Sequence(m) THEN [r0 EXCEPT !.released = @ \cup {m}]
            ELSE LET t == [anchor EXCEPT !.inTurn = @ \ cur, !.released = @ \cup cur]
                 IN IF Complete(t,m) THEN Drain(TLCEval(DeliverMessage(t,m,FALSE)), c, MaxMessages+1, FALSE)
                    ELSE [t EXCEPT !.inTurn = @ \cup {m}]
       ELSE LET held == {n \in r0.heldSequenced : Channel(n)=c}
                t == IF m \in held THEN more
                     ELSE IF \A n \in held : Newer(m,n) THEN [anchor EXCEPT !.heldSequenced = (@ \ held) \cup {m},
                                                                         !.released = @ \cup held]
                     ELSE [r0 EXCEPT !.released = @ \cup {m}]
            IN IF m \in t.heldSequenced /\ Complete(t,m) /\ Order(m) = t.ordered_read[c] THEN
                 Drain(TLCEval(DeliverMessage([t EXCEPT !.heldSequenced = @ \ {m}],m,FALSE)), c, MaxMessages+1, FALSE)
               ELSE t

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
    /\ connected=TRUE /\ reason="NONE" /\ missed={}
    /\ maxPayload=0 /\ sendRefused=0 /\ calls=0

\* The channel's ordering and sequencing counters after a send of code k on
\* channel c joins its queue (rn_channels_finish)
NextOrdered(k,c) == IF k=3 THEN [ordered_write EXCEPT ![c]=@+1] ELSE ordered_write
NextSequenced(k,c) == IF k=3 THEN [sequenced_write EXCEPT ![c]=0]
                      ELSE IF Sequenced(k) THEN [sequenced_write EXCEPT ![c]=@+1]
                      ELSE sequenced_write

\* rn_endpoint_start and rn_endpoint_finish with room, under the cap the
\* transport hands the start (an unreliable source keeps it in
\* max_payload_bytes for its finish)
Enqueue(cap) ==
    /\ sendRefused'=sendRefused /\ calls'=(calls+1) % CountMod
    /\ connected /\ Len(queue)<MaxMessages /\ Len(queue)<SendCapacity
    /\ LET m == Len(queue)+1 c == plan[m].channel k == plan[m].code w == Route(k, Large(m))
       IN /\ queue'=Append(queue,[source |-> k, code |-> w, channel |-> c,
                  ordering |-> ordered_write[c], sequencing |-> sequenced_write[c]])
          /\ ordered_write'=NextOrdered(w,c)
          /\ sequenced_write'=NextSequenced(w,c)
          /\ maxPayload'=IF k \in UnreliableSources THEN cap ELSE maxPayload
    /\ UNCHANGED <<connected,reason,missed>>
    /\ UNCHANGED <<plan,written,numbers,opened,lost,acked,resend,
                    receiver,orderedInputs,referenceTrace>>

CanWrite(m,attempt) ==
    /\ m \in DOMAIN queue /\ attempt \in 1..2
    /\ Packet(m,attempt) \notin written
    /\ ~Block(m)
    /\ IF attempt=1 THEN \A n \in 1..(m-1) : Block(n) \/ Packet(n,1) \in written
       ELSE Reliable(Code(m)) /\ Packet(m,1) \in written /\ m \notin acked

WritePacket(m,attempt) ==
    /\ UNCHANGED Starts
    /\ connected /\ CanWrite(m,attempt)
    /\ written'=written \cup {Packet(m,attempt)}
    /\ numbers'=IF Reliable(Code(m)) /\ m \notin Values(numbers) THEN Append(numbers,m) ELSE numbers
    /\ resend'=IF Reliable(Code(m)) THEN resend \cup {m} ELSE resend
    /\ UNCHANGED <<plan,queue,ordered_write,sequenced_write,opened,lost,
                    acked,receiver,orderedInputs,referenceTrace,connected,reason,missed>>

OpenPacket(pid,full) ==
    /\ UNCHANGED Starts
    /\ connected /\ pid \in written \ (opened \cup lost)
    /\ full => Refusable(PacketMessage(pid)) \/
               (Scenario = "refused" /\ Code(PacketMessage(pid)) = 3)
    /\ opened'=opened \cup {pid}
    \* No receive step ends the connection: the code never ends it on a full
    \* hold. Every refusal is a counted drop (Dispatch's full, Drain's du);
    \* BuffersBounded checks that the holds stay inside their capacities.
    /\ LET m == PacketMessage(pid)
           r == TLCEval(Dispatch(receiver,m,full))
       IN /\ receiver'=r
          /\ orderedInputs'=IF Code(m)=3 THEN orderedInputs \cup {m} ELSE orderedInputs
          /\ missed'=IF ~(Reliable(Code(m)) /\ m \in receiver.seen) /\
                         Deliverable(receiver,m) /\ m \notin Values(r.trace) /\ ~full
                      THEN missed \cup {m} ELSE missed
    /\ referenceTrace'=[c \in Channels |-> OrderedReference(orderedInputs',c,0)]
    /\ UNCHANGED <<plan,queue,ordered_write,sequenced_write,written,numbers,
                    lost,acked,resend,connected,reason>>

ReceiveAck(q) ==
    /\ UNCHANGED Starts
    /\ connected /\ q # {} /\ q \subseteq opened
    /\ \E pid \in q : Reliable(Code(PacketMessage(pid))) /\ PacketMessage(pid) \notin acked
    /\ LET delivered == {PacketMessage(pid) : pid \in q}
           reliable == {m \in delivered : Reliable(Code(m))}
       IN /\ acked'=acked \cup reliable /\ resend'=resend \ reliable
    /\ UNCHANGED <<plan,queue,ordered_write,sequenced_write,written,numbers,
                    opened,lost,receiver,orderedInputs,referenceTrace,connected,reason,missed>>
LosePacket(pid) ==
    /\ UNCHANGED Starts
    /\ connected /\ pid \in written \ (opened \cup lost) /\ pid % 2 = 1
    /\ lost'=lost \cup {pid}
    /\ UNCHANGED <<plan,queue,ordered_write,sequenced_write,written,numbers,
                    opened,acked,resend,receiver,orderedInputs,referenceTrace,connected,reason,missed>>

\* The application offers the next send while the send capacity is full
\* (standard/MESSAGES.md m1.7), its steps in the code's order: the route
\* reads; an unreliable source writes the transport's cap into
\* max_payload_bytes (sdk/rocketnet_endpoint.cpp, before the route's
\* OVER_MAX refusal and the region's); rn_channels_start (or the unreliable
\* path) asks the region, whose fit refuses before any take; the start
\* returns NULL and send_refused counts it. Nothing else of the connection
\* changes. Broken = "SendKick": v1.0's end of the connection with
\* MESSAGE_MEMORY_EXHAUSTED from the send path. Broken = "CountersAtStart":
\* the channel's ordering and sequencing counters taken at the start, before
\* the room is known, and kept on the refusal.
Refuse(cap) ==
    /\ connected /\ Len(queue)=SendCapacity /\ Len(queue)<MaxMessages
    /\ LET m == Len(queue)+1 c == plan[m].channel k == plan[m].code
       IN /\ maxPayload'=IF k \in UnreliableSources THEN cap ELSE maxPayload
          /\ IF Broken = "CountersAtStart"
             THEN ordered_write'=NextOrdered(k,c) /\ sequenced_write'=NextSequenced(k,c)
             ELSE UNCHANGED <<ordered_write,sequenced_write>>
    /\ sendRefused'=(sendRefused+1) % CountMod /\ calls'=(calls+1) % CountMod
    /\ IF Broken = "SendKick"
       THEN /\ connected'=FALSE /\ reason'="MESSAGE_MEMORY_EXHAUSTED"
            /\ resend'={} /\ receiver'=[receiver EXCEPT !.heldOrdered={}, !.heldSequenced={}, !.inTurn={}]
       ELSE UNCHANGED <<connected,reason,resend,receiver>>
    /\ UNCHANGED <<plan,queue,written,numbers,
                    opened,lost,acked,orderedInputs,referenceTrace,missed>>

\* A block fragment reaches the receiver. Fragments are reliable numbers whose
\* transport (WritePacket, ReceiveAck) is RNBlocks' and SessionAcks'
\* concern; here only their arrival order matters. missed records a block
\* whose fragments all arrived in its turn, that was Deliverable when the last
\* one came, and that was not delivered.
ArriveFragment(m,f) ==
    /\ UNCHANGED Starts
    /\ connected /\ m \in DOMAIN queue /\ Block(m)
    /\ <<m,f>> \notin receiver.arrived
    /\ LET a == [receiver EXCEPT !.arrived = @ \cup {<<m,f>>},
                    !.arrivedInTurn = IF Order(m) = receiver.ordered_read[Channel(m)]
                                      THEN @ \cup {<<m,f>>} ELSE @]
           r == TLCEval(DispatchFragment(a,m,f))
       IN /\ receiver'=r
          /\ missed'=IF ({m} \X Fragments) \subseteq a.arrivedInTurn /\
                         Deliverable(receiver,m) /\ m \notin Values(r.trace)
                      THEN missed \cup {m} ELSE missed
    /\ UNCHANGED <<plan,queue,ordered_write,sequenced_write,written,numbers,
                    opened,lost,acked,resend,orderedInputs,referenceTrace,connected,reason>>

WriteAny == \E m \in Messages, attempt \in 1..2 : WritePacket(m,attempt)
OpenAny == \E pid \in Pids, full \in BOOLEAN : OpenPacket(pid,full)
AckAny == \E pid \in Pids : ReceiveAck({pid})
LoseAny == \E pid \in Pids : LosePacket(pid)
ArriveAny == \E m \in Messages, f \in Fragments : ArriveFragment(m,f)
StartAny == \E cap \in TransportCaps : Enqueue(cap) \/ Refuse(cap)
Next == StartAny \/ WriteAny \/ OpenAny \/ AckAny \/ LoseAny \/ ArriveAny
SafetySpec == Init /\ [][Next]_vars

\* ENABLED <<Next>>_vars written out as a state predicate: a step of Next
\* that changes vars exists. Each disjunct is one action's guard, and each of
\* those actions changes a variable whenever its guard holds (Enqueue grows
\* queue, Refuse moves the calls ghost and the refusal count, WritePacket
\* grows written, OpenPacket grows opened, ReceiveAck grows acked,
\* ArriveFragment grows receiver.arrived); LosePacket's extra steps add no
\* enabledness. StepPossibleIsEnabled checks the equality in the cheaper
\* configurations (TLC evaluates ENABLED by computing every successor again).
\* Broken = "StaleStepPossible" is the v1.1 predicate, whose refusal disjunct
\* held only under "SendKick" (the refusal was then a stutter otherwise).
StepPossible ==
    /\ connected
    /\ \/ Len(queue)<MaxMessages /\ Len(queue)<SendCapacity
       \/ Broken # "StaleStepPossible" /\ Len(queue)=SendCapacity /\ Len(queue)<MaxMessages
       \/ \E m \in Messages, attempt \in 1..2 : CanWrite(m,attempt)
       \/ written \ (opened \cup lost) # {}
       \/ \E pid \in opened : Reliable(Code(PacketMessage(pid))) /\ PacketMessage(pid) \notin acked
       \/ \E m \in DOMAIN queue, f \in Fragments : Block(m) /\ <<m,f>> \notin receiver.arrived

StepPossibleIsEnabled == StepPossible = ENABLED <<Next>>_vars

\* All enabled actions consume a finite id/cursor. Weak fairness of this
\* disjunction therefore drains every reliable send/arrival/packet ACK.
\* Spec is Init /\ [][Next]_vars /\ WF_vars(Next), with WF expanded by its
\* definition (WF_v(A) == []<>~ENABLED <<A>>_v \/ []<><<A>>_v) and two exact
\* rewrites TLC checks far faster: ENABLED <<Next>>_vars is StepPossible, and
\* in a behaviour of SafetySpec every step that changes vars is a Next step,
\* so <<Next>>_vars is <<TRUE>>_vars. Evaluating ENABLED on every state and
\* Next on every edge of the liveness graph cost the control about two
\* thirds of its time.
Spec == SafetySpec /\ ([]<>~StepPossible \/ []<><<TRUE>>_vars)

TypeOK ==
    /\ plan \in Plans /\ Len(queue)<=MaxMessages
    /\ queue \in Seq([source:Codes, code:Codes, channel:Channels, ordering:0..MaxMessages, sequencing:0..MaxMessages])
    /\ ordered_write \in [Channels -> 0..MaxMessages]
    /\ sequenced_write \in [Channels -> 0..MaxMessages]
    /\ receiver.ordered_read \in [Channels -> 0..MaxMessages]
    /\ receiver.sequenced_watermark \in [Channels -> 0..MaxMessages]
    /\ receiver.heldOrdered \subseteq DOMAIN queue
    /\ receiver.heldSequenced \subseteq DOMAIN queue
    /\ receiver.inTurn \subseteq {m \in DOMAIN queue : Block(m)}
    /\ receiver.arrived \subseteq {m \in DOMAIN queue : Block(m)} \X Fragments
    /\ receiver.counted \subseteq receiver.arrived
    /\ receiver.arrivedInTurn \subseteq receiver.arrived
    /\ receiver.released \subseteq DOMAIN queue
    /\ receiver.refused \subseteq {m \in DOMAIN queue : Refusable(m)}
    /\ receiver.superseders \subseteq DOMAIN queue
    /\ missed \subseteq DOMAIN queue
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
    /\ maxPayload \in {0} \cup TransportCaps
    /\ sendRefused \in 0..(CountMod-1) /\ calls \in 0..(CountMod-1)
BuffersBounded ==
    /\ Cardinality(receiver.heldOrdered)<=OrderedCapacity
    /\ Cardinality(receiver.heldSequenced)<=HoldCapacity
    /\ \A c \in Channels : Cardinality({m \in receiver.heldSequenced : Channel(m)=c})<=1
    /\ \A c \in Channels : Cardinality({m \in receiver.inTurn : Channel(m)=c})<=1
    /\ Len(numbers)<=SendCapacity /\ Cardinality(resend)<=SendCapacity
    /\ Cardinality(written)<=2*MaxMessages
SequencedNeverBackwards == \A c \in Channels :
    LET t == SequencedTrace(receiver,c) IN \A i,j \in DOMAIN t :
        i<j /\ Order(t[i])=Order(t[j]) => Sequence(t[i])<=Sequence(t[j])
NoSequencedDuplicate == \A c \in Channels :
    LET t == SequencedTrace(receiver,c) IN Len(t)=Cardinality(Values(t))
\* The route's wire code against the rule: reversed by Broken =
\* "PromoteSequencedToReliable" (and "NoPromotion").
PromotedIsTwin == \A m \in DOMAIN queue :
    Large(m) /\ Source(m) \in PromotionSources => Code(m) = Twin(Source(m))
\* An oversize unreliable send never leaves on an unreliable wire code, so its
\* delivery is the reliable twin's (ReliableExactlyOnce, and the liveness
\* EveryReliableAckedAndDelivered for codes 2 and 3). Reversed by Broken =
\* "NoPromotion".
PromotedNeverUnreliable == \A m \in DOMAIN queue :
    Large(m) /\ Source(m) \in PromotionSources => Reliable(Code(m))
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
\* The one in-turn rule. Nothing in turn sits in the future-state slot, and
\* every in-turn message, block or record, that was Deliverable when it
\* became complete was delivered then.
InTurnNeverHeldAsFuture == \A m \in receiver.heldSequenced :
    Order(m) > receiver.ordered_read[Channel(m)]
InTurnDeliveredWhenComplete == missed = {}
InTurnOneRule == InTurnNeverHeldAsFuture /\ InTurnDeliveredWhenComplete
\* (N) No send ends the connection: no start step, accepted or refused,
\* leaves the connection ended (standard/MESSAGES.md m1.7), read on the
\* state the step leaves. Reversed by Broken = "SendKick".
SendNeverEnds == [][StartAny => connected']_vars

\* The next send of the plan, which a start offers
NextCode == plan[Len(queue)+1].code

\* A start the send capacity has no room for (by the state before it)
\* leaves the connection exactly as it was (standard/MESSAGES.md m1.7, rule
\* 2), whatever it did on the way: the refusal count moves by one (design
\* (L)1), and max_payload_bytes may change, only for an unreliable source, to
\* the cap the transport handed the start. That write is dead while no start
\* is pending (the route refuses a start while one is), and every read of
\* max_payload_bytes is rn_endpoint_finish of a pending start whose own
\* start wrote it (an unreliable source; a reliable one ignores it) or
\* rn_endpoint_send (tests only) handing it back to the start as its cap.
\* Reversed by Broken = "CountersAtStart" and "SendKick".
RefusalLeavesStateUnchanged ==
    [][StartAny /\ Len(queue)>=SendCapacity /\ Len(queue)<MaxMessages =>
          /\ UNCHANGED Kept
          /\ sendRefused'=(sendRefused+1) % CountMod
          /\ (maxPayload' # maxPayload => NextCode \in UnreliableSources)]_vars
\* m7.4 (nothing is silent): every sequenced record that arrived is delivered,
\* still held, superseded, refused, or released and counted; a code-4 block
\* counts once any fragment of it has arrived. Broken = "InTurnStaleSilent"
\* drops an in-turn stale record without counting it; Broken =
\* "StaleBlockSilent" drops a stale block without counting it.
SequencedArrivedAccounted == connected =>
    \A m \in {n \in {PacketMessage(pid) : pid \in opened} : Sequenced(Code(n)) /\ ~Block(n)}
             \cup {n \in DOMAIN queue : Block(n) /\ \E f \in Fragments : <<n,f>> \in receiver.arrived} :
        m \in Values(receiver.trace) \/ m \in receiver.heldSequenced \/
        m \in receiver.inTurn \/ m \in receiver.released \/ m \in receiver.refused
\* standard/MESSAGES.md m6.3: a refused code-1 record leaves the channel
\* unchanged, so a record that released the channel's hold or in-turn block
\* (an accepted, ACK-eligible code-4 message among them) is never one refused.
\* Reversed by Broken = "ReleaseBeforeRefuse".
SupersederKept == receiver.superseders \cap receiver.refused = {}
\* The same rule on the channel's state itself: a step that refuses a record
\* changes no hold, in-turn block, release or delivery.
RefusalLeavesChannelUnchanged ==
    [][receiver'.refused # receiver.refused =>
          /\ receiver'.heldSequenced = receiver.heldSequenced
          /\ receiver'.inTurn = receiver.inTurn
          /\ receiver'.released = receiver.released
          /\ receiver'.trace = receiver.trace]_vars
TransportAckClearsResend == connected => resend=Values(numbers) \ acked
AckedImpliesTransportArrival == \A m \in acked : \E pid \in opened : PacketMessage(pid)=m
EveryReliableAckedAndDelivered == <> (~connected \/ (Len(queue)=MaxMessages /\
    \A m \in DOMAIN queue : Reliable(Code(m)) =>
      m \in acked /\ (Code(m) \in {2,3} => m \in Values(receiver.trace))))
=============================================================================
