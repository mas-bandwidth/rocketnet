------------------------- MODULE FragmentsAndAcks -------------------------
(***************************************************************************)
(* rocketnet: fragment reassembly and the ack window, over the lossy link. *)
(*                                                                         *)
(* Two peers on one connection.  A sends session packets to B: single     *)
(* payloads (type 5, one packet id each) and fragmented payloads (type 9,  *)
(* count fragments under one payload id, one packet id in an aggregate).   *)
(* B opens them through its replay window, reassembles fragments into a   *)
(* slot, and delivers a complete payload into the per-update delivered     *)
(* batch.  B sends ack packets back to A carrying the block built from its *)
(* window; A marks its sent records acked and surfaces packet ids into the *)
(* acked batch the application drains at its next update.  Both directions *)
(* cross tla/Link.tla, so every transition here is driven by a packet that *)
(* may be dropped, duplicated, reordered or delayed.                       *)
(*                                                                         *)
(* The code this models, by function (sdk/, at the commit cited from):     *)
(*   rocketnet_reassembly_process_fragment      rocketnet_fragment.cpp:199 *)
(*   rocketnet_fragment_plan                     rocketnet_fragment.cpp:40  *)
(*   rocketnet_ack_endpoint_record_sent          rocketnet_ack.cpp:228      *)
(*   rocketnet_ack_endpoint_open_aggregate       rocketnet_ack.cpp:280      *)
(*   rocketnet_ack_apply_run                     rocketnet_ack.cpp:318      *)
(*   rocketnet_ack_endpoint_process_block        rocketnet_ack.cpp:427      *)
(*   rocketnet_ack_block_build                   rocketnet_ack.cpp:51       *)
(*   rocketnet_replay_already_received / advance rocketnet_replay.cpp:27,42 *)
(*   the session sequence and its burns          rocketnet_client_transport *)
(*     .cpp:760-877 (finish_arena_packet), USAGE.md "Sending"              *)
(*                                                                         *)
(* What is abstracted: bytes are sizes, never contents (a fragment carries *)
(* its byte count and the slot records where it was placed); the replay    *)
(* window is the set of opened sequences with the width W (the word        *)
(* representation and its F1 defect belong to the replay-window module);   *)
(* ack packets carry the block alone, no sequence of their own, so a       *)
(* duplicated block is applied twice (process_block is idempotent on an    *)
(* acked record, so this is the stricter adversary); the block is never    *)
(* truncated by packet room; A's receive budget per update is unbounded    *)
(* while B's is RecvPerUpdate (the drain bound the delivered batch is sized *)
(* from); rtt, stats, keep-alives and the ack debt are not here.           *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
    MaxSeq,          \* the last session sequence A may spend (finite horizon)
    SentRing,        \* A's sent-record ring (ROCKETNET_SENT_PACKET_BUFFER_SIZE)
    AggRing,         \* A's aggregate ring (ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE)
    BatchCap,        \* A's acked batch capacity per update (acked_capacity)
    W,               \* B's replay window width (ROCKETNET_REPLAY_WINDOW_BITS)
    Slots,           \* B's reassembly slots on this connection
    IdMod,           \* payload ids live in 1..IdMod-1, serial arithmetic (2^32)
    FirstPid,        \* the first payload id A uses (start near the wrap)
    PayloadSizes,    \* the fragmented payload sizes the honest sender uses
    MaxFragBytes,    \* bytes a fragment may carry (ROCKETNET_MAX_FRAGMENT_BYTES)
    MaxPayload,      \* B's configured max payload bytes (its arena per slot)
    RecvPerUpdate,   \* packets B's receive path drains per update
    DeliveredCap,    \* B's delivered batch capacity (delivered_capacity)
    Burns,           \* BOOLEAN: may A burn sequences (abort, seal failure)
    Forged,          \* fragment headers a defective sender may emit ({} = honest)
    \* the two links
    C2SPattern, S2CPattern, Capacity, MaxDelay, Duplication, Reordering,
    BurstStart, BurstLen

ASSUME /\ MaxSeq \in Nat \ {0}
       /\ SentRing \in Nat \ {0} /\ AggRing \in Nat \ {0} /\ BatchCap \in Nat \ {0}
       /\ W \in Nat \ {0} /\ Slots \in Nat \ {0}
       /\ IdMod \in Nat /\ IdMod >= 4 /\ FirstPid \in 1..IdMod-1
       /\ MaxFragBytes \in Nat \ {0} /\ MaxPayload \in Nat \ {0}
       /\ PayloadSizes \subseteq (MaxFragBytes+1)..MaxPayload
       /\ RecvPerUpdate \in Nat \ {0} /\ DeliveredCap \in Nat \ {0}
       /\ Burns \in BOOLEAN

(***************************************************************************)
(* The honest sender's plan: rocketnet_fragment_plan.  count is at least   *)
(* two, stride = ceil(n / count), the final fragment takes the remainder.  *)
(***************************************************************************)
Ceil(a, b) == (a + b - 1) \div b

PlanCount(n)  == LET c == Ceil(n, MaxFragBytes) IN IF c < 2 THEN 2 ELSE c
PlanStride(n) == Ceil(n, PlanCount(n))
PlanFinal(n)  == n - PlanStride(n) * (PlanCount(n) - 1)

MaxCount == LET counts == {PlanCount(n) : n \in PayloadSizes} \cup {f.cnt : f \in Forged}
            IN CHOOSE m \in counts : \A c \in counts : c <= m

Pids == 1..IdMod-1
MaxPacketId == MaxSeq          \* every packet id spends at least one sequence

(***************************************************************************)
(* The packets.                                                            *)
(***************************************************************************)
FragmentHeader == [pid : 0..IdMod-1, idx : 0..MaxCount-1, cnt : 1..MaxCount, bytes : 1..MaxFragBytes]

ASSUME Forged \subseteq FragmentHeader

C2SPackets ==
    [type : {"payload", "fragment"}, seq : 1..MaxSeq,
     pid : 0..(IF IdMod-1 > MaxPacketId THEN IdMod-1 ELSE MaxPacketId),
     idx : 0..MaxCount-1, cnt : 1..MaxCount, bytes : 1..MaxFragBytes]

\* an ack packet is the block: the set of sequences it names as received
S2CPackets == SUBSET (1..MaxSeq)

VARIABLES
    \* A, the sender
    nextSeq,        \* send_sequence_session: the next sequence to spend
    highestSent,    \* ack endpoint highest_sequence: highest recorded as sent
    sentRec,        \* the sent ring: slot -> [seq, acked, hasPid, pid, agg]
    aggs,           \* the aggregate ring: slot -> [id, pid, remaining]
    numAggs,        \* aggregates opened
    batch,          \* the acked batch (packet ids), drained by UpdateA
    nextPacketId,   \* the application's next packet id
    nextPid,        \* the next fragment payload id
    pending,        \* fragments sealed and waiting to go on the wire
    \* B, the receiver
    highest,        \* replay highest_opened
    opened,         \* every sequence B ever opened (the window is a view of it)
    slots,          \* reassembly slots: slot -> [pid, cnt, stride, finalBytes, finalStaged, received]
    numDelivered,   \* payloads completed into this update's delivered batch
    recvCount,      \* packets drained this update
    \* ghosts
    surfaced,       \* packet ids ever surfaced into A's batch
    surfacedTwice,  \* a packet id surfaced a second time
    sentPayloads,   \* packet id -> the sequences actually transmitted for it
    plans,          \* payload id -> [cnt, stride, final] the honest sender used
    burned,         \* sequences spent and never transmitted
    withdrawn,      \* packet ids whose fragmented send was withdrawn
    historyOverflow,\* an unacked record was overwritten (SEND_HISTORY_OVERFLOW)
    everDelivered,  \* payload ids B delivered
    deliveredLog,   \* [pid, cnt, stride, final] of every delivery
    deliveredTwice, \* a payload id delivered a second time
    completed,      \* payload ids whose slot filled
    batchFull,      \* a complete payload was dropped for a full delivered batch
    abandoned,      \* payload ids displaced from a slot while incomplete
    \* the links
    c2sWire, c2sSent, c2sPulse,
    s2cWire, s2cSent, s2cPulse

C2S == INSTANCE Link WITH wire <- c2sWire, sent <- c2sSent, pulse <- c2sPulse,
                          Packets <- C2SPackets, Pattern <- C2SPattern
S2C == INSTANCE Link WITH wire <- s2cWire, sent <- s2cSent, pulse <- s2cPulse,
                          Packets <- S2CPackets, Pattern <- S2CPattern

aVars     == <<nextSeq, highestSent, sentRec, aggs, numAggs, batch, nextPacketId, nextPid, pending>>
bVars     == <<highest, opened, slots, numDelivered, recvCount>>
ghostA    == <<surfaced, surfacedTwice, sentPayloads, plans, burned, withdrawn, historyOverflow>>
ghostB    == <<everDelivered, deliveredLog, deliveredTwice, completed, batchFull, abandoned>>
c2sVars   == <<c2sWire, c2sSent, c2sPulse>>
s2cVars   == <<s2cWire, s2cSent, s2cPulse>>
vars      == <<aVars, bVars, ghostA, ghostB, c2sVars, s2cVars>>

EmptyRec  == [seq |-> 0, acked |-> FALSE, hasPid |-> FALSE, pid |-> 0, agg |-> 0]
EmptyAgg  == [id |-> 0, pid |-> 0, remaining |-> 0]
EmptySlot == [pid |-> 0, cnt |-> 0, stride |-> 0, finalBytes |-> 0, finalStaged |-> FALSE, received |-> {}]

Init ==
    /\ nextSeq = 1 /\ highestSent = 0
    /\ sentRec = [i \in 0..SentRing-1 |-> EmptyRec]
    /\ aggs = [i \in 0..AggRing-1 |-> EmptyAgg]
    /\ numAggs = 0 /\ batch = <<>> /\ nextPacketId = 1 /\ nextPid = FirstPid /\ pending = <<>>
    /\ highest = 0 /\ opened = {} /\ slots = [i \in 0..Slots-1 |-> EmptySlot]
    /\ numDelivered = 0 /\ recvCount = 0
    /\ surfaced = {} /\ surfacedTwice = FALSE /\ sentPayloads = <<>> /\ plans = <<>>
    /\ burned = {} /\ withdrawn = {} /\ historyOverflow = FALSE
    /\ everDelivered = {} /\ deliveredLog = {} /\ deliveredTwice = FALSE
    /\ completed = {} /\ batchFull = FALSE /\ abandoned = {}
    /\ C2S!Init /\ S2C!Init

(***************************************************************************)
(* A's bookkeeping: rocketnet_ack_endpoint_record_sent.  Sequences only    *)
(* grow here (the atomic add), so the stale-sequence refusal (:238-244)    *)
(* never fires; the overwrite of an unacked record is the history          *)
(* overflow the caller counts and continues past.                          *)
(***************************************************************************)
RecordSent(rec, seq, hasPid, pid, agg) ==
    [rec EXCEPT ![seq % SentRing] = [seq |-> seq, acked |-> FALSE, hasPid |-> hasPid, pid |-> pid, agg |-> agg]]

OverwritesUnacked(rec, seq) ==
    LET old == rec[seq % SentRing] IN old.seq # 0 /\ ~old.acked

\* record every fragment of a block of sequences; seqs is a set, the order
\* does not matter because they land in distinct ring slots (count <= ring)
RECURSIVE RecordAll(_, _, _)
RecordAll(rec, seqs, agg) ==
    IF seqs = {} THEN rec
    ELSE LET s == CHOOSE x \in seqs : TRUE
         IN RecordAll(RecordSent(rec, s, FALSE, 0, agg), seqs \ {s}, agg)

AnyOverwrites(rec, seqs) == \E s \in seqs : OverwritesUnacked(rec, s)

NextPid(p) == IF p = IdMod - 1 THEN 1 ELSE p + 1

Fragment(seq, pid, idx, cnt, bytes) ==
    [type |-> "fragment", seq |-> seq, pid |-> pid, idx |-> idx, cnt |-> cnt, bytes |-> bytes]

PayloadPacket(seq, pid) ==
    [type |-> "payload", seq |-> seq, pid |-> pid, idx |-> 0, cnt |-> 1, bytes |-> 1]

(***************************************************************************)
(* A sends a single payload: start_packet spends the sequence, the seal    *)
(* records it with its packet id, the flush puts it on the wire.           *)
(***************************************************************************)
SendPayload ==
    /\ pending = <<>>
    /\ nextSeq <= MaxSeq
    /\ C2S!Send(PayloadPacket(nextSeq, nextPacketId))
    /\ sentRec' = RecordSent(sentRec, nextSeq, TRUE, nextPacketId, 0)
    /\ historyOverflow' = (historyOverflow \/ OverwritesUnacked(sentRec, nextSeq))
    /\ highestSent' = nextSeq
    /\ sentPayloads' = (nextPacketId :> {nextSeq}) @@ sentPayloads
    /\ nextSeq' = nextSeq + 1
    /\ nextPacketId' = nextPacketId + 1
    /\ UNCHANGED <<aggs, numAggs, batch, nextPid, pending, surfaced, surfacedTwice, plans, burned, withdrawn>>
    /\ UNCHANGED <<bVars, ghostB, s2cVars>>

(***************************************************************************)
(* A starts a fragmented payload of n bytes: fragment 0 rides the sequence *)
(* start_packet spent and start_fragments reserves count-1 more, so the    *)
(* payload occupies count consecutive sequences; one aggregate holds the   *)
(* packet id with remaining = count; every fragment is sealed and recorded *)
(* before any is flushed (finish_arena_packet's loop).  The fragments then *)
(* go on the wire one TransmitPending step at a time.                      *)
(***************************************************************************)
StartFragmented(n) ==
    LET cnt == PlanCount(n)
        stride == PlanStride(n)
        final == PlanFinal(n)
        seqs == nextSeq .. nextSeq + cnt - 1
        aggId == numAggs + 1
        frags == [i \in 1..cnt |-> Fragment(nextSeq + i - 1, nextPid, i - 1, cnt,
                                            IF i = cnt THEN final ELSE stride)]
    IN
    /\ pending = <<>>
    /\ nextSeq + cnt - 1 <= MaxSeq
    /\ numAggs' = aggId
    /\ aggs' = [aggs EXCEPT ![aggId % AggRing] = [id |-> aggId, pid |-> nextPacketId, remaining |-> cnt]]
    /\ sentRec' = RecordAll(sentRec, seqs, aggId)
    /\ historyOverflow' = (historyOverflow \/ AnyOverwrites(sentRec, seqs))
    /\ highestSent' = nextSeq + cnt - 1
    /\ pending' = frags
    /\ sentPayloads' = (nextPacketId :> seqs) @@ sentPayloads
    /\ plans' = (nextPid :> [cnt |-> cnt, stride |-> stride, final |-> final]) @@ plans
    /\ nextSeq' = nextSeq + cnt
    /\ nextPacketId' = nextPacketId + 1
    /\ nextPid' = NextPid(nextPid)
    /\ UNCHANGED <<batch, surfaced, surfacedTwice, burned, withdrawn>>
    /\ UNCHANGED <<bVars, ghostB, c2sVars, s2cVars>>

TransmitPending ==
    /\ pending # <<>>
    /\ C2S!Send(Head(pending))
    /\ pending' = Tail(pending)
    /\ UNCHANGED <<nextSeq, highestSent, sentRec, aggs, numAggs, batch, nextPacketId, nextPid>>
    /\ UNCHANGED <<bVars, ghostA, ghostB, s2cVars>>

(***************************************************************************)
(* Burns (USAGE.md "Sending").  AbortPacket: start_packet spent a sequence *)
(* and abort_packet, a frame-acquisition failure or a zero-returning       *)
(* finish on an unfragmented payload gave nothing back; no record exists.  *)
(* SealFailFragmented(n, i): the seal loop failed at fragment i after the  *)
(* block was reserved and fragments 0..i-1 were recorded; every frame is   *)
(* withdrawn, all count sequences burn, nothing is transmitted, and the    *)
(* records of 0..i-1 stay in the ring unacked with their aggregate.        *)
(***************************************************************************)
AbortPacket ==
    /\ Burns
    /\ pending = <<>>
    /\ nextSeq <= MaxSeq
    /\ burned' = burned \cup {nextSeq}
    /\ nextSeq' = nextSeq + 1
    /\ UNCHANGED <<highestSent, sentRec, aggs, numAggs, batch, nextPacketId, nextPid, pending>>
    /\ UNCHANGED <<surfaced, surfacedTwice, sentPayloads, plans, withdrawn, historyOverflow>>
    /\ UNCHANGED <<bVars, ghostB, c2sVars, s2cVars>>

SealFailFragmented(n, i) ==
    LET cnt == PlanCount(n)
        seqs == nextSeq .. nextSeq + cnt - 1
        recorded == nextSeq .. nextSeq + i - 1       \* empty when i = 0
        aggId == numAggs + 1
    IN
    /\ Burns
    /\ pending = <<>>
    /\ i \in 0..cnt-1
    /\ nextSeq + cnt - 1 <= MaxSeq
    /\ numAggs' = aggId
    /\ aggs' = [aggs EXCEPT ![aggId % AggRing] = [id |-> aggId, pid |-> nextPacketId, remaining |-> cnt]]
    /\ sentRec' = RecordAll(sentRec, recorded, aggId)
    /\ historyOverflow' = (historyOverflow \/ AnyOverwrites(sentRec, recorded))
    /\ highestSent' = IF i = 0 THEN highestSent ELSE nextSeq + i - 1
    /\ burned' = burned \cup seqs
    /\ withdrawn' = withdrawn \cup {nextPacketId}
    /\ nextSeq' = nextSeq + cnt
    /\ nextPacketId' = nextPacketId + 1
    /\ nextPid' = NextPid(nextPid)
    /\ UNCHANGED <<batch, pending, surfaced, surfacedTwice, sentPayloads, plans>>
    /\ UNCHANGED <<bVars, ghostB, c2sVars, s2cVars>>

(***************************************************************************)
(* A defective sender: a fragment with a header from Forged, on a fresh    *)
(* sequence, recorded without a packet id.  Exercises the refusals.        *)
(***************************************************************************)
SendForged(h) ==
    /\ pending = <<>>
    /\ nextSeq <= MaxSeq
    /\ C2S!Send(Fragment(nextSeq, h.pid, h.idx, h.cnt, h.bytes))
    /\ sentRec' = RecordSent(sentRec, nextSeq, FALSE, 0, 0)
    /\ historyOverflow' = (historyOverflow \/ OverwritesUnacked(sentRec, nextSeq))
    /\ highestSent' = nextSeq
    /\ nextSeq' = nextSeq + 1
    /\ UNCHANGED <<aggs, numAggs, batch, nextPacketId, nextPid, pending>>
    /\ UNCHANGED <<surfaced, surfacedTwice, sentPayloads, plans, burned, withdrawn>>
    /\ UNCHANGED <<bVars, ghostB, s2cVars>>

(***************************************************************************)
(* B's replay window, as a set: rocketnet_replay_already_received.         *)
(***************************************************************************)
AlreadyReceived(s) ==
    /\ s <= highest
    /\ \/ (highest >= W /\ s <= highest - W)
       \/ s \in opened

(***************************************************************************)
(* B's reassembly: rocketnet_reassembly_process_fragment, check by check   *)
(* in the code's order.  The result names the counter the code bumps and   *)
(* the slot as the code leaves it.  Bytes are sizes: a non-final fragment  *)
(* is placed at idx * stride, the final at stride * (cnt - 1) once the     *)
(* stride is known, else staged.                                           *)
(***************************************************************************)
Older(a, b) == ((a - b) % IdMod) > IdMod \div 2        \* payload_id - slot->payload_id > 0x80000000u

SlotOf(pid) == pid % Slots

ProcessFragment(slot, f) ==
    LET same     == slot.pid = f.pid
        isFinal  == f.idx = f.cnt - 1
        stride   == IF same /\ slot.stride # 0 THEN slot.stride
                    ELSE IF ~isFinal THEN f.bytes ELSE 0
        finalContribution == IF isFinal THEN f.bytes
                             ELSE IF same /\ slot.finalBytes # 0 THEN slot.finalBytes
                             ELSE 1
        tooLarge == stride # 0 /\ stride * (f.cnt - 1) + finalContribution > MaxPayload
        fresh    == [pid |-> f.pid, cnt |-> f.cnt, stride |-> 0, finalBytes |-> 0,
                     finalStaged |-> FALSE, received |-> {}]
        base     == IF same THEN slot ELSE fresh
        placed   == [base EXCEPT
                       !.stride      = IF isFinal THEN @ ELSE (IF @ = 0 THEN stride ELSE @),
                       !.finalStaged = IF isFinal THEN base.stride = 0
                                       ELSE (IF @ /\ base.stride = 0 THEN FALSE ELSE @),
                       !.finalBytes  = IF isFinal THEN f.bytes ELSE @,
                       !.received    = @ \cup {f.idx}]
        complete == Cardinality(placed.received) = placed.cnt
    IN
    CASE f.pid = 0 ->
            [result |-> "zero_payload_id", slot |-> slot, abandoned |-> FALSE]
      [] slot.pid # 0 /\ Older(f.pid, slot.pid) ->
            [result |-> "stale", slot |-> slot, abandoned |-> FALSE]
      [] f.cnt = 1 ->
            [result |-> "count_invalid", slot |-> slot, abandoned |-> FALSE]
      [] f.idx >= f.cnt ->
            [result |-> "index_out_of_range", slot |-> slot, abandoned |-> FALSE]
      [] same /\ f.cnt # slot.cnt ->
            [result |-> "count_mismatch", slot |-> slot, abandoned |-> FALSE]
      [] same /\ slot.stride # 0 /\ ((~isFinal /\ f.bytes # slot.stride) \/ (isFinal /\ f.bytes > slot.stride)) ->
            [result |-> "size_mismatch", slot |-> slot, abandoned |-> FALSE]
      [] same /\ slot.stride = 0 /\ ~isFinal /\ slot.finalStaged /\ slot.finalBytes > stride ->
            [result |-> "size_mismatch", slot |-> EmptySlot, abandoned |-> FALSE]
      [] tooLarge ->
            [result |-> "payload_too_large", slot |-> IF same THEN EmptySlot ELSE slot, abandoned |-> FALSE]
      [] same /\ f.idx \in slot.received ->
            [result |-> "duplicate_index", slot |-> slot, abandoned |-> FALSE]
      [] complete ->
            [result |-> "complete", slot |-> EmptySlot, abandoned |-> ~same /\ slot.pid # 0,
             delivery |-> [pid |-> placed.pid, cnt |-> placed.cnt, stride |-> placed.stride, final |-> placed.finalBytes]]
      [] OTHER ->
            [result |-> "accepted", slot |-> placed, abandoned |-> ~same /\ slot.pid # 0]

(***************************************************************************)
(* B receives a session packet from A: the replay check and advance, then  *)
(* the type.  Every packet drained counts against the update's budget,     *)
(* accepted or not.  A complete payload is delivered into this update's    *)
(* batch if there is room, else dropped and counted BATCH_FULL             *)
(* (rocketnet_fragment.cpp:439-452); the slot clears either way.          *)
(***************************************************************************)
RecvPacket(p) ==
    /\ recvCount < RecvPerUpdate
    /\ C2S!Deliver(p)
    /\ recvCount' = recvCount + 1
    /\ IF AlreadyReceived(p.seq)
       THEN UNCHANGED <<highest, opened, slots, numDelivered, ghostB>>
       ELSE
         /\ opened' = opened \cup {p.seq}
         /\ highest' = IF p.seq > highest THEN p.seq ELSE highest
         /\ IF p.type = "payload"
            THEN UNCHANGED <<slots, numDelivered, ghostB>>
            ELSE LET k == SlotOf(p.pid)
                     r == ProcessFragment(slots[k], p)
                 IN
                 /\ slots' = [slots EXCEPT ![k] = r.slot]
                 /\ abandoned' = IF r.abandoned THEN abandoned \cup {slots[k].pid} ELSE abandoned
                 /\ IF r.result = "complete"
                    THEN /\ completed' = completed \cup {r.delivery.pid}
                         /\ IF numDelivered < DeliveredCap
                            THEN /\ numDelivered' = numDelivered + 1
                                 /\ everDelivered' = everDelivered \cup {r.delivery.pid}
                                 /\ deliveredLog' = deliveredLog \cup {r.delivery}
                                 /\ deliveredTwice' = (deliveredTwice \/ r.delivery.pid \in everDelivered)
                                 /\ UNCHANGED batchFull
                            ELSE /\ batchFull' = TRUE
                                 /\ UNCHANGED <<numDelivered, everDelivered, deliveredLog, deliveredTwice>>
                    ELSE UNCHANGED <<numDelivered, everDelivered, deliveredLog, deliveredTwice, completed, batchFull>>
    /\ UNCHANGED <<aVars, ghostA, s2cVars>>

(***************************************************************************)
(* B sends an ack packet: the block rocketnet_ack_block_build makes from   *)
(* its window, every received sequence from the floor to highest.          *)
(***************************************************************************)
Floor == IF highest >= W THEN highest - W + 1 ELSE 1

Block == {s \in Floor..highest : s \in opened}

SendAck ==
    /\ S2C!Send(Block)
    /\ UNCHANGED <<aVars, bVars, ghostA, ghostB, c2sVars>>

(***************************************************************************)
(* A receives a block: rocketnet_ack_endpoint_process_block over the       *)
(* block's runs, which is rocketnet_ack_apply_run over every named         *)
(* sequence from the highest down.  Above highest_sequence: never sent,    *)
(* not visited.  Below the retained window: ignored.  A record not naming  *)
(* the sequence: unknown.  An unacked record carrying a packet id or an    *)
(* aggregate with the batch full: deferred, untouched.  Otherwise acked:   *)
(* a packet id surfaces; a fragment decrements its aggregate and the       *)
(* payload's id surfaces at zero, unless the aggregate was recycled.       *)
(***************************************************************************)
WindowLow == IF highestSent >= SentRing THEN highestSent - SentRing + 1 ELSE 1

Max(S) == CHOOSE x \in S : \A y \in S : y <= x

Surface(st, pid) ==
    [st EXCEPT !.batch = Append(@, pid),
               !.twice = (@ \/ pid \in st.surfaced \/ (\E j \in DOMAIN st.batch : st.batch[j] = pid))]

ApplyOne(st, s) ==
    LET i == s % SentRing
        rec == st.rec[i]
    IN
    IF rec.seq # s THEN st                               \* named but never sent (unknown)
    ELSE IF rec.acked THEN st
    ELSE IF (rec.hasPid \/ rec.agg # 0) /\ Len(st.batch) >= BatchCap
         THEN [st EXCEPT !.deferred = TRUE]
         ELSE LET acked == [st EXCEPT !.rec[i].acked = TRUE]
              IN IF rec.hasPid THEN Surface(acked, rec.pid)
                 ELSE IF rec.agg # 0
                      THEN LET a == st.aggs[rec.agg % AggRing]
                           IN IF a.id = rec.agg /\ a.remaining > 0
                              THEN LET dec == [acked EXCEPT !.aggs[rec.agg % AggRing].remaining = @ - 1]
                                   IN IF a.remaining = 1 THEN Surface(dec, a.pid) ELSE dec
                              ELSE acked
                      ELSE acked

RECURSIVE ApplyAcks(_, _)
ApplyAcks(st, seqs) ==
    IF seqs = {} THEN st
    ELSE LET s == Max(seqs) IN ApplyAcks(ApplyOne(st, s), seqs \ {s})

RecvAck(b) ==
    LET named == {s \in b : s >= WindowLow /\ s <= highestSent}
        st == ApplyAcks([rec |-> sentRec, aggs |-> aggs, batch |-> batch, surfaced |-> surfaced,
                         twice |-> surfacedTwice, deferred |-> FALSE], named)
    IN
    /\ S2C!Deliver(b)
    /\ sentRec' = st.rec
    /\ aggs' = st.aggs
    /\ batch' = st.batch
    /\ surfaced' = surfaced \cup {st.batch[j] : j \in DOMAIN st.batch}
    /\ surfacedTwice' = st.twice
    /\ UNCHANGED <<nextSeq, highestSent, numAggs, nextPacketId, nextPid, pending>>
    /\ UNCHANGED <<sentPayloads, plans, burned, withdrawn, historyOverflow>>
    /\ UNCHANGED <<bVars, ghostB, c2sVars>>

(***************************************************************************)
(* Updates.  A's update hands the acked batch to the application and       *)
(* starts a new one; B's update hands the delivered batch over and resets  *)
(* its drain budget.  Each update ages the link that feeds that end: a     *)
(* frame has passed there.                                                 *)
(***************************************************************************)
UpdateA ==
    /\ batch' = <<>>
    /\ S2C!Age
    /\ UNCHANGED <<nextSeq, highestSent, sentRec, aggs, numAggs, nextPacketId, nextPid, pending>>
    /\ UNCHANGED <<bVars, ghostA, ghostB, c2sVars>>

UpdateB ==
    /\ numDelivered' = 0
    /\ recvCount' = 0
    /\ C2S!Age
    /\ UNCHANGED <<highest, opened, slots>>
    /\ UNCHANGED <<aVars, ghostA, ghostB, s2cVars>>

(***************************************************************************)
(* The path's own events, one link at a time.                              *)
(***************************************************************************)
C2SEnvironment == C2S!Environment /\ UNCHANGED <<aVars, bVars, ghostA, ghostB, s2cVars>>
S2CEnvironment == S2C!Environment /\ UNCHANGED <<aVars, bVars, ghostA, ghostB, c2sVars>>

Next ==
    \/ SendPayload
    \/ \E n \in PayloadSizes : StartFragmented(n)
    \/ TransmitPending
    \/ AbortPacket
    \/ \E n \in PayloadSizes, i \in 0..MaxCount-1 : SealFailFragmented(n, i)
    \/ \E h \in Forged : SendForged(h)
    \/ \E p \in C2SPackets : RecvPacket(p)
    \/ SendAck
    \/ \E b \in S2CPackets : RecvAck(b)
    \/ UpdateA
    \/ UpdateB
    \/ C2SEnvironment
    \/ S2CEnvironment

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Fairness, for liveness only: both ends keep updating, B keeps sending   *)
(* blocks, A keeps taking them, the ack path is a wire (Link!WireFair),    *)
(* and blocks sent without bound are delivered without bound              *)
(* (AcksGetThrough).  Nothing is assumed about the payload path: the       *)
(* transport never resends, so no liveness property here needs a fragment  *)
(* to arrive.                                                              *)
(*                                                                         *)
(* AcksGetThrough is Link!EventualDelivery stated once for the path rather *)
(* than once per block value.  The per-block form quantifies over every    *)
(* subset of 1..MaxSeq; with sixteen of them TLC's tableau for the         *)
(* negated property came out empty ("Temporal formula is a tautology") and *)
(* the run would have proved nothing.  The path-level form implies the     *)
(* per-block one when the link does not duplicate: every copy is delivered *)
(* at most once, only finitely many blocks differ from the one B sends     *)
(* once A's sends are exhausted, so unbounded deliveries are unbounded     *)
(* deliveries of that block.  The liveness runs set Duplication = FALSE.   *)
(***************************************************************************)
AnyRecvAck == \E b \in S2CPackets : RecvAck(b)

AcksGetThrough == ([]<> <<SendAck>>_vars) => ([]<> <<AnyRecvAck>>_vars)

Fairness ==
    /\ WF_vars(UpdateA) /\ WF_vars(UpdateB)
    /\ WF_vars(SendAck) /\ WF_vars(AnyRecvAck)
    /\ S2C!WireFair
    /\ AcksGetThrough

LiveSpec == Spec /\ Fairness

\* the reversed witness for the fairness: the peers are fair but the ack path
\* is not (no WireFair, no AcksGetThrough), so under "arbitrary" every block
\* may be dropped forever; ArrivedSurfaces must fail.  WireFair alone is not
\* a witness here: with MaxDelay = 0 every copy is deliverable the moment it
\* is sent, so SF on Deliver already defeats the dropper.
LiveSpecNoLinkFairness ==
    /\ Spec
    /\ WF_vars(UpdateA) /\ WF_vars(UpdateB)
    /\ WF_vars(SendAck) /\ WF_vars(AnyRecvAck)

(***************************************************************************)
(* Invariants.                                                             *)
(***************************************************************************)
RecordType == [seq : 0..MaxSeq, acked : BOOLEAN, hasPid : BOOLEAN, pid : 0..MaxPacketId, agg : Nat]
AggType    == [id : Nat, pid : 0..MaxPacketId, remaining : Nat]
SlotType   == [pid : 0..IdMod-1, cnt : 0..MaxCount, stride : 0..MaxFragBytes,
               finalBytes : 0..MaxFragBytes, finalStaged : BOOLEAN, received : SUBSET (0..MaxCount-1)]

TypeOK ==
    /\ nextSeq \in 1..MaxSeq+1 /\ highestSent \in 0..MaxSeq
    /\ sentRec \in [0..SentRing-1 -> RecordType]
    /\ aggs \in [0..AggRing-1 -> AggType]
    /\ batch \in Seq(1..MaxPacketId)
    /\ nextPacketId \in 1..MaxPacketId+1 /\ nextPid \in Pids
    /\ pending \in Seq(C2SPackets)
    /\ highest \in 0..MaxSeq /\ opened \subseteq 1..MaxSeq
    /\ slots \in [0..Slots-1 -> SlotType]
    /\ numDelivered \in 0..DeliveredCap /\ recvCount \in 0..RecvPerUpdate
    /\ C2S!TypeOK /\ S2C!TypeOK

\* the acked batch never exceeds its capacity (the deferral discipline)
AckBatchBounded == Len(batch) <= BatchCap

\* a packet id surfaces at most once (U: acked_packets names each id once)
SurfacedOnce == ~surfacedTwice

\* the acked batch names a packet id only when every packet of it arrived:
\* a single payload's one sequence, a fragmented payload's every fragment
SurfacedOnlyWhenAllArrived ==
    \A pid \in surfaced \cup {batch[j] : j \in DOMAIN batch} :
        pid \in DOMAIN sentPayloads /\ sentPayloads[pid] \subseteq opened

\* a burned sequence is never acked as received, and a withdrawn
\* fragmented payload never surfaces
BurnedNeverAcked ==
    /\ \A i \in 0..SentRing-1 : sentRec[i].seq \in burned => ~sentRec[i].acked
    /\ withdrawn \cap (surfaced \cup {batch[j] : j \in DOMAIN batch}) = {}

\* reassembly state never exceeds its bound: the slot's placed bytes fit
\* the arena, a staged final fits the staging buffer, the received set is
\* within the pinned count, and a slot never holds a complete payload
ReassemblyBounded ==
    \A k \in 0..Slots-1 :
        LET s == slots[k] IN
        s.pid # 0 =>
            /\ s.cnt >= 2
            /\ s.received \subseteq 0..s.cnt-1
            /\ Cardinality(s.received) < s.cnt
            /\ s.stride # 0 => s.stride * (s.cnt - 1) + (IF s.finalBytes = 0 THEN 1 ELSE s.finalBytes) <= MaxPayload
            /\ s.finalStaged => (s.stride = 0 /\ s.finalBytes \in 1..MaxFragBytes)
            /\ (s.cnt - 1) \in s.received => s.finalBytes # 0

\* a payload is delivered whole or not at all: every delivery of an honest
\* payload id has the sender's exact geometry
DeliveredWhole ==
    \A d \in deliveredLog :
        d.pid \in DOMAIN plans =>
            /\ d.cnt = plans[d.pid].cnt
            /\ d.stride = plans[d.pid].stride
            /\ d.final = plans[d.pid].final

\* never twice
DeliveredOnce == ~deliveredTwice

\* under a defective sender the delivered geometry is still one the arena
\* holds: a count of at least two, a final of at least one byte within the
\* stride, and the whole within the configured maximum
DeliveredConsistent ==
    \A d \in deliveredLog :
        /\ d.cnt >= 2
        /\ d.final >= 1 /\ d.final <= d.stride
        /\ d.stride * (d.cnt - 1) + d.final <= MaxPayload

\* a fully received payload is delivered in the step that completes it,
\* so within the update it completed in; this is what the delivered
\* batch's capacity is sized to guarantee (BATCH_FULL "must stay zero")
CompleteIsDelivered == completed = everDelivered

BatchFullNeverFires == ~batchFull

\* a payload id abandoned in a slot never occupies it again (F10: expected
\* to fail; checked in its own run)
NoResurrection == \A k \in 0..Slots-1 : slots[k].pid \notin abandoned

(***************************************************************************)
(* Liveness.                                                               *)
(***************************************************************************)
\* every packet of a payload that B opened: its id eventually surfaces,
\* provided nothing laps the record or the window (MaxSeq <= SentRing, W)
AllArrived(pid) == pid \in DOMAIN sentPayloads /\ sentPayloads[pid] \subseteq opened

ArrivedSurfaces ==
    \A pid \in 1..MaxPacketId : AllArrived(pid) ~> pid \in surfaced

\* the acked batch is drained
BatchDrains == \A pid \in 1..MaxPacketId : (\E j \in DOMAIN batch : batch[j] = pid) ~> pid \in surfaced

=============================================================================
