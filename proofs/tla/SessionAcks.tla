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
(* "overclaim".  Every packet this model sends is a payload.  A payload is *)
(* ack-eligible once the message layer accepts it, not when it opens       *)
(* (standard/TRANSPORT.md t15.3): the block is built from the accepted     *)
(* history (accepted_session, rocketnet_session_ack_block_build), which a  *)
(* payload enters only through the accept (rocketnet_server_protocol_      *)
(* accept_payload, rocketnet_client_protocol_accept_payload), and the      *)
(* accept is what raises the ack debt.                                     *)
(*                                                                         *)
(* The receiver update, in the code order: the receive drain (RecvData),   *)
(* the update ack decision (Sweep: the server maintenance sweep and the    *)
(* client protocol update, which pay a debt carried in), the message layer *)
(* accepting what arrived (Accept), then the send pass (LatePass:          *)
(* rocketnet_server_transport_write_accepted_acks and                      *)
(* rocketnet_client_protocol_write_owed_ack), which pays the debt the      *)
(* accepts raised with a standalone ack unless one already went to the     *)
(* peer this update (the standalone_ack_time stamp) or the wire has no     *)
(* room.  SameUpdateAck and OneStandalonePerUpdate state the two rules.    *)
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
(*   "skipdeferred" the block memo is kept after an apply that deferred an *)
(*                 id (the batch was full)                                 *)
(*   "lateorder"   the order before the fix: no send pass, so the debt an  *)
(*                 accept raises waits for the next update ack decision    *)
(*   "nostamp"     the send pass ignores the stamp and sends a second      *)
(*                 standalone ack after the update ack decision sent one   *)
(*                                                                         *)
(* The block memo.  process_block remembers the last block it applied in   *)
(* full when nothing was deferred, and a send clears it; the same block    *)
(* again is RecvAckRepeat, which walks nothing.  Here every block names    *)
(* only sequences already sent, so the send clear never changes what a     *)
(* repeat would ack; it keeps the code's unknown-sequence count exact and  *)
(* covers a block naming an unsent sequence, which the unit test           *)
(* test_ack_block_memo_matches_the_full_apply drives against the full      *)
(* apply.  The deferral rule is what this model can break.                 *)
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

Witnesses == {"none", "dropfull", "overclaim", "walkskip", "staleheld", "skipdeferred",
              "lateorder", "nostamp"}

(***************************************************************************)
(* AckTiming.  TRUE runs the receiver update in its steps (Sweep, Accept,  *)
(* LatePass) with the debt and the stamp, where SameUpdateAck and          *)
(* OneStandalonePerUpdate are checked (MCSessionAcksTiming.cfg and the two *)
(* witnesses).  FALSE, the default, accepts a payload in the step that     *)
(* opens it: the block is still built from the accepted history, and the   *)
(* ack-window runs keep the state space they had.  A definition, replaced  *)
(* with <- in the timing cfgs.                                             *)
(***************************************************************************)
AckTiming == FALSE

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
    \* the block memo (block_memo): the last block applied in full
    memoValid,      \* valid
    memoBlock,      \* the block's fields, as the set of sequences it names
    memoHigh,       \* highest_sequence when it was applied
    repeatExact,    \* ghost: every skipped repeat left what the full apply would
    \* the receiver: the replay window and the drain
    highestOpened,  \* replay highest_opened
    opened,         \* opened sequences in (highestOpened - Window, highestOpened]
    drained,        \* packets drained this update
    \* the receiver: the accepted history and the update ack timing
    highestAccepted, \* accepted_session highest
    accepted,       \* accepted sequences in (highestAccepted - Window, highestAccepted]
    pending,        \* opened payloads the message layer has not accepted yet
    debt,           \* ack_debt >= ROCKETNET_ACK_DEBT_THRESHOLD (1)
    swept,          \* this update ack decision has run
    lateDone,       \* this update send pass has run
    lateRefused,    \* this update send pass found no room for its standalone ack
    standalones,    \* standalone acks sent to the peer this update
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
timingVars   == <<debt, swept, lateDone, lateRefused, standalones>>
receiverVars == <<highestOpened, opened, drained, highestAccepted, accepted, pending, timingVars>>
ghostVars    == <<burned, surfacedCount, openedEver, namedEver>>
d2rVars      == <<d2rWire, d2rSent, d2rPulse>>
r2dVars      == <<r2dWire, r2dSent, r2dPulse>>
memoVars     == <<memoValid, memoBlock, memoHigh, repeatExact>>
bitVars      == <<held, unacked, bitsChecked, lastExpect, lastVisited, memoVars>>
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
    /\ highestAccepted = 0 /\ accepted = {} /\ pending = {}
    /\ debt = FALSE /\ swept = FALSE /\ lateDone = FALSE /\ lateRefused = FALSE
    /\ standalones = 0
    /\ burned = {}
    /\ surfacedCount = [s \in Seqs |-> 0]
    /\ openedEver = {} /\ namedEver = {}
    /\ held = {} /\ unacked = {} /\ bitsChecked = 0
    /\ lastExpect = {} /\ lastVisited = {}
    /\ memoValid = FALSE /\ memoBlock = {} /\ memoHigh = 0 /\ repeatExact = TRUE
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
    /\ memoValid' = FALSE
    /\ UNCHANGED <<memoBlock, memoHigh, repeatExact>>

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

\* The block memo holds b: the last block applied in full, at the same
\* high-water mark.
MemoHit(b) ==
    /\ memoValid
    /\ memoBlock = b
    /\ memoHigh = highestSent

\* the reconcile at the top of process_block, and the full apply of b from it
ReconciledHeld   == IF Witness = "staleheld" THEN held ELSE held \ ReconcileSlots
ReconciledUnacked == unacked \ ReconcileSlots
FullApply(b, skip) ==
    ApplyNamed(b, [sent |-> sent, batch |-> batch, sc |-> surfacedCount,
                   unacked |-> ReconciledUnacked, visited |-> {}], skip)
Expect(b) == {s \in b : InWindow(s) /\ sent[s % SentRing].seq = s /\ ~sent[s % SentRing].acked}

\* process_block's full apply (rocketnet_ack_endpoint_process_block_full,
\* and process_block when the memo does not hold b): the walk, then the
\* memo set to b unless an id was deferred (a visited record left
\* unacknowledged)
RecvAck ==
    /\ r2dWire # <<>>
    /\ LET b      == Head(r2dWire).pkt
           cand   == {s \in b : InWindow(s) /\ s % SentRing \in ReconciledUnacked /\ sent[s % SentRing].seq = s}
           skip   == IF Witness = "walkskip" /\ cand # {} THEN Min(cand) ELSE 0
       IN
        /\ ~MemoHit(b)
        /\ R2D!Deliver(b)
        /\ LET st == FullApply(b, skip)
               deferred == \E s \in st.visited : ~st.sent[s % SentRing].acked
           IN /\ sent' = st.sent
              /\ batch' = st.batch
              /\ surfacedCount' = st.sc
              /\ unacked' = st.unacked
              /\ lastVisited' = st.visited
              /\ memoValid' = (~deferred \/ Witness = "skipdeferred")
        /\ held' = ReconciledHeld
        /\ bitsChecked' = highestSent
        /\ lastExpect' = Expect(b)
        /\ memoBlock' = b
        /\ memoHigh' = highestSent
    /\ UNCHANGED <<nextSeq, highestSent, outbox, repeatExact>>
    /\ UNCHANGED <<receiverVars, burned, openedEver, namedEver, d2rVars>>

\* process_block when the memo holds b: the reconcile, then nothing walked,
\* nothing acked, nothing surfaced. repeatExact records whether the full
\* apply would have left the same ring, batch, surfaced counts and bits.
RecvAckRepeat ==
    /\ r2dWire # <<>>
    /\ LET b  == Head(r2dWire).pkt
           st == FullApply(b, 0)
       IN
        /\ MemoHit(b)
        /\ R2D!Deliver(b)
        /\ repeatExact' = (repeatExact /\ st.sent = sent /\ st.batch = batch
                                       /\ st.sc = surfacedCount /\ st.unacked = ReconciledUnacked)
        /\ lastExpect' = Expect(b)
    /\ held' = ReconciledHeld
    /\ unacked' = ReconciledUnacked
    /\ bitsChecked' = highestSent
    /\ lastVisited' = {}
    /\ UNCHANGED <<sent, batch, surfacedCount, memoValid, memoBlock, memoHigh>>
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
\* The block is built from the accepted history (accepted_session), whose
\* window runs from its own highest sequence.
AFloor == IF highestAccepted >= Window THEN highestAccepted - Window + 1 ELSE 1

\* rocketnet_replay_already_received on accepted_session, negated
AFresh(s) == s > highestAccepted \/ (s >= AFloor /\ s \notin accepted)

\* rocketnet_replay_advance on accepted_session
AAdvanceHighest(s) == IF s > highestAccepted THEN s ELSE highestAccepted
AAdvance(s)        == {x \in accepted \cup {s} : x > AAdvanceHighest(s) - Window}

RunDownFrom(s) == {x \in AFloor..s : \A y \in x..s : y \in accepted}

RECURSIVE NamedFrom(_, _)
NamedFrom(top, runs) ==
    IF top < AFloor \/ runs = 0 THEN {}
    ELSE IF top \notin accepted THEN NamedFrom(top - 1, runs)
    ELSE LET run == RunDownFrom(top) IN run \cup NamedFrom(Min(run) - 1, runs - 1)

Block ==
    IF highestAccepted = 0 THEN {}
    ELSE IF Witness = "overclaim" THEN AFloor..highestAccepted
    ELSE NamedFrom(highestAccepted, 1 + MaxRanges)

\* One of the receiver own session packets (a payload or a keep-alive)
\* carrying the current block.  Every session type carries one, and any of
\* them clears the debt (TRANSPORT.md t15.4).  It is not a standalone ack.
SendAck ==
    /\ R2D!Send(Block)
    /\ namedEver' = namedEver \cup Block
    /\ debt' = FALSE
    /\ UNCHANGED <<senderVars, highestOpened, opened, drained, highestAccepted, accepted,
                   pending, swept, lateDone, lateRefused, standalones,
                   burned, surfacedCount, openedEver, d2rVars, bitVars>>

\* A standalone ack (type 8) when the wire has room: the block alone, the
\* debt paid, one more standalone this update.  No room: the debt stays.
StandaloneOrRefused(refused) ==
    IF R2D!InFlight < Capacity
      THEN /\ R2D!Send(Block)
           /\ namedEver' = namedEver \cup Block
           /\ debt' = FALSE /\ standalones' = standalones + 1
           /\ refused = FALSE
      ELSE /\ UNCHANGED <<r2dVars, namedEver, debt, standalones>>
           /\ refused = TRUE

\* The update ack decision (the server maintenance sweep in
\* update_finish_body, rocketnet_server_protocol_write_ack_if_due; the
\* client in rocketnet_client_protocol_update): a debt carried into the
\* update is paid with a standalone ack.  It runs after the receive drain
\* and before the message layer accepts anything this update.
Sweep ==
    /\ AckTiming /\ ~swept
    /\ swept' = TRUE
    /\ IF debt THEN StandaloneOrRefused(TRUE) \/ StandaloneOrRefused(FALSE)
               ELSE UNCHANGED <<r2dVars, namedEver, debt, standalones>>
    /\ UNCHANGED <<senderVars, highestOpened, opened, drained, highestAccepted, accepted,
                   pending, lateDone, lateRefused, burned, surfacedCount, openedEver,
                   d2rVars, bitVars>>

\* The message layer accepts a payload opened this update: it enters the
\* accepted history unless that history already holds it, and the accept
\* raises the debt (accept_payload).  Every payload that arrived is accepted
\* before the send pass; a message layer that refuses one is outside this
\* model.
Accept(s) ==
    /\ AckTiming /\ swept /\ ~lateDone /\ s \in pending
    /\ pending' = pending \ {s}
    /\ IF AFresh(s)
         THEN /\ highestAccepted' = AAdvanceHighest(s)
              /\ accepted' = AAdvance(s)
              /\ debt' = TRUE
         ELSE UNCHANGED <<highestAccepted, accepted, debt>>
    /\ UNCHANGED <<senderVars, highestOpened, opened, drained, swept, lateDone, lateRefused,
                   standalones, ghostVars, d2rVars, r2dVars, bitVars>>

\* The send pass (rocketnet_server_transport_write_accepted_acks,
\* rocketnet_client_protocol_write_owed_ack, at the start of send_packets):
\* a debt the accepts raised is paid by a standalone ack in the same update,
\* unless one already went to the peer this update (the stamp).  Under
\* "lateorder" there is no send pass; under "nostamp" it ignores the stamp.
LatePass ==
    /\ AckTiming /\ swept /\ ~lateDone /\ pending = {}
    /\ lateDone' = TRUE
    /\ IF /\ debt /\ Witness # "lateorder"
          /\ (standalones = 0 \/ Witness = "nostamp")
         THEN \/ StandaloneOrRefused(TRUE) /\ lateRefused' = TRUE
              \/ StandaloneOrRefused(FALSE) /\ UNCHANGED lateRefused
         ELSE UNCHANGED <<r2dVars, namedEver, debt, standalones, lateRefused>>
    /\ UNCHANGED <<senderVars, highestOpened, opened, drained, highestAccepted, accepted,
                   pending, swept, burned, surfacedCount, openedEver, d2rVars, bitVars>>

\* A data packet arrives: the reading order's replay check, then the
\* payload is opened.  A refused packet was still drained.
RecvData ==
    /\ ~swept
    /\ drained < DrainCap
    /\ d2rWire # <<>>
    /\ LET s == Head(d2rWire).pkt IN
        /\ D2R!Deliver(s)
        /\ drained' = drained + 1
        /\ IF ~Fresh(s)
           THEN UNCHANGED <<highestOpened, opened, openedEver, pending, highestAccepted, accepted>>
           ELSE /\ highestOpened' = AdvanceHighest(s)
                /\ opened' = AdvanceOpened(s)
                /\ openedEver' = openedEver \cup {s}
                /\ IF AckTiming
                     THEN /\ pending' = pending \cup {s}
                          /\ UNCHANGED <<highestAccepted, accepted>>
                     ELSE /\ UNCHANGED pending
                          /\ IF AFresh(s)
                               THEN /\ highestAccepted' = AAdvanceHighest(s)
                                    /\ accepted' = AAdvance(s)
                               ELSE UNCHANGED <<highestAccepted, accepted>>
    /\ UNCHANGED <<senderVars, timingVars,
                   burned, surfacedCount, namedEver, r2dVars, bitVars>>

(***************************************************************************)
(* Time: one update on both ends.                                          *)
(***************************************************************************)
Tick ==
    /\ ~AckTiming \/ (swept /\ lateDone)
    /\ batch' = <<>>
    /\ drained' = 0
    /\ swept' = FALSE /\ lateDone' = FALSE /\ lateRefused' = FALSE /\ standalones' = 0
    /\ D2R!Age /\ R2D!Age
    /\ UNCHANGED <<nextSeq, sent, highestSent, outbox>>
    /\ UNCHANGED <<highestOpened, opened, highestAccepted, accepted, pending, debt,
                   ghostVars, bitVars>>

D2REnvironment == D2R!Environment /\ UNCHANGED <<senderVars, receiverVars, ghostVars, r2dVars, bitVars>>
R2DEnvironment == R2D!Environment /\ UNCHANGED <<senderVars, receiverVars, ghostVars, d2rVars, bitVars>>

Next ==
    \/ StartPayload
    \/ BurnOne
    \/ Flush
    \/ RecvAck
    \/ RecvAckRepeat
    \/ SendAck
    \/ RecvData
    \/ Sweep
    \/ \E s \in Seqs : Accept(s)
    \/ LatePass
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
    /\ WF_vars(Sweep) /\ WF_vars(\E s \in Seqs : Accept(s)) /\ WF_vars(LatePass)
    /\ SF_vars(Flush)
    /\ SF_vars(SendAck)
    /\ SF_vars(RecvAck \/ RecvAckRepeat)
    /\ SF_vars(RecvData)
    /\ WF_d2rVars(D2R!Age) /\ WF_r2dVars(R2D!Age)
    /\ ([]<> <<Flush>>_vars)   => ([]<> <<RecvData>>_vars)
    /\ ([]<> <<SendAck>>_vars) => ([]<> <<(RecvAck \/ RecvAckRepeat) /\ Head(r2dWire).pkt = Block>>_vars)

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
    /\ highestAccepted \in 0..MaxSeq
    /\ accepted \subseteq Seqs /\ accepted \subseteq AFloor..highestAccepted
    /\ pending \subseteq Seqs
    /\ debt \in BOOLEAN /\ swept \in BOOLEAN /\ lateDone \in BOOLEAN
    /\ lateRefused \in BOOLEAN /\ standalones \in 0..2
    /\ burned \subseteq Seqs
    /\ surfacedCount \in [Seqs -> 0..2]
    /\ openedEver \subseteq Seqs /\ namedEver \subseteq Seqs
    /\ held \subseteq 0..SentRing-1 /\ unacked \subseteq 0..SentRing-1
    /\ bitsChecked \in 0..MaxSeq
    /\ lastExpect \subseteq Seqs /\ lastVisited \subseteq Seqs
    /\ memoValid \in BOOLEAN /\ memoBlock \subseteq Seqs /\ memoHigh \in 0..MaxSeq
    /\ repeatExact \in BOOLEAN
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

\* A block the memo skips leaves exactly the state its full apply would
\* (sdk/rocketnet_ack.cpp, rocketnet_ack_endpoint_process_block). The
\* reversed witness "skipdeferred" breaks it.
RepeatExact == repeatExact

\* An accepted payload is acknowledged in the update that accepted it when
\* none of the receiver own packets carried the block: the update ends with
\* no debt unless a standalone ack already went to the peer this update or
\* the wire had no room (TRANSPORT.md t15.4).  The reversed witness
\* "lateorder" (the order before the fix) leaves it for the next update.
SameUpdateAck == [][Tick => (~debt \/ standalones >= 1 \/ lateRefused)]_vars

\* At most one standalone ack to the peer in one update (the
\* standalone_ack_time stamp).  The reversed witness "nostamp" sends two.
OneStandalonePerUpdate == standalones <= 1

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
