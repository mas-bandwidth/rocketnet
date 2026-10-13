-------------------------- MODULE FragmentsAndAcks --------------------------
(***************************************************************************)
(* rocketnet: fragment reassembly and the ack window, one direction.       *)
(*                                                                         *)
(* A sender A hands payloads to a receiver B over the shared lossy link    *)
(* tla/Link.tla (D2R, "data to receiver"); B sends ack blocks back over a  *)
(* second instance (R2D).  A is one direction's session sequence and ack   *)
(* endpoint: sdk/rocketnet_ack.cpp (record_sent, open_aggregate,           *)
(* process_block, apply_run) and the fragmented send path                  *)
(* (sdk/rocketnet_client_transport.cpp finish_arena_packet and its server  *)
(* mirror, sdk/rocketnet_client_protocol.cpp start_fragments).  B is the   *)
(* replay window as a set (sdk/rocketnet_replay.cpp), the block builder    *)
(* (rocketnet_ack_block_build) and reassembly                              *)
(* (sdk/rocketnet_fragment.cpp, rocketnet_reassembly_process_fragment).    *)
(*                                                                         *)
(* Sequences.  A spends session sequences in order: one per payload, a     *)
(* block of cnt for a fragmented payload (fragment 0 rides the sequence    *)
(* start_packet spent, the rest come from the block start_fragments        *)
(* reserves), and a burn spends sequences nothing is ever sent on          *)
(* (USAGE.md, "A fragmented payload spends far more than one").  The       *)
(* sender stops at MaxSeq so the state space is finite.                    *)
(*                                                                         *)
(* Payload ids.  A fragmented payload's id is a per-direction counter      *)
(* that skips zero and wraps (next_fragment_payload_id, 1 + (raw-1) mod    *)
(* (2^32-1)); here the counter runs modulo IdSpace from FirstId, so a run  *)
(* can start just under the wrap.  gen is the issue number, a ghost the    *)
(* honest sender stamps on every fragment so the properties can say "older *)
(* in issue order" without ambiguity across the wrap.  A defective         *)
(* fragment carries gen 0.                                                 *)
(*                                                                         *)
(* Bytes.  A fragment's bytes are its size; the slot's arena is the        *)
(* function placed (index -> [id, bytes]) plus the one staging buffer, so  *)
(* that a byte of one id landing in another id's payload is visible.       *)
(* Application packet ids: a plain payload's is its sequence, a fragmented *)
(* payload's is MaxSeq + gen; both are what surfaces in the acked batch.   *)
(*                                                                         *)
(* The receiver's replay state is the abstract set of opened sequences in  *)
(* the window (highestOpened - Window, highestOpened].  The word-level     *)
(* representation and its large-skip clear (F1, docs/TLA-INVENTORY.md) is  *)
(* module RNReplayWindow's business; here every block is truthful by       *)
(* construction unless Witness = "overclaim".                              *)
(*                                                                         *)
(* Time.  Tick is one update on both ends: A's acked batch drains, B's     *)
(* delivered arena and receive drain reset, both links Age.                *)
(*                                                                         *)
(* Witnesses (one constant flip each; the code is Witness = "none",        *)
(* StaleRule = "code"):                                                    *)
(*   "firstfrag"   the aggregate surfaces its packet id on the first       *)
(*                 fragment acknowledged                                   *)
(*   "dropfull"    a packet id that does not fit the batch is dropped      *)
(*                 instead of deferred                                     *)
(*   "overclaim"   the block names every sequence in the window, opened or *)
(*                 not                                                     *)
(*   "noclear"     a newer id evicts the occupant without clearing its     *)
(*                 received bits, placed bytes and staging                 *)
(*   "nosizecheck" the reconstructed-size check is skipped                 *)
(*   "nopin"       a later fragment may change the pinned count            *)
(* StaleRule selects the stale test: "code" is the shipped unsigned        *)
(* difference bounded by StaleAhead (ROCKETNET_MAX_FRAGMENTS), "serial" the *)
(* standard's half-space comparison, "plain" a plain less-than.           *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    MaxSeq,         \* the sender stops at this sequence (model bound)
    SentRing,       \* ROCKETNET_SENT_PACKET_BUFFER_SIZE
    AggRing,        \* ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE
    BatchCap,       \* acked_capacity: packet ids the batch holds per update
    Window,         \* ROCKETNET_REPLAY_WINDOW_BITS at the receiver
    MaxRanges,      \* ROCKETNET_ACK_BLOCK_MAX_RANGES: runs below the first run
    Slots,          \* num_reassembly_slots
    IdSpace,        \* payload ids live in 1..IdSpace-1, arithmetic mod IdSpace
    FirstId,        \* the first payload id the sender issues
    StaleAhead,     \* ROCKETNET_MAX_FRAGMENTS in the "code" stale test
    StaleRule,      \* "code" | "serial" | "plain"
    MaxGen,         \* fragmented payloads (and fragmented burns) the sender may issue
    MaxCount,       \* the largest fragment count any packet may name
    Counts,         \* fragment counts the honest sender uses, within 2..MaxCount
    Stride,         \* the honest non-final fragment size
    FinalBytes,     \* the honest final fragment size, 1..Stride
    MaxPayload,     \* max_payload_bytes at the receiver
    MaxFragment,    \* ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES: the staging bound
    DrainCap,       \* packets the receiver drains per update
    DeliveredCap,   \* delivered_capacity: completions deliverable per update
    DefectIds,      \* ids a defective sender may name (empty = honest sender)
    Burns,          \* BOOLEAN: may sequences burn
    Witness,
    D2RPattern, R2DPattern,                         \* Link patterns
    Capacity, MaxDelay, Duplication, Reordering,    \* Link parameters
    BurstStart, BurstLen

Witnesses == {"none", "firstfrag", "dropfull", "overclaim", "noclear", "nosizecheck", "nopin"}

ASSUME /\ MaxSeq \in Nat \ {0} /\ SentRing \in Nat \ {0} /\ AggRing \in Nat \ {0}
       /\ BatchCap \in Nat \ {0} /\ Window \in Nat \ {0} /\ MaxRanges \in Nat
       /\ Slots \in Nat \ {0} /\ IdSpace \in Nat \ {2} /\ IdSpace >= 4
       /\ FirstId \in 1..IdSpace-1 /\ StaleAhead \in Nat \ {0}
       /\ StaleRule \in {"code", "serial", "plain"}
       /\ MaxGen \in Nat \ {0} /\ MaxCount \in Nat \ {0, 1}
       /\ Counts \subseteq 2..MaxCount /\ Counts # {}
       /\ Stride \in Nat \ {0} /\ FinalBytes \in 1..Stride
       /\ MaxPayload \in Nat \ {0} /\ MaxFragment \in Nat \ {0}
       /\ Stride <= MaxFragment /\ Stride * (MaxCount - 1) + FinalBytes <= MaxPayload
       /\ DrainCap \in Nat \ {0} /\ DeliveredCap \in Nat \ {0}
       /\ DefectIds \subseteq 0..IdSpace-1
       /\ Burns \in BOOLEAN /\ Witness \in Witnesses

Seqs   == 1..MaxSeq
Idx    == 0..MaxCount-1
Ids    == 1..IdSpace-1
Gens   == 1..MaxGen
Pids   == 1..MaxSeq+MaxGen          \* plain: seq; fragmented: MaxSeq + gen
Defect == DefectIds # {}

\* the id the sender's counter yields on its gen-th fragmented payload
IdOf(g) == 1 + ((FirstId - 1 + g - 1) % (IdSpace - 1))

Max(S) == CHOOSE x \in S : \A y \in S : y <= x
Min(S) == CHOOSE x \in S : \A y \in S : y >= x

(***************************************************************************)
(* The packets.  A fragment is the 6-byte header plus its size; a payload  *)
(* is its sequence (its packet id is the sequence).  A block is the set of *)
(* sequences it names: largest is the maximum of that set, the first range *)
(* the run ending there, so the set is the block.                          *)
(***************************************************************************)
HonestFragments ==
    {p \in [type : {"fragment"}, seq : Seqs, gen : Gens, id : Ids,
            idx : Idx, cnt : Counts, bytes : {Stride, FinalBytes}] :
        /\ p.id = IdOf(p.gen)
        /\ p.idx < p.cnt
        /\ p.bytes = IF p.idx = p.cnt - 1 THEN FinalBytes ELSE Stride}

DefectFragments ==
    [type : {"fragment"}, seq : Seqs, gen : {0}, id : DefectIds,
     idx : Idx, cnt : 1..MaxCount, bytes : 1..MaxFragment]

Payloads == [type : {"payload"}, seq : Seqs]

DataPackets == Payloads \cup HonestFragments \cup DefectFragments

AckPackets == SUBSET Seqs

VARIABLES
    \* the sender: the session sequence and rocketnet_ack_endpoint_t
    nextSeq,        \* send_sequence_session: the next sequence to spend
    nextGen,        \* the next fragmented payload's issue number
    sent,           \* the sent ring, [0..SentRing-1 -> record]
    highestSent,    \* highest_sequence
    aggs,           \* the aggregate ring, [0..AggRing-1 -> [id, pid, remaining]]
    numAggs,        \* num_aggregates
    batch,          \* acked_packet_ids: packet ids surfaced since the last update
    outbox,         \* packets sealed and staged, flushed one per step in order
    \* the receiver: the replay window, the drain, reassembly
    highestOpened,  \* replay highest_opened
    opened,         \* opened sequences in (highestOpened - Window, highestOpened]
    drained,        \* packets drained this update
    slots,          \* reassembly slots, [0..Slots-1 -> slot]
    numDelivered,   \* completions delivered this update
    batchFull,      \* a completion was dropped because numDelivered = DeliveredCap
    lastDelivery,   \* the last delivered slot, as it was at completion
    \* ghosts
    burned,         \* sequences spent and never sent
    fragsOf,        \* packet id -> the sequences carrying it
    surfacedCount,  \* packet id -> how often it entered the batch, saturating at 2
    deliveredCount, \* gen -> how often the honest payload was delivered, saturating at 2
    openedEver,     \* every sequence B ever opened
    namedEver,      \* every sequence a block ever named
    badStale,       \* an honest fragment newer than the occupant was refused as stale
    \* the links
    d2rWire, d2rSent, d2rPulse,
    r2dWire, r2dSent, r2dPulse

D2R == INSTANCE Link WITH wire <- d2rWire, sent <- d2rSent, pulse <- d2rPulse,
                          Packets <- DataPackets, Pattern <- D2RPattern
R2D == INSTANCE Link WITH wire <- r2dWire, sent <- r2dSent, pulse <- r2dPulse,
                          Packets <- AckPackets, Pattern <- R2DPattern

senderVars   == <<nextSeq, nextGen, sent, highestSent, aggs, numAggs, batch, outbox>>
receiverVars == <<highestOpened, opened, drained, slots, numDelivered, batchFull, lastDelivery>>
ghostVars    == <<burned, fragsOf, surfacedCount, deliveredCount, openedEver, namedEver, badStale>>
d2rVars      == <<d2rWire, d2rSent, d2rPulse>>
r2dVars      == <<r2dWire, r2dSent, r2dPulse>>
vars         == <<senderVars, receiverVars, ghostVars, d2rVars, r2dVars>>

(***************************************************************************)
(* Records.                                                                *)
(***************************************************************************)
NoRecord   == [seq |-> 0, pid |-> 0, has |-> FALSE, agg |-> 0, acked |-> FALSE]
Record     == [seq : 0..MaxSeq, pid : {0} \cup Pids, has : BOOLEAN, agg : 0..MaxGen, acked : BOOLEAN]
NoAgg      == [id |-> 0, pid |-> 0, remaining |-> 0]
Agg        == [id : 0..MaxGen, pid : {0} \cup Pids, remaining : 0..MaxCount]
EmptyPlace == [id |-> 0, bytes |-> 0]
Place      == [id : 0..IdSpace-1, bytes : 0..MaxFragment]
EmptySlot  == [id |-> 0, gen |-> 0, cnt |-> 0, stride |-> 0, finalBytes |-> -1,
               finalStaged |-> FALSE, staged |-> EmptyPlace, recv |-> {},
               placed |-> [i \in Idx |-> EmptyPlace]]
Slot       == [id : 0..IdSpace-1, gen : 0..MaxGen, cnt : 0..MaxCount, stride : 0..MaxFragment,
               finalBytes : -1..MaxFragment, finalStaged : BOOLEAN, staged : Place,
               recv : SUBSET Idx, placed : [Idx -> Place]]

Sat2(n) == IF n >= 2 THEN 2 ELSE n + 1

Init ==
    /\ nextSeq = 1 /\ nextGen = 1
    /\ sent = [i \in 0..SentRing-1 |-> NoRecord]
    /\ highestSent = 0
    /\ aggs = [i \in 0..AggRing-1 |-> NoAgg]
    /\ numAggs = 0
    /\ batch = <<>>
    /\ outbox = <<>>
    /\ highestOpened = 0 /\ opened = {} /\ drained = 0
    /\ slots = [k \in 0..Slots-1 |-> EmptySlot]
    /\ numDelivered = 0 /\ batchFull = FALSE
    /\ lastDelivery = EmptySlot
    /\ burned = {}
    /\ fragsOf = [p \in Pids |-> {}]
    /\ surfacedCount = [p \in Pids |-> 0]
    /\ deliveredCount = [g \in Gens |-> 0]
    /\ openedEver = {} /\ namedEver = {} /\ badStale = FALSE
    /\ D2R!Init /\ R2D!Init

(***************************************************************************)
(* The sender.                                                             *)
(*                                                                         *)
(* record_sent (sdk/rocketnet_ack.cpp) refuses a sequence at or below the  *)
(* record it would overwrite, or below the retained window; with sequences *)
(* spent in order neither can fire, so the record is simply written.  The  *)
(* overwrite of an unacknowledged record is the advisory overflow counter, *)
(* not a refusal.                                                          *)
(***************************************************************************)
RecordSent(f, s, pid, has, agg) ==
    [f EXCEPT ![s % SentRing] = [seq |-> s, pid |-> pid, has |-> has, agg |-> agg, acked |-> FALSE]]

RECURSIVE RecordAll(_, _, _, _)
RecordAll(f, S, agg, pid) ==
    IF S = {} THEN f
    ELSE LET s == Min(S) IN RecordAll(RecordSent(f, s, pid, FALSE, agg), S \ {s}, agg, pid)

\* start_packet + finish_packet on the wire route: one sequence, one record
\* with the packet id, the packet staged for the flush.
StartPayload ==
    /\ nextSeq <= MaxSeq
    /\ outbox = <<>>
    /\ sent' = RecordSent(sent, nextSeq, nextSeq, TRUE, 0)
    /\ highestSent' = nextSeq
    /\ fragsOf' = [fragsOf EXCEPT ![nextSeq] = {nextSeq}]
    /\ outbox' = <<[type |-> "payload", seq |-> nextSeq]>>
    /\ nextSeq' = nextSeq + 1
    /\ UNCHANGED <<nextGen, aggs, numAggs, batch>>
    /\ UNCHANGED <<receiverVars, burned, surfacedCount, deliveredCount, openedEver, namedEver, badStale, d2rVars, r2dVars>>

\* finish_packet on the arena route (rocketnet_client_transport_finish_arena_packet
\* and the server's): the plan gives cnt, start_fragments reserves cnt - 1
\* sequences after the one start_packet spent, the payload id is drawn,
\* open_aggregate pins the packet id behind cnt acknowledgments, and
\* seal_fragment records each fragment's sequence against the aggregate.
Fragment(s, g, i, cnt) ==
    [type |-> "fragment", seq |-> s, gen |-> g, id |-> IdOf(g), idx |-> i, cnt |-> cnt,
     bytes |-> IF i = cnt - 1 THEN FinalBytes ELSE Stride]

StartFragmented(cnt) ==
    /\ nextSeq + cnt - 1 <= MaxSeq
    /\ nextGen <= MaxGen
    /\ outbox = <<>>
    /\ LET seqs == nextSeq..nextSeq+cnt-1
           pid  == MaxSeq + nextGen
           aggId == numAggs + 1
       IN /\ aggs' = [aggs EXCEPT ![aggId % AggRing] = [id |-> aggId, pid |-> pid, remaining |-> cnt]]
          /\ numAggs' = aggId
          /\ sent' = RecordAll(sent, seqs, aggId, 0)
          /\ highestSent' = nextSeq + cnt - 1
          /\ fragsOf' = [fragsOf EXCEPT ![pid] = seqs]
          /\ outbox' = [i \in 1..cnt |-> Fragment(nextSeq + i - 1, nextGen, i - 1, cnt)]
          /\ nextSeq' = nextSeq + cnt
          /\ nextGen' = nextGen + 1
    /\ UNCHANGED batch
    /\ UNCHANGED <<receiverVars, burned, surfacedCount, deliveredCount, openedEver, namedEver, badStale, d2rVars, r2dVars>>

\* A sequence spent and never sent: start_packet acquired a frame and the
\* packet was aborted, or finish_packet returned zero on the wire route
\* (USAGE.md, "Not every NULL from start_packet spends one").
BurnOne ==
    /\ Burns
    /\ nextSeq <= MaxSeq
    /\ burned' = burned \cup {nextSeq}
    /\ nextSeq' = nextSeq + 1
    /\ UNCHANGED <<nextGen, sent, highestSent, aggs, numAggs, batch, outbox>>
    /\ UNCHANGED <<receiverVars, fragsOf, surfacedCount, deliveredCount, openedEver, namedEver, badStale, d2rVars, r2dVars>>

\* A seal refusal inside the fragment loop: the whole reservation withdraws,
\* the payload id and the aggregate are consumed (both are drawn before the
\* seal loop), nothing is sent.  Fragments sealed before the refusal have
\* records and never reach the wire, which is what the link's loss already
\* models.  A full ring or a failed start_fragments returns before the draw
\* (sdk/rocketnet_client_transport.cpp:805-826) and burns only the sequence
\* start_packet spent: that is BurnOne.
BurnFragmented(cnt) ==
    /\ Burns
    /\ nextSeq + cnt - 1 <= MaxSeq
    /\ nextGen <= MaxGen
    /\ LET seqs == nextSeq..nextSeq+cnt-1
           pid  == MaxSeq + nextGen
           aggId == numAggs + 1
       IN /\ aggs' = [aggs EXCEPT ![aggId % AggRing] = [id |-> aggId, pid |-> pid, remaining |-> cnt]]
          /\ numAggs' = aggId
          /\ fragsOf' = [fragsOf EXCEPT ![pid] = seqs]
          /\ burned' = burned \cup seqs
          /\ nextSeq' = nextSeq + cnt
          /\ nextGen' = nextGen + 1
    /\ UNCHANGED <<sent, highestSent, batch, outbox>>
    /\ UNCHANGED <<receiverVars, surfacedCount, deliveredCount, openedEver, namedEver, badStale, d2rVars, r2dVars>>

\* A defective sender: a sealed fragment whose header says anything.  It is
\* the sender's own session (the seal rules out a third party), so it spends
\* a sequence like any send; it records nothing the properties watch.
DefectiveSend ==
    /\ Defect
    /\ nextSeq <= MaxSeq
    /\ outbox = <<>>
    /\ \E id \in DefectIds, i \in Idx, cnt \in 1..MaxCount, b \in 1..MaxFragment :
         outbox' = <<[type |-> "fragment", seq |-> nextSeq, gen |-> 0, id |-> id, idx |-> i, cnt |-> cnt, bytes |-> b]>>
    /\ nextSeq' = nextSeq + 1
    /\ UNCHANGED <<nextGen, sent, highestSent, aggs, numAggs, batch>>
    /\ UNCHANGED <<receiverVars, ghostVars, d2rVars, r2dVars>>

\* send_packets: the staged packets go to the wire in fill order.
Flush ==
    /\ outbox # <<>>
    /\ D2R!Send(Head(outbox))
    /\ outbox' = Tail(outbox)
    /\ UNCHANGED <<nextSeq, nextGen, sent, highestSent, aggs, numAggs, batch>>
    /\ UNCHANGED <<receiverVars, ghostVars, r2dVars>>

(***************************************************************************)
(* process_block / apply_run (sdk/rocketnet_ack.cpp).  The walk visits the *)
(* named sequences from the highest down, within the retained window and   *)
(* at or below the highest sent; a named sequence with no record is a      *)
(* burned one or a defective one (ACK_UNKNOWN_SEQUENCE, no effect); an     *)
(* unacknowledged record with a packet id or an aggregate that cannot fit  *)
(* the batch is deferred: it stays unacknowledged for a later block.  A    *)
(* fragment's acknowledgment decrements its aggregate, which surfaces the  *)
(* payload's packet id at zero; a lapped aggregate absorbs the ack.  The   *)
(* pre-scan the merged code added (skip a run with nothing to acknowledge) *)
(* changes only the unknown-sequence counter and is not modelled.          *)
(***************************************************************************)
WindowLow == IF highestSent >= SentRing THEN highestSent - SentRing + 1 ELSE 1

\* st = [sent, aggs, batch, sc]
ApplyOne(s, st) ==
    IF s > highestSent \/ s < WindowLow THEN st
    ELSE LET k == s % SentRing
             r == st.sent[k]
         IN IF r.seq # s \/ r.acked THEN st
            ELSE IF (r.has \/ r.agg # 0) /\ Len(st.batch) >= BatchCap THEN
                    IF Witness = "dropfull"
                        THEN [st EXCEPT !.sent[k].acked = TRUE]
                        ELSE st
            ELSE IF r.has THEN
                    [st EXCEPT !.sent[k].acked = TRUE,
                               !.batch = Append(@, r.pid),
                               !.sc[r.pid] = Sat2(@)]
            ELSE IF r.agg # 0 THEN
                    LET j == r.agg % AggRing
                        a == st.aggs[j]
                    IN IF a.id = r.agg /\ a.remaining > 0 THEN
                           LET rem == IF Witness = "firstfrag" THEN 0 ELSE a.remaining - 1
                           IN IF rem = 0
                                THEN [st EXCEPT !.sent[k].acked = TRUE,
                                                !.aggs[j].remaining = 0,
                                                !.batch = Append(@, a.pid),
                                                !.sc[a.pid] = Sat2(@)]
                                ELSE [st EXCEPT !.sent[k].acked = TRUE,
                                                !.aggs[j].remaining = rem]
                       ELSE [st EXCEPT !.sent[k].acked = TRUE]
            ELSE [st EXCEPT !.sent[k].acked = TRUE]

RECURSIVE ApplyNamed(_, _)
ApplyNamed(named, st) ==
    IF named = {} THEN st
    ELSE LET s == Max(named) IN ApplyNamed(named \ {s}, ApplyOne(s, st))

\* The block at the head of the ack link arrives (only the head is ever
\* deliverable, so the receive names it rather than searching AckPackets).
RecvAck ==
    /\ r2dWire # <<>>
    /\ LET b == Head(r2dWire).pkt IN
        /\ R2D!Deliver(b)
        /\ LET st == ApplyNamed(b, [sent |-> sent, aggs |-> aggs, batch |-> batch, sc |-> surfacedCount])
           IN /\ sent' = st.sent
              /\ aggs' = st.aggs
              /\ batch' = st.batch
              /\ surfacedCount' = st.sc
    /\ UNCHANGED <<nextSeq, nextGen, highestSent, numAggs, outbox>>
    /\ UNCHANGED <<receiverVars, burned, fragsOf, deliveredCount, openedEver, namedEver, badStale, d2rVars>>

(***************************************************************************)
(* The receiver's replay window (sdk/rocketnet_replay.cpp) as a set.       *)
(***************************************************************************)
Floor == IF highestOpened >= Window THEN highestOpened - Window + 1 ELSE 1

\* rocketnet_replay_already_received, negated
Fresh(s) == s > highestOpened \/ (s >= Floor /\ s \notin opened)

\* rocketnet_replay_advance
AdvanceHighest(s) == IF s > highestOpened THEN s ELSE highestOpened
AdvanceOpened(s)  == {x \in opened \cup {s} : x > AdvanceHighest(s) - Window}

(***************************************************************************)
(* rocketnet_ack_block_build: the run ending at largest, then (gap, run)   *)
(* pairs downward to the floor, at most MaxRanges of them; a trailing gap  *)
(* names nothing.  The block is the set of sequences named.                *)
(***************************************************************************)
RunDownFrom(s) == {x \in Floor..s : \A y \in x..s : y \in opened}

RECURSIVE NamedFrom(_, _)
NamedFrom(top, runs) ==
    IF top < Floor \/ runs = 0 THEN {}
    ELSE IF top \notin opened THEN NamedFrom(top - 1, runs)
    ELSE LET run == RunDownFrom(top) IN run \cup NamedFrom(Min(run) - 1, runs - 1)

Block ==
    IF highestOpened = 0 THEN {}
    ELSE IF Witness = "overclaim" THEN Floor..highestOpened
    ELSE NamedFrom(highestOpened, 1 + MaxRanges)

\* A session packet carrying the current block.  Every session type carries
\* one; the standalone ack (type 8) is the one sent for the block alone, and
\* the standard lets a peer send it whenever freshness is worth a packet.
SendAck ==
    /\ R2D!Send(Block)
    /\ namedEver' = namedEver \cup Block
    /\ UNCHANGED <<senderVars, receiverVars, burned, fragsOf, surfacedCount, deliveredCount, openedEver, badStale, d2rVars>>

(***************************************************************************)
(* rocketnet_reassembly_process_fragment, the standard's checks in order.  *)
(* Every refusal is decided before anything mutates; the two whole-payload *)
(* refusals clear the slot.                                                *)
(***************************************************************************)
Stale(new, old) ==
    CASE StaleRule = "code"   -> ((new - old) % IdSpace) > StaleAhead
      [] StaleRule = "serial" -> ((old - new) % IdSpace) < IdSpace \div 2
      [] StaleRule = "plain"  -> new < old

ClearSlot(S) == EmptySlot

\* the slot after a newer id takes it, or the slot itself when the id matches
Occupy(S, p) ==
    IF S.id = p.id THEN S
    ELSE IF Witness = "noclear"
         THEN [S EXCEPT !.id = p.id, !.gen = p.gen, !.cnt = p.cnt, !.finalBytes = -1, !.stride = 0]
         ELSE [EmptySlot EXCEPT !.id = p.id, !.gen = p.gen, !.cnt = p.cnt, !.finalBytes = -1]

\* placing the accepted fragment, after Occupy
PlaceFragment(S, p, stride) ==
    LET final == p.idx = p.cnt - 1
        cnt   == IF Witness = "nopin" THEN p.cnt ELSE S.cnt
        S1    == IF Witness = "nopin" THEN [S EXCEPT !.cnt = p.cnt] ELSE S
    IN IF ~final THEN
           LET S2 == IF S1.stride = 0
                     THEN LET S3 == [S1 EXCEPT !.stride = stride]
                          IN IF S3.finalStaged
                             THEN [S3 EXCEPT !.placed[cnt - 1] = S3.staged, !.finalStaged = FALSE, !.staged = EmptyPlace]
                             ELSE S3
                     ELSE S1
           IN [S2 EXCEPT !.placed[p.idx] = [id |-> p.id, bytes |-> p.bytes], !.recv = @ \cup {p.idx}]
       ELSE
           LET S2 == [S1 EXCEPT !.finalBytes = p.bytes]
           IN IF S2.stride # 0
              THEN [S2 EXCEPT !.placed[cnt - 1] = [id |-> p.id, bytes |-> p.bytes], !.recv = @ \cup {p.idx}]
              ELSE [S2 EXCEPT !.staged = [id |-> p.id, bytes |-> p.bytes], !.finalStaged = TRUE, !.recv = @ \cup {p.idx}]

Complete(S) == Cardinality(S.recv) = S.cnt

\* The outcome of one fragment: a record [slot, delivered, dropped, stale]
\* where slot is the slot afterwards, delivered the slot at completion (or
\* EmptySlot), dropped whether a completion hit the delivery bound, stale
\* whether the fragment was refused as stale.
Outcome(S, p) ==
    LET same   == S.id = p.id
        final  == p.idx = p.cnt - 1
        stride == IF same /\ S.stride # 0 THEN S.stride ELSE IF ~final THEN p.bytes ELSE 0
        finalContribution == IF final THEN p.bytes ELSE IF same /\ S.finalBytes >= 0 THEN S.finalBytes ELSE 1
        none   == [slot |-> S, delivered |-> EmptySlot, dropped |-> FALSE, stale |-> FALSE]
    IN IF p.id = 0 THEN none                                                        \* FRAGMENT_ZERO_PAYLOAD_ID
       ELSE IF S.id # 0 /\ ~same /\ Stale(p.id, S.id) THEN [none EXCEPT !.stale = TRUE] \* FRAGMENT_STALE
       ELSE IF p.cnt = 1 THEN none                                                  \* FRAGMENT_COUNT_INVALID
       ELSE IF p.idx >= p.cnt THEN none                                             \* FRAGMENT_INDEX_OUT_OF_RANGE
       ELSE IF same /\ p.cnt # S.cnt /\ Witness # "nopin" THEN none                \* FRAGMENT_COUNT_MISMATCH
       ELSE IF same /\ S.stride # 0 /\ ~final /\ p.bytes # S.stride THEN none       \* FRAGMENT_SIZE_MISMATCH
       ELSE IF same /\ S.stride # 0 /\ final /\ p.bytes > S.stride THEN none        \* FRAGMENT_SIZE_MISMATCH
       ELSE IF same /\ S.stride = 0 /\ ~final /\ S.finalStaged /\ S.finalBytes > stride
            THEN [none EXCEPT !.slot = ClearSlot(S)]                               \* staged final exceeds the stride
       ELSE IF stride # 0 /\ Witness # "nosizecheck"
               /\ stride * (p.cnt - 1) + finalContribution > MaxPayload
            THEN [none EXCEPT !.slot = IF same THEN ClearSlot(S) ELSE S]           \* FRAGMENT_PAYLOAD_TOO_LARGE
       ELSE IF same /\ p.idx \in S.recv THEN none                                   \* FRAGMENT_DUPLICATE_INDEX
       ELSE LET S1 == PlaceFragment(Occupy(S, p), p, stride)
            IN IF Complete(S1)
               THEN IF numDelivered < DeliveredCap
                    THEN [slot |-> EmptySlot, delivered |-> S1, dropped |-> FALSE, stale |-> FALSE]
                    ELSE [slot |-> EmptySlot, delivered |-> EmptySlot, dropped |-> TRUE, stale |-> FALSE]
               ELSE [none EXCEPT !.slot = S1]

RecvFragment(p) ==
    LET k == p.id % Slots
        o == Outcome(slots[k], p)
    IN /\ slots' = [slots EXCEPT ![k] = o.slot]
       /\ numDelivered' = IF o.delivered # EmptySlot THEN numDelivered + 1 ELSE numDelivered
       /\ batchFull' = (batchFull \/ o.dropped)
       /\ lastDelivery' = IF o.delivered # EmptySlot THEN o.delivered ELSE lastDelivery
       /\ deliveredCount' = IF o.delivered # EmptySlot /\ o.delivered.gen # 0
                            THEN [deliveredCount EXCEPT ![o.delivered.gen] = Sat2(@)]
                            ELSE deliveredCount
       /\ badStale' = (badStale \/ (o.stale /\ p.gen # 0 /\ slots[k].gen # 0 /\ p.gen > slots[k].gen))

\* A data packet arrives: the reading order's replay check, then the type's
\* processing.  A refused packet was still drained.
RecvData ==
    /\ drained < DrainCap
    /\ d2rWire # <<>>
    /\ LET p == Head(d2rWire).pkt IN
        /\ D2R!Deliver(p)
        /\ drained' = drained + 1
        /\ IF ~Fresh(p.seq)
           THEN UNCHANGED <<highestOpened, opened, slots, numDelivered, batchFull, lastDelivery, deliveredCount, badStale, openedEver>>
           ELSE /\ highestOpened' = AdvanceHighest(p.seq)
                /\ opened' = AdvanceOpened(p.seq)
                /\ openedEver' = openedEver \cup {p.seq}
                /\ IF p.type = "payload"
                   THEN UNCHANGED <<slots, numDelivered, batchFull, lastDelivery, deliveredCount, badStale>>
                   ELSE RecvFragment(p)
    /\ UNCHANGED <<senderVars, burned, fragsOf, surfacedCount, namedEver, r2dVars>>

(***************************************************************************)
(* Time: one update on both ends.                                          *)
(***************************************************************************)
Tick ==
    /\ batch' = <<>>
    /\ drained' = 0
    /\ numDelivered' = 0
    /\ D2R!Age /\ R2D!Age
    /\ UNCHANGED <<nextSeq, nextGen, sent, highestSent, aggs, numAggs, outbox>>
    /\ UNCHANGED <<highestOpened, opened, slots, batchFull, lastDelivery, ghostVars>>

D2REnvironment == D2R!Environment /\ UNCHANGED <<senderVars, receiverVars, ghostVars, r2dVars>>
R2DEnvironment == R2D!Environment /\ UNCHANGED <<senderVars, receiverVars, ghostVars, d2rVars>>

Next ==
    \/ StartPayload
    \/ \E cnt \in Counts : StartFragmented(cnt)
    \/ BurnOne
    \/ \E cnt \in Counts : BurnFragmented(cnt)
    \/ DefectiveSend
    \/ Flush
    \/ RecvAck
    \/ SendAck
    \/ RecvData
    \/ Tick
    \/ D2REnvironment
    \/ R2DEnvironment

(***************************************************************************)
(* Fairness.  Tick is weakly fair.  The sends and the receives are         *)
(* strongly fair: a head that is due infinitely often is received          *)
(* infinitely often (Link's WireFair for the one deliverable copy), and a  *)
(* peer with something to send that finds room on the wire infinitely     *)
(* often sends infinitely often (Capacity is a model-size knob, never a    *)
(* reason a sender is shut out forever: README-Link.md).  Per link, a link *)
(* sent on without bound delivers without bound (Link's EventualDelivery   *)
(* folded over the link's packets; for the ack link the copy delivered    *)
(* must carry the block the receiver currently holds, or stale duplicates  *)
(* of an old block satisfy it while every current block is dropped, which  *)
(* a cold read of this model proved with TLC).  Link!Fairness itself       *)
(* quantifies strong fairness over every packet value, and with the ack    *)
(* link carrying SUBSET Seqs (256 values) TLC 2.19 reports every temporal  *)
(* property a                                                              *)
(* tautology ("satisfiability problem has 0 branches") instead of checking *)
(* it; README-FragmentsAndAcks.md records the bisection.  The per-link     *)
(* form is enough here because B's state is monotone: once the sender      *)
(* stops, every later block is the same block, so "some block is delivered *)
(* without bound" is "that block is".  With weak fairness on the receives  *)
(* TLC found the Delay/Age starvation README-Link.md describes; with weak  *)
(* fairness on the sends it found Duplicate refilling the wire with copies *)
(* of a stale block after every delivery, so the current block never got   *)
(* on.  Both lassos are in the README.                                     *)
(***************************************************************************)
Fairness ==
    /\ WF_vars(Tick)
    /\ SF_vars(Flush)
    /\ SF_vars(SendAck)
    /\ SF_vars(RecvAck)
    /\ SF_vars(RecvData)
    /\ WF_d2rVars(D2R!Age) /\ WF_r2dVars(R2D!Age)
    /\ ([]<> <<Flush>>_vars)   => ([]<> <<RecvData>>_vars)
    /\ ([]<> <<SendAck>>_vars) => ([]<> <<RecvAck /\ Head(r2dWire).pkt = Block>>_vars)

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* Safety.                                                                 *)
(***************************************************************************)
TypeOK ==
    /\ nextSeq \in 1..MaxSeq+MaxCount /\ nextGen \in 1..MaxGen+1
    /\ sent \in [0..SentRing-1 -> Record]
    /\ highestSent \in 0..MaxSeq
    /\ aggs \in [0..AggRing-1 -> Agg]
    /\ numAggs \in 0..MaxGen
    /\ batch \in Seq(Pids) /\ Len(batch) <= BatchCap
    /\ outbox \in Seq(DataPackets) /\ Len(outbox) <= MaxCount
    /\ highestOpened \in 0..MaxSeq
    /\ opened \subseteq Seqs /\ opened \subseteq Floor..highestOpened
    /\ drained \in 0..DrainCap
    /\ slots \in [0..Slots-1 -> Slot]
    /\ numDelivered \in 0..DeliveredCap
    /\ batchFull \in BOOLEAN
    /\ lastDelivery \in Slot
    /\ burned \subseteq Seqs
    /\ fragsOf \in [Pids -> SUBSET Seqs]
    /\ surfacedCount \in [Pids -> 0..2]
    /\ deliveredCount \in [Gens -> 0..2]
    /\ openedEver \subseteq Seqs /\ namedEver \subseteq Seqs
    /\ badStale \in BOOLEAN
    /\ D2R!TypeOK /\ R2D!TypeOK

\* The acked batch names a packet id at most once, ever (USAGE.md:
\* "the packet ids newly acknowledged by the last update").
SurfacedOnce == \A pid \in Pids : surfacedCount[pid] <= 1

\* A packet id surfaces only when every sequence carrying it was opened by
\* the receiver: for a fragmented payload, every fragment.
SurfacedOnlyIfOpened ==
    \A pid \in Pids : surfacedCount[pid] >= 1 => fragsOf[pid] \subseteq openedEver

\* A burned sequence is never opened and never named by a block, so it is
\* never acknowledged as received.
BurnedNeverNamed == burned \cap (openedEver \cup namedEver) = {}

\* A record with a packet id is marked acknowledged only once its id has
\* surfaced, and an aggregate reaches zero only once its id has: the batch
\* defers, it never drops (sdk/rocketnet_ack.cpp, "a packet id that cannot
\* fit the batch is deferred, never dropped").  The safety face of
\* OpenedSurfacesOrLaps.
AckedMeansSurfaced ==
    /\ \A k \in 0..SentRing-1 :
         (sent[k].seq # 0 /\ sent[k].acked /\ sent[k].has) => surfacedCount[sent[k].pid] >= 1
    /\ \A j \in 0..AggRing-1 :
         (aggs[j].id # 0 /\ aggs[j].remaining = 0) => surfacedCount[aggs[j].pid] >= 1

\* A delivered payload is exactly the fragments of one id, each index once,
\* every non-final fragment the stride, the final at most the stride.
DeliveredExact ==
    lastDelivery = EmptySlot \/
    LET D == lastDelivery IN
        /\ D.recv = 0..D.cnt-1
        /\ ~D.finalStaged
        /\ \A i \in 0..D.cnt-1 :
             /\ D.placed[i].id = D.id
             /\ IF i = D.cnt - 1 THEN D.placed[i].bytes \in 1..D.stride
                                 ELSE D.placed[i].bytes = D.stride

\* An honest payload is delivered at most once.
DeliveredOnce == \A g \in Gens : deliveredCount[g] <= 1

\* A payload is delivered the moment it completes: no slot holds a
\* complete payload (the "within one update" of the card, which the code
\* meets inside the fragment's own process call).
CompleteDelivers == \A k \in 0..Slots-1 : slots[k].id = 0 \/ Cardinality(slots[k].recv) < slots[k].cnt

\* Reassembly state stays inside what was allocated at create: every placed
\* byte inside stride x (cnt - 1) + final <= max payload, the staging buffer
\* at most one fragment of the receive maximum, the received set inside the
\* pinned count.
ArenaBounded ==
    \A k \in 0..Slots-1 :
        LET S == slots[k] IN
            S.id = 0 \/
            /\ S.recv \subseteq 0..S.cnt-1
            /\ S.staged.bytes <= MaxFragment
            /\ \A i \in Idx : S.placed[i].id # 0 =>
                 /\ i < S.cnt
                 /\ IF i = S.cnt - 1
                    THEN S.stride * i + S.placed[i].bytes <= MaxPayload
                    ELSE S.stride * (i + 1) <= MaxPayload

\* A fragment refused as stale is older than the occupant in the sender's
\* issue order (STANDARD.md, Fragmentation: "its payload was already
\* superseded").
StaleOnlyIfOlder == ~badStale

\* A completed payload is never dropped for want of delivery room
\* (sdk/rocketnet_fragment.h: "BATCH_FULL is a true invariant: zero under
\* any legitimate load").
BatchFullNeverFires == ~batchFull

\* The first fragment of a payload id fixes its count (STANDARD.md,
\* FRAGMENT_COUNT_MISMATCH).
CountPinned ==
    [][\A k \in 0..Slots-1 :
         (slots[k].id # 0 /\ slots'[k].id = slots[k].id) => slots'[k].cnt = slots[k].cnt]_vars

(***************************************************************************)
(* Liveness.                                                               *)
(***************************************************************************)
\* The packet id's records are still the retained ones: no sequence lapped,
\* and for a fragmented payload its aggregate not recycled.
Retained(pid) ==
    \A s \in fragsOf[pid] :
        LET r == sent[s % SentRing] IN
            /\ r.seq = s
            /\ r.agg # 0 => aggs[r.agg % AggRing].id = r.agg

OpenedAll(pid) == fragsOf[pid] # {} /\ fragsOf[pid] \subseteq openedEver

\* A payload every fragment of which the receiver opened surfaces in the
\* sender's batch, unless the sender's history laps it first (USAGE.md:
\* "Missing a notification therefore does not prove that a payload was
\* lost").  A deferred id surfaces from a later block.
OpenedSurfacesOrLaps ==
    \A pid \in Pids : [](OpenedAll(pid) => <>(surfacedCount[pid] >= 1 \/ ~Retained(pid)))

=============================================================================
