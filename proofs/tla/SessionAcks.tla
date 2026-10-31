----------------------------- MODULE SessionAcks -----------------------------
(***************************************************************************)
(* rocketnet: the session ack window, one direction.                      *)
(*                                                                         *)
(* A sender A hands payloads to a receiver B over the shared lossy link    *)
(* tla/Link.tla (D2R, "data to receiver"); B sends ack blocks back over a  *)
(* second instance (R2D).  A is one direction's session sequence and ack   *)
(* endpoint: sdk/rocketnet_ack.cpp (record_sent, process_block, apply_run) *)
(* and the staged send of the client and server transports.  B is the     *)
(* replay window as a set (sdk/rocketnet_replay.cpp) and the block builder *)
(* (rocketnet_ack_block_build).                                            *)
(*                                                                         *)
(* Sequences.  A spends session sequences in order: one per payload, and a *)
(* burn spends a sequence nothing is ever sent on (USAGE.md, "Not every    *)
(* NULL from start_packet spends one").  The sender stops at MaxSeq so the *)
(* state space is finite.  A payload's packet id is its sequence; it is    *)
(* what surfaces in the acked batch.                                       *)
(*                                                                         *)
(* The receiver's replay state is the abstract set of opened sequences in  *)
(* the window (highestOpened - Window, highestOpened].  The word-level     *)
(* representation and its large-skip clear is module ReplayWindow's        *)
(* business; here every block is truthful by construction unless Witness = *)
(* "overclaim".  Every packet this model sends is a payload, which is ack- *)
(* eligible once it opens (standard/TRANSPORT.md t15.3), so the block is   *)
(* built from the opened set.                                              *)
(*                                                                         *)
(* Time.  Tick is one update on both ends: A's acked batch drains, B's     *)
(* receive drain resets, both links Age.                                   *)
(*                                                                         *)
(* Witnesses (one constant flip each; the code is Witness = "none"):       *)
(*   "dropfull"    a packet id that does not fit the batch is dropped      *)
(*                 instead of deferred                                     *)
(*   "overclaim"   the block names every sequence in the window, opened or *)
(*                 not                                                     *)
(*   "walkskip"    the bitmap walk skips the lowest named sequence whose   *)
(*                 unacknowledged bit is set                               *)
(*   "staleheld"   the reconcile leaves a slot's held bit set after its    *)
(*                 sequence falls below the retained window                *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    MaxSeq,         \* the sender stops at this sequence (model bound)
    SentRing,       \* ROCKETNET_SENT_PACKET_BUFFER_SIZE
    BatchCap,       \* acked_capacity: packet ids the batch holds per update
    Window,         \* ROCKETNET_REPLAY_WINDOW_BITS at the receiver
    MaxRanges,      \* ROCKETNET_ACK_BLOCK_MAX_RANGES: runs below the first run
    DrainCap,       \* packets the receiver drains per update
    Burns,          \* BOOLEAN: may sequences burn
    Witness,
    D2RPattern, R2DPattern,                         \* Link patterns
    Capacity, MaxDelay, Duplication, Reordering,    \* Link parameters
    BurstStart, BurstLen

Witnesses == {"none", "dropfull", "overclaim", "walkskip", "staleheld"}

ASSUME /\ MaxSeq \in Nat \ {0} /\ SentRing \in Nat \ {0}
       /\ BatchCap \in Nat \ {0} /\ Window \in Nat \ {0} /\ MaxRanges \in Nat
       /\ DrainCap \in Nat \ {0}
       /\ Burns \in BOOLEAN /\ Witness \in Witnesses

Seqs == 1..MaxSeq

Max(S) == CHOOSE x \in S : \A y \in S : y <= x
Min(S) == CHOOSE x \in S : \A y \in S : y >= x

(***************************************************************************)
(* The packets.  A payload is its sequence.  A block is the set of         *)
(* sequences it names: largest is the maximum of that set, the first range *)
(* the run ending there, so the set is the block.                          *)
(***************************************************************************)
DataPackets == Seqs

AckPackets == SUBSET Seqs

VARIABLES
    \* the sender: the session sequence and rocketnet_ack_endpoint_t
    nextSeq,        \* send_sequence_session: the next sequence to spend
    sent,           \* the sent ring, [0..SentRing-1 -> record]
    highestSent,    \* highest_sequence
    batch,          \* acked_packet_ids: packet ids surfaced since the last update
    outbox,         \* packets sealed and staged, flushed one per step in order
    \* the sender's sent-ring bitmaps (sent_held, sent_unacked, sent_bits_checked)
    held,           \* slots holding a written record, as a set of slot indexes
    unacked,        \* slots written and not yet acknowledged
    bitsChecked,    \* the highest sequence whose slot the reconcile has checked
    lastExpect,     \* ghost: the named sequences the last block's walk should visit
    lastVisited,    \* ghost: the named sequences the last block's walk did visit
    \* the receiver: the replay window and the drain
    highestOpened,  \* replay highest_opened
    opened,         \* opened sequences in (highestOpened - Window, highestOpened]
    drained,        \* packets drained this update
    \* ghosts
    burned,         \* sequences spent and never sent
    surfacedCount,  \* packet id -> how often it entered the batch, saturating at 2
    openedEver,     \* every sequence B ever opened
    namedEver,      \* every sequence a block ever named
    \* the links
    d2rWire, d2rSent, d2rPulse,
    r2dWire, r2dSent, r2dPulse

D2R == INSTANCE Link WITH wire <- d2rWire, sent <- d2rSent, pulse <- d2rPulse,
                          Packets <- DataPackets, Pattern <- D2RPattern
R2D == INSTANCE Link WITH wire <- r2dWire, sent <- r2dSent, pulse <- r2dPulse,
                          Packets <- AckPackets, Pattern <- R2DPattern

senderVars   == <<nextSeq, sent, highestSent, batch, outbox>>
receiverVars == <<highestOpened, opened, drained>>
ghostVars    == <<burned, surfacedCount, openedEver, namedEver>>
d2rVars      == <<d2rWire, d2rSent, d2rPulse>>
r2dVars      == <<r2dWire, r2dSent, r2dPulse>>
bitVars      == <<held, unacked, bitsChecked, lastExpect, lastVisited>>
vars         == <<senderVars, receiverVars, ghostVars, d2rVars, r2dVars, bitVars>>

(***************************************************************************)
(* Records.                                                                *)
(***************************************************************************)
NoRecord == [seq |-> 0, acked |-> FALSE]
Record   == [seq : 0..MaxSeq, acked : BOOLEAN]

Sat2(n) == IF n >= 2 THEN 2 ELSE n + 1

Init ==
    /\ nextSeq = 1
    /\ sent = [i \in 0..SentRing-1 |-> NoRecord]
    /\ highestSent = 0
    /\ batch = <<>>
    /\ outbox = <<>>
    /\ highestOpened = 0 /\ opened = {} /\ drained = 0
    /\ burned = {}
    /\ surfacedCount = [s \in Seqs |-> 0]
    /\ openedEver = {} /\ namedEver = {}
    /\ held = {} /\ unacked = {} /\ bitsChecked = 0
    /\ lastExpect = {} /\ lastVisited = {}
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

\* start_packet + finish_packet: one sequence, one record with the packet
\* id, the packet staged for the flush.
StartPayload ==
    /\ nextSeq <= MaxSeq
    /\ outbox = <<>>
    /\ sent' = [sent EXCEPT ![nextSeq % SentRing] = [seq |-> nextSeq, acked |-> FALSE]]
    /\ held' = held \cup {nextSeq % SentRing}
    /\ unacked' = unacked \cup {nextSeq % SentRing}
    /\ highestSent' = nextSeq
    /\ outbox' = <<nextSeq>>
    /\ nextSeq' = nextSeq + 1
    /\ UNCHANGED batch
    /\ UNCHANGED <<receiverVars, ghostVars, d2rVars, r2dVars>>
    /\ UNCHANGED <<bitsChecked, lastExpect, lastVisited>>

\* A sequence spent and never sent: a client start_packet takes it and then
\* refuses (the client's send ring or lease table), or an already-started
\* client packet is aborted or its finish is refused. The server assigns its
\* sequence only after finish accepts the payload and reserves its wire
\* frame, so its refused starts, aborted starts and refused finishes spend
\* none. USAGE.md, "Not every NULL from start_packet spends one".
BurnOne ==
    /\ Burns
    /\ nextSeq <= MaxSeq
    /\ burned' = burned \cup {nextSeq}
    /\ nextSeq' = nextSeq + 1
    /\ UNCHANGED <<sent, highestSent, batch, outbox>>
    /\ UNCHANGED <<receiverVars, surfacedCount, openedEver, namedEver, d2rVars, r2dVars>>
    /\ UNCHANGED bitVars

\* send_packets: the staged packets go to the wire in fill order.
Flush ==
    /\ outbox # <<>>
    /\ D2R!Send(Head(outbox))
    /\ outbox' = Tail(outbox)
    /\ UNCHANGED <<nextSeq, sent, highestSent, batch>>
    /\ UNCHANGED <<receiverVars, ghostVars, r2dVars, bitVars>>

(***************************************************************************)
(* process_block / apply_run (sdk/rocketnet_ack.cpp).  The walk visits the *)
(* named sequences from the highest down, within the retained window and   *)
(* at or below the highest sent; a named sequence with no record is a      *)
(* burned one (ACK_UNKNOWN_SEQUENCE, no effect); an unacknowledged record  *)
(* whose packet id cannot fit the batch is deferred: it stays              *)
(* unacknowledged for a later block.  The pre-scan (skip a run with        *)
(* nothing to acknowledge) changes only the unknown-sequence counter and   *)
(* is not modelled.                                                        *)
(***************************************************************************)
WindowLow == IF highestSent >= SentRing THEN highestSent - SentRing + 1 ELSE 1

\* st = [sent, batch, sc, unacked, visited]
ApplyOne(s, st) ==
    IF s > highestSent \/ s < WindowLow THEN st
    ELSE LET k == s % SentRing
             r == st.sent[k]
         IN IF r.seq # s \/ r.acked THEN st
            ELSE IF Len(st.batch) >= BatchCap THEN
                    IF Witness = "dropfull"
                        THEN [st EXCEPT !.sent[k].acked = TRUE, !.unacked = @ \ {k}]
                        ELSE st
            ELSE [st EXCEPT !.sent[k].acked = TRUE, !.unacked = @ \ {k},
                            !.batch = Append(@, s),
                            !.sc[s] = Sat2(@)]

(***************************************************************************)
(* The sent-ring bitmaps (sdk/rocketnet_ack.cpp).  held and unacked are    *)
(* the bits as sets of slot indexes.  A record write sets its slot in      *)
(* both; an ack clears its slot from unacked; a deferred record keeps it.  *)
(* At the top of process_block the reconcile checks every sequence from    *)
(* bitsChecked + 1 to highestSent, clamped below to WindowLow: a slot that *)
(* does not hold its sequence (a burned sequence, or a record that fell    *)
(* below the window) loses both bits.  The walk then visits a named        *)
(* sequence s in the window only when slot s % SentRing has its            *)
(* unacknowledged bit set and holds s.                                     *)
(***************************************************************************)
ReconcileSlots ==
    {x % SentRing : x \in {y \in Max({bitsChecked + 1, WindowLow})..highestSent :
                              sent[y % SentRing].seq # y}}

\* the walk over the named sequences, highest first; skip is the sequence
\* the "walkskip" witness passes over (0 for none)
RECURSIVE ApplyNamed(_, _, _)
ApplyNamed(named, st, skip) ==
    IF named = {} THEN st
    ELSE LET s     == Max(named)
             k     == s % SentRing
             visit == /\ s <= highestSent /\ s >= WindowLow
                      /\ k \in st.unacked /\ st.sent[k].seq = s
                      /\ s # skip
         IN ApplyNamed(named \ {s},
                       IF visit THEN [ApplyOne(s, st) EXCEPT !.visited = @ \cup {s}] ELSE st,
                       skip)

\* The block at the head of the ack link arrives (only the head is ever
\* deliverable, so the receive names it rather than searching AckPackets).
InWindow(s) == s >= WindowLow /\ s <= highestSent

RecvAck ==
    /\ r2dWire # <<>>
    /\ LET b      == Head(r2dWire).pkt
           h1     == IF Witness = "staleheld" THEN held ELSE held \ ReconcileSlots
           u1     == unacked \ ReconcileSlots
           expect == {s \in b : InWindow(s) /\ sent[s % SentRing].seq = s /\ ~sent[s % SentRing].acked}
           cand   == {s \in b : InWindow(s) /\ s % SentRing \in u1 /\ sent[s % SentRing].seq = s}
           skip   == IF Witness = "walkskip" /\ cand # {} THEN Min(cand) ELSE 0
       IN
        /\ R2D!Deliver(b)
        /\ LET st == ApplyNamed(b, [sent |-> sent, batch |-> batch, sc |-> surfacedCount,
                                    unacked |-> u1, visited |-> {}], skip)
           IN /\ sent' = st.sent
              /\ batch' = st.batch
              /\ surfacedCount' = st.sc
              /\ unacked' = st.unacked
              /\ lastVisited' = st.visited
        /\ held' = h1
        /\ bitsChecked' = highestSent
        /\ lastExpect' = expect
    /\ UNCHANGED <<nextSeq, highestSent, outbox>>
    /\ UNCHANGED <<receiverVars, burned, openedEver, namedEver, d2rVars>>

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
\* the standard lets an endpoint send it whenever freshness is worth a packet.
SendAck ==
    /\ R2D!Send(Block)
    /\ namedEver' = namedEver \cup Block
    /\ UNCHANGED <<senderVars, receiverVars, burned, surfacedCount, openedEver, d2rVars, bitVars>>

\* A data packet arrives: the reading order's replay check, then the
\* payload is opened.  A refused packet was still drained.
RecvData ==
    /\ drained < DrainCap
    /\ d2rWire # <<>>
    /\ LET s == Head(d2rWire).pkt IN
        /\ D2R!Deliver(s)
        /\ drained' = drained + 1
        /\ IF ~Fresh(s)
           THEN UNCHANGED <<highestOpened, opened, openedEver>>
           ELSE /\ highestOpened' = AdvanceHighest(s)
                /\ opened' = AdvanceOpened(s)
                /\ openedEver' = openedEver \cup {s}
    /\ UNCHANGED <<senderVars, burned, surfacedCount, namedEver, r2dVars, bitVars>>

(***************************************************************************)
(* Time: one update on both ends.                                          *)
(***************************************************************************)
Tick ==
    /\ batch' = <<>>
    /\ drained' = 0
    /\ D2R!Age /\ R2D!Age
    /\ UNCHANGED <<nextSeq, sent, highestSent, outbox>>
    /\ UNCHANGED <<highestOpened, opened, ghostVars, bitVars>>

D2REnvironment == D2R!Environment /\ UNCHANGED <<senderVars, receiverVars, ghostVars, r2dVars, bitVars>>
R2DEnvironment == R2D!Environment /\ UNCHANGED <<senderVars, receiverVars, ghostVars, d2rVars, bitVars>>

Next ==
    \/ StartPayload
    \/ BurnOne
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
(* infinitely often (Link's WireFair for the one deliverable copy), and an *)
(* endpoint with something to send that finds room on the wire infinitely  *)
(* often sends infinitely often (Capacity is a model-size knob, never a    *)
(* reason a sender is shut out forever: README-Link.md).  Per link, a link *)
(* sent on without bound delivers without bound (Link's EventualDelivery   *)
(* folded over the link's packets).  For the ack link the copy delivered   *)
(* must carry the block the receiver currently holds: a link that keeps    *)
(* delivering stale duplicates of an old block while every current block   *)
(* is dropped satisfies "some block is delivered without bound" and starves *)
(* the property.  Naming the current block is sound because Block is a     *)
(* function of B's state alone, every SendAck carries it, and B's state is *)
(* monotone, so once the sender stops, the block named is the one that     *)
(* carries the information the property needs.  Link!Fairness itself      *)
(* quantifies strong fairness over every packet value; with the ack link   *)
(* carrying SUBSET Seqs TLC reports every temporal property a tautology    *)
(* ("satisfiability problem has 0 branches") instead of checking it, so    *)
(* the per-link form is stated here (README-SessionAcks.md).               *)
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
    /\ nextSeq \in 1..MaxSeq+1
    /\ sent \in [0..SentRing-1 -> Record]
    /\ highestSent \in 0..MaxSeq
    /\ batch \in Seq(Seqs) /\ Len(batch) <= BatchCap
    /\ outbox \in Seq(DataPackets) /\ Len(outbox) <= 1
    /\ highestOpened \in 0..MaxSeq
    /\ opened \subseteq Seqs /\ opened \subseteq Floor..highestOpened
    /\ drained \in 0..DrainCap
    /\ burned \subseteq Seqs
    /\ surfacedCount \in [Seqs -> 0..2]
    /\ openedEver \subseteq Seqs /\ namedEver \subseteq Seqs
    /\ held \subseteq 0..SentRing-1 /\ unacked \subseteq 0..SentRing-1
    /\ bitsChecked \in 0..MaxSeq
    /\ lastExpect \subseteq Seqs /\ lastVisited \subseteq Seqs
    /\ D2R!TypeOK /\ R2D!TypeOK

\* The acked batch names a packet id at most once, ever (USAGE.md:
\* "the packet ids newly acknowledged by the last update").
SurfacedOnce == \A s \in Seqs : surfacedCount[s] <= 1

\* A packet id surfaces only when the receiver opened its sequence.  The
\* reversed witness "overclaim" names an unopened one.
SurfacedOnlyIfOpened ==
    \A s \in Seqs : surfacedCount[s] >= 1 => s \in openedEver

\* A burned sequence is never opened and never named by a block, so it is
\* never acknowledged as received.
BurnedNeverNamed == burned \cap (openedEver \cup namedEver) = {}

\* A record is marked acknowledged only once its packet id has surfaced:
\* the batch defers, it never drops (sdk/rocketnet_ack.cpp, "a packet id
\* that cannot fit the batch is deferred, never dropped").  The safety face
\* of OpenedSurfacesOrLaps.
AckedMeansSurfaced ==
    \A k \in 0..SentRing-1 :
        (sent[k].seq # 0 /\ sent[k].acked) => surfacedCount[sent[k].seq] >= 1

\* The bitmaps never miss a record: every slot whose record is in the
\* window, written and not acknowledged has its unacknowledged bit set, and
\* an unacknowledged bit is only ever set on a held slot.
BitsCoverRing ==
    /\ unacked \subseteq held
    /\ \A k \in 0..SentRing-1 :
         (sent[k].seq # 0 /\ InWindow(sent[k].seq) /\ ~sent[k].acked) => k \in unacked

\* Once the reconcile has caught up with the highest sent sequence, the
\* bits are exactly the ring: held is the slots whose record is in the
\* window, unacked those of them not acknowledged (the
\* test_ack_block_walk_bits_match_ring unit test checks the same).
BitsMatchRing ==
    bitsChecked = highestSent =>
        /\ held = {k \in 0..SentRing-1 : sent[k].seq # 0 /\ sent[k].seq >= WindowLow}
        /\ unacked = {k \in held : ~sent[k].acked}

\* The bitmap walk visits exactly the named sequences in the window whose
\* ring record holds them unacknowledged, read from the ring before the walk.
WalkVisitsUnacked == lastVisited = lastExpect

(***************************************************************************)
(* Liveness.                                                               *)
(***************************************************************************)
\* The payload's record is still the retained one: its sequence not lapped.
Retained(s) == sent[s % SentRing].seq = s

\* A payload the receiver opened surfaces in the sender's batch, unless the
\* sender's history laps it first (USAGE.md: "Missing a notification
\* therefore does not prove that a payload was lost").  A deferred id
\* surfaces from a later block.
OpenedSurfacesOrLaps ==
    \A s \in Seqs : [](s \in openedEver => <>(surfacedCount[s] >= 1 \/ ~Retained(s)))

=============================================================================
