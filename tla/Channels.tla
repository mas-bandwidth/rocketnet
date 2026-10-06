----------------------------- MODULE Channels -----------------------------
(***************************************************************************)
(* rocketnet: one connection's message channels over the lossy link.      *)
(*                                                                         *)
(* Layer 3 of docs/CHANNELS.md (the reliable window, 6.1-6.5) with the     *)
(* per-channel state of layer 4 (7.1-7.2) that the window's own safety     *)
(* argument needs (6.3 step 2 rests on in-order first writes and the       *)
(* drain).  One direction: a sender with N ordering channels, the wire     *)
(* (tla/Link.tla: loss, duplication, reordering, delay), a receiver, and   *)
(* the transport's ack path back to the sender.                            *)
(*                                                                         *)
(* What the transport gives, and the model takes as given (CHANNELS.md 2): *)
(*   P2  an acked packet id means the receiver opened that payload once;   *)
(*       the ack is made at open, before this layer sees the payload.      *)
(*   P4  packet ids never repeat.  The model recycles an id only once it   *)
(*       is nowhere (not on the wire, not pending as an ack, not in the    *)
(*       sent map), so a recycled id is indistinguishable from a fresh one.*)
(*   P5  a missing ack proves nothing: an ack may be lost (the ack history *)
(*       overflowing, a burned sequence), and silence is answered by a     *)
(*       resend, never by a delivery fact.  The loss is decided at open,   *)
(*       since the sender cannot see when it happens.                      *)
(*   The transport's replay window drops a duplicated packet (opened):     *)
(*       this layer dedupes numbers, the transport dedupes packets.        *)
(*   The transport never resends; every resend here is a new packet id    *)
(*       carrying the same message number (I4).                            *)
(*                                                                         *)
(* Reliability is per send, RakNet's eight values (I1, I9).  The           *)
(* application may send only the values in Enabled, {0, 3} in the first   *)
(* version (7.5).  The other six are stubs the state already admits:       *)
(*   - the receipt variants 5, 6, 7 ride the wire as 0, 2, 3 (L2.1) and    *)
(*     dispatch as those codes; the receipt itself is a later card;        *)
(*   - RELIABLE (2) takes a number and is delivered unordered; the         *)
(*     number path is the same as RELIABLE_ORDERED's;                      *)
(*   - the sequenced types 1 and 4 have their per-channel counters here    *)
(*     (seqWrite, seqMark, heldSeq) and the coupling with ordered sends    *)
(*     and deliveries (7.1) runs from the first version, but their         *)
(*     dispatch (the watermark test) is the RNChannel card's and is not    *)
(*     written: Enabled must not contain 1 or 4.                           *)
(*                                                                         *)
(* Time is abstracted away.  The RTO is "some time later": a number may   *)
(* be resent once its last copy is settled (delivered, dropped, its ack    *)
(* arrived or lost), and with SpuriousResend = TRUE at any moment it is    *)
(* unacked, which is every timing the design admits but a far larger       *)
(* state space.  For liveness strong fairness on Resend stands in for a    *)
(* timer that always fires.  "Fair loss" for liveness is the bounded-loss  *)
(* instance (BoundedLoss = TRUE): a behaviour loses at most MaxDrops       *)
(* packets and MaxAckLoss acks.  README-Channels.md says why this rather   *)
(* than Link!EventualDelivery.                                             *)
(*                                                                         *)
(* The witness knobs, each TRUE in the design and FALSE in one reversed    *)
(* witness run (README-Channels.md): PidDedupe (the transport's replay     *)
(* drop), NumDedupe (6.5), CountBound (6.2), ChannelFifo (9.2).            *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    NumChannels,   \* N: the ordering channels are 1..N (num_channels)
    Window,        \* W: numbers the sender may have unacked; bounds the receiver's holds
    QueueCap,      \* Q: messages waiting for their first write (send_queue_messages)
    SentMapCap,    \* P: packet ids the sender remembers (packets_in_flight)
    PidCap,        \* packet ids the model can tell apart: a model-size knob
    NumMsgs,       \* M: the application sends messages 1..M in that order
    Enabled,       \* reliabilities the application may send (7.5)
    BoundedLoss,   \* TRUE: at most MaxDrops drops and MaxAckLoss lost acks per behaviour
    MaxDrops,
    MaxAckLoss,
    PidDedupe,     \* TRUE: the transport drops a packet id it has opened
    NumDedupe,     \* TRUE: the receiver drops a number it has received (6.5)
    CountBound,    \* TRUE: the sender numbers only inside the window (6.2)
    ChannelFifo,   \* TRUE: a channel's first writes follow its ordering (9.2)
    SpuriousResend,\* TRUE: the RTO may fire while a copy or its ack is still in flight
    \* the link (tla/Link.tla)
    Capacity, MaxDelay, Duplication, Reordering, Pattern, BurstStart, BurstLen

Reliabilities == 0..7

\* L2.1: the receipt variants never reach the wire.
WireCode(r)  == CASE r = 5 -> 0 [] r = 6 -> 2 [] r = 7 -> 3 [] OTHER -> r
Reliable(r)  == WireCode(r) \in {2, 3, 4}
Ordered(r)   == WireCode(r) = 3
Sequenced(r) == WireCode(r) \in {1, 4}
Channeled(r) == WireCode(r) \in {1, 3, 4}

ASSUME /\ NumChannels \in Nat \ {0}
       /\ Window \in Nat \ {0}
       /\ QueueCap \in Nat \ {0}
       /\ SentMapCap \in Nat \ {0}
       /\ PidCap \in Nat \ {0}
       /\ NumMsgs \in Nat \ {0}
       /\ Enabled \subseteq {0, 2, 3, 5, 6, 7}   \* sequenced dispatch is not on this card
       /\ BoundedLoss \in BOOLEAN /\ MaxDrops \in Nat /\ MaxAckLoss \in Nat
       /\ PidDedupe \in BOOLEAN /\ NumDedupe \in BOOLEAN
       /\ CountBound \in BOOLEAN /\ ChannelFifo \in BOOLEAN
       /\ SpuriousResend \in BOOLEAN

Msgs    == 1..NumMsgs
Chans   == 1..NumChannels
Pids    == 1..PidCap
Nums    == 1..NumMsgs            \* a message takes at most one number

\* What the wire carries: the packet id and the message.  The record's
\* code, channel, ordering index and number are read from desc and numOf,
\* which never change once the message is sent, so a copy on the wire
\* carries exactly the values the sender wrote into it.
Packets == [id : Pids, msg : Msgs]

VARIABLES
    \* the sender (6.1, 7.1, 9.1)
    nextMsg,     \* the next message the application will send
    desc,        \* per message: reliability, channel, ordering and sequencing index
    uq,          \* the unchanneled send queue (codes 0 and 2), FIFO
    cq,          \* per channel: the send queue, FIFO (9.2)
    ordWrite,    \* per channel: next ordering index (ordered_write)
    seqWrite,    \* per channel: next sequencing index (sequenced_write); stub
    numOf,       \* per message: its reliable number, 0 while unnumbered (6.2)
    nextNum,     \* the next reliable number
    acked,       \* numbers the sender has seen acked
    sentMap,     \* the sent-packet map: FIFO of [id, num], at most SentMapCap (6.4)
    \* the wire
    wire, lsent, pulse,
    drops,       \* drops so far, counted only under BoundedLoss
    \* the ack path: ids the receiver opened that the sender has not yet seen
    ackPending,
    ackLost,     \* acks lost so far, counted only under BoundedLoss
    \* the receiver (6.1, 6.5, 7.1, 7.2)
    opened,      \* opened packet ids with a copy still on the wire (its replay window)
    R,           \* the lowest number not yet received
    rcvd,        \* received numbers at or above R (the bitset)
    ordRead,     \* per channel: next ordering index to deliver (ordered_read)
    seqMark,     \* per channel: sequenced watermark; stub
    held,        \* per channel: ordered messages waiting for a predecessor
    heldSeq,     \* per channel: the newest held sequenced message, 0 if none; stub
    dlog,        \* per channel: ordered messages delivered, in delivery order
    delivered,   \* every message delivered, any type
    \* ghosts: a flag that goes TRUE when the design's claim is broken
    dup,         \* a message was delivered twice
    outside,     \* a number arrived outside the receiver's window (6.5 violation)
    behind       \* a non-duplicate ordered record was behind ordered_read (7.2 violation)

senderVars == <<nextMsg, desc, uq, cq, ordWrite, seqWrite, numOf, nextNum, acked, sentMap>>
linkVars   == <<wire, lsent, pulse>>
ackVars    == <<ackPending, ackLost>>
rcvVars    == <<opened, R, rcvd, ordRead, seqMark, held, heldSeq, dlog, delivered, dup, outside, behind>>
vars       == <<senderVars, linkVars, drops, ackVars, rcvVars>>

L == INSTANCE Link WITH sent <- lsent

(***************************************************************************)
(* Helpers.                                                                *)
(***************************************************************************)

NoDesc == [rel |-> 0, chan |-> 1, ord |-> 0, seq |-> 0]

Remove(s, i) == [j \in 1..Len(s)-1 |-> IF j < i THEN s[j] ELSE s[j+1]]
Range(s)     == {s[i] : i \in DOMAIN s}

RECURSIVE SumLen(_)
SumLen(c) == IF c = 0 THEN 0 ELSE Len(cq[c]) + SumLen(c - 1)
Queued    == Len(uq) + SumLen(NumChannels)

Unacked == {n \in 1..nextNum-1 : n \notin acked}
Base    == IF Unacked = {} THEN nextNum ELSE CHOOSE n \in Unacked : \A k \in Unacked : n <= k

\* 6.2, the count bound: n may be given only while n < U' + W.  (Blocks,
\* which make U' differ from U, are the RNBlocks card's.)
MayNumber == ~CountBound \/ nextNum < Base + Window

MapPids  == {sentMap[i].id : i \in DOMAIN sentMap}
OnWire   == {wire[i].pkt.id : i \in DOMAIN wire}
FreePids == Pids \ (OnWire \cup ackPending \cup MapPids)
NextPid  == CHOOSE p \in FreePids : \A q \in FreePids : p <= q

MsgOf(n) == CHOOSE m \in Msgs : numOf[m] = n

\* The replay window matters only for an id a copy of which may still
\* arrive, so opened is kept to the ids on the wire.
Prune(S, w) == {q \in S : \E i \in DOMAIN w : w[i].pkt.id = q}

\* 6.4: the sent map evicts its oldest entry when full.
Commit(e) == IF Len(sentMap) < SentMapCap THEN Append(sentMap, e) ELSE Append(Tail(sentMap), e)

\* The receiver's R advances past every received number (6.5).
RECURSIVE Advance(_, _)
Advance(S, r) == IF r \in S THEN Advance(S, r + 1) ELSE r

\* The drain (7.2): the held messages with ordering o, o+1, ..., in order.
RECURSIVE Chain(_, _)
Chain(o, H) ==
    LET A == {h \in H : desc[h].ord = o}
    IN IF A = {} THEN <<>>
       ELSE LET h == CHOOSE x \in A : TRUE IN <<h>> \o Chain(o + 1, H \ {h})

Init ==
    /\ nextMsg = 1
    /\ desc = [m \in Msgs |-> NoDesc]
    /\ uq = <<>>
    /\ cq = [c \in Chans |-> <<>>]
    /\ ordWrite = [c \in Chans |-> 0]
    /\ seqWrite = [c \in Chans |-> 0]
    /\ numOf = [m \in Msgs |-> 0]
    /\ nextNum = 1
    /\ acked = {}
    /\ sentMap = <<>>
    /\ L!Init
    /\ drops = 0
    /\ ackPending = {}
    /\ ackLost = 0
    /\ opened = {}
    /\ R = 1
    /\ rcvd = {}
    /\ ordRead = [c \in Chans |-> 0]
    /\ seqMark = [c \in Chans |-> 0]
    /\ held = [c \in Chans |-> {}]
    /\ heldSeq = [c \in Chans |-> 0]
    /\ dlog = [c \in Chans |-> <<>>]
    /\ delivered = {}
    /\ dup = FALSE
    /\ outside = FALSE
    /\ behind = FALSE

(***************************************************************************)
(* The application: send_message with a reliability and a channel (R2).    *)
(* Refused (disabled) when the queue is full (R1) or the type is not      *)
(* enabled (7.5).  The ordering and sequencing indices are taken here, at  *)
(* send, with RakNet's coupling (7.1); the number is not (6.2).            *)
(***************************************************************************)
AppSend(r, c) ==
    /\ nextMsg <= NumMsgs
    /\ r \in Enabled
    /\ Channeled(r) \/ c = 1        \* the channel is ignored for codes 0 and 2
    /\ Queued < QueueCap
    /\ LET m == nextMsg IN
       /\ nextMsg' = m + 1
       /\ IF Ordered(r) THEN
             /\ desc' = [desc EXCEPT ![m] = [rel |-> r, chan |-> c, ord |-> ordWrite[c], seq |-> 0]]
             /\ ordWrite' = [ordWrite EXCEPT ![c] = @ + 1]
             /\ seqWrite' = [seqWrite EXCEPT ![c] = 0]
             /\ cq' = [cq EXCEPT ![c] = Append(@, m)]
             /\ UNCHANGED uq
          ELSE IF Sequenced(r) THEN      \* stub: unreachable while Enabled excludes 1 and 4
             /\ desc' = [desc EXCEPT ![m] = [rel |-> r, chan |-> c, ord |-> ordWrite[c], seq |-> seqWrite[c]]]
             /\ seqWrite' = [seqWrite EXCEPT ![c] = @ + 1]
             /\ cq' = [cq EXCEPT ![c] = Append(@, m)]
             /\ UNCHANGED <<uq, ordWrite>>
          ELSE
             /\ desc' = [desc EXCEPT ![m] = [rel |-> r, chan |-> 1, ord |-> 0, seq |-> 0]]
             /\ uq' = Append(uq, m)
             /\ UNCHANGED <<cq, ordWrite, seqWrite>>
    /\ UNCHANGED <<numOf, nextNum, acked, sentMap, linkVars, drops, ackVars, rcvVars>>

(***************************************************************************)
(* Writing a packet.  One record per packet in this model (aggregation is  *)
(* the RNScheduler card's); each write takes a fresh packet id (P4 above:  *)
(* a free id has no copy on the wire, so it is not in opened).             *)
(***************************************************************************)
Write(m, p) == L!Send([id |-> p, msg |-> m])

\* The head of the unchanneled queue goes out.  Code 0 takes no number and
\* no map entry (6.4: a packet carrying only unreliable records is not in
\* the map).  Code 2, a stub, takes a number as an ordered record does.
FirstWriteUnchanneled ==
    /\ uq # <<>>
    /\ FreePids # {}
    /\ LET m == Head(uq)
           p == NextPid
       IN /\ uq' = Tail(uq)
          /\ Write(m, p)
          /\ IF Reliable(desc[m].rel)
                THEN /\ MayNumber
                     /\ numOf' = [numOf EXCEPT ![m] = nextNum]
                     /\ nextNum' = nextNum + 1
                     /\ sentMap' = Commit([id |-> p, num |-> nextNum])
                ELSE UNCHANGED <<numOf, nextNum, sentMap>>
    /\ UNCHANGED <<nextMsg, desc, cq, ordWrite, seqWrite, acked, drops, ackVars, rcvVars>>

\* A queued ordered record on channel c is first written: it takes the next
\* number (6.2) and a map entry (6.4).  With ChannelFifo the head goes
\* first, which is what 6.3 step 2 and 9.2 require.
FirstWriteOrdered(c) ==
    /\ cq[c] # <<>>
    /\ FreePids # {}
    /\ MayNumber
    /\ \E i \in DOMAIN cq[c] :
         /\ ChannelFifo => i = 1
         /\ LET m == cq[c][i]
                p == NextPid
                n == nextNum
            IN /\ cq' = [cq EXCEPT ![c] = Remove(@, i)]
               /\ numOf' = [numOf EXCEPT ![m] = n]
               /\ nextNum' = n + 1
               /\ sentMap' = Commit([id |-> p, num |-> n])
               /\ Write(m, p)
    /\ UNCHANGED <<nextMsg, desc, uq, ordWrite, seqWrite, acked, drops, ackVars, rcvVars>>

\* A copy of n the receiver has not opened is still on the wire, or its ack
\* is still on the way back.  A copy whose id is already opened is a wire
\* duplicate the transport will drop; it does not hold the timer (TLC found
\* the lasso: see README-Channels.md).
InFlight(n) ==
    \/ \E i \in DOMAIN wire : numOf[wire[i].pkt.msg] = n /\ wire[i].pkt.id \notin opened
    \/ \E i \in DOMAIN sentMap : sentMap[i].num = n /\ sentMap[i].id \in ackPending

\* An unacked number is resent: a new packet id, the same number (I4), a new
\* map entry.  The RTO has fired: after the last copy is settled, or (with
\* SpuriousResend) at any time the number is unacked.
Resend(n) ==
    /\ n \in Unacked
    /\ SpuriousResend \/ ~InFlight(n)
    /\ FreePids # {}
    /\ LET p == NextPid IN
       /\ sentMap' = Commit([id |-> p, num |-> n])
       /\ Write(MsgOf(n), p)
    /\ UNCHANGED <<nextMsg, desc, uq, cq, ordWrite, seqWrite, numOf, nextNum, acked, drops, ackVars, rcvVars>>

(***************************************************************************)
(* The receiver.  The transport opens the packet, acks it (P2), and this   *)
(* layer dispatches the record (7.2).                                      *)
(***************************************************************************)

DeliverNow(S) ==
    /\ delivered' = delivered \cup S
    /\ dup' = (dup \/ (S \cap delivered # {}))

OrderedDispatch(m) ==
    LET c == desc[m].chan
        o == desc[m].ord
    IN IF o = ordRead[c] THEN
          LET chain == Chain(o + 1, held[c])
          IN /\ DeliverNow({m} \cup Range(chain))
             /\ dlog' = [dlog EXCEPT ![c] = @ \o <<m>> \o chain]
             /\ held' = [held EXCEPT ![c] = @ \ Range(chain)]
             /\ ordRead' = [ordRead EXCEPT ![c] = o + 1 + Len(chain)]
             /\ seqMark' = [seqMark EXCEPT ![c] = 0]
             /\ UNCHANGED <<heldSeq, outside, behind>>
       ELSE IF o > ordRead[c] THEN
          /\ held' = [held EXCEPT ![c] = @ \cup {m}]
          /\ UNCHANGED <<ordRead, seqMark, heldSeq, dlog, delivered, dup, outside, behind>>
       ELSE
          /\ behind' = TRUE
          /\ UNCHANGED <<ordRead, seqMark, held, heldSeq, dlog, delivered, dup, outside>>

ReliableDispatch(m, code) ==
    LET n == numOf[m] IN
    IF NumDedupe /\ (n < R \/ n \in rcvd) THEN
       \* a duplicate: dropped and counted (MESSAGE_DUPLICATE)
       UNCHANGED <<R, rcvd, ordRead, seqMark, held, heldSeq, dlog, delivered, dup, outside, behind>>
    ELSE IF n >= R + Window THEN
       \* a violation (MESSAGE_NUMBER_OUTSIDE_WINDOW); a correct sender never does this
       /\ outside' = TRUE
       /\ UNCHANGED <<R, rcvd, ordRead, seqMark, held, heldSeq, dlog, delivered, dup, behind>>
    ELSE
       /\ LET S  == rcvd \cup {n}
              R2 == Advance(S, R)
          IN /\ R' = R2
             /\ rcvd' = {k \in S : k >= R2}
       /\ IF code = 2
             THEN /\ DeliverNow({m})
                  /\ UNCHANGED <<ordRead, seqMark, held, heldSeq, dlog, outside, behind>>
             ELSE OrderedDispatch(m)

Dispatch(m) ==
    LET code == WireCode(desc[m].rel) IN
    CASE code = 0 ->
            /\ DeliverNow({m})
            /\ UNCHANGED <<R, rcvd, ordRead, seqMark, held, heldSeq, dlog, outside, behind>>
      [] code \in {2, 3} -> ReliableDispatch(m, code)
      [] OTHER -> FALSE      \* codes 1 and 4: the RNChannel card writes this case

Receive(p) ==
    /\ L!Deliver(p)
    /\ IF PidDedupe /\ p.id \in opened THEN
          \* the transport's replay window drops it; nothing reaches this layer
          /\ opened' = Prune(opened, Tail(wire))
          /\ UNCHANGED <<ackVars, R, rcvd, ordRead, seqMark, held, heldSeq, dlog, delivered, dup, outside, behind>>
       ELSE
          /\ opened' = Prune(opened \cup {p.id}, Tail(wire))
          /\ \/ \* the ack reaches the sender, some time later
                /\ ackPending' = ackPending \cup {p.id}
                /\ UNCHANGED ackLost
             \/ \* P5: the ack never reaches the sender
                /\ BoundedLoss => ackLost < MaxAckLoss
                /\ ackLost' = IF BoundedLoss THEN ackLost + 1 ELSE ackLost
                /\ UNCHANGED ackPending
          /\ Dispatch(p.msg)
    /\ UNCHANGED <<senderVars, drops>>

(***************************************************************************)
(* The ack path (6.4).  An acked id the map still holds acks its number;   *)
(* an id the map evicted, or never held, is ignored.                       *)
(***************************************************************************)
AckArrives(p) ==
    /\ p \in ackPending
    /\ ackPending' = ackPending \ {p}
    /\ LET refs == {sentMap[i].num : i \in {j \in DOMAIN sentMap : sentMap[j].id = p}}
       IN /\ acked' = acked \cup refs
          /\ sentMap' = SelectSeq(sentMap, LAMBDA e : e.id # p)
    /\ UNCHANGED <<nextMsg, desc, uq, cq, ordWrite, seqWrite, numOf, nextNum, linkVars, drops, ackLost, rcvVars>>

(***************************************************************************)
(* The wire on its own.  Link!Environment, with Drop counted so that the   *)
(* liveness instance can bound it.                                         *)
(***************************************************************************)
Environment ==
    /\ \/ \E i \in DOMAIN wire :
             \/ /\ L!Drop(i)
                /\ BoundedLoss => drops < MaxDrops
                /\ drops' = IF BoundedLoss THEN drops + 1 ELSE drops
                /\ opened' = Prune(opened, L!Remove(wire, i))
             \/ /\ L!Duplicate(i) \/ L!Reorder(i) \/ L!Delay(i)
                /\ UNCHANGED <<drops, opened>>
       \/ /\ MaxDelay > 0
          /\ L!Age
          /\ UNCHANGED <<drops, opened>>
    /\ UNCHANGED <<senderVars, ackVars, R, rcvd, ordRead, seqMark, held, heldSeq, dlog, delivered, dup, outside, behind>>

Next ==
    \/ \E r \in Reliabilities, c \in Chans : AppSend(r, c)
    \/ FirstWriteUnchanneled
    \/ \E c \in Chans : FirstWriteOrdered(c)
    \/ \E n \in Nums : Resend(n)
    \/ \E p \in Packets : Receive(p)
    \/ \E p \in Pids : AckArrives(p)
    \/ Environment

(***************************************************************************)
(* Fairness, for liveness only.  The sender's writes and resends always   *)
(* get their turn (strong, because a free packet id and an open window    *)
(* come and go); an ack that is pending again and again arrives; the wire  *)
(* delivers a due head and time passes (Link!WireFair's two clauses).     *)
(* Each is one condition on an existential, not one per message, packet   *)
(* or id: TLC expands a conjunction of k fairness conditions into 2^k     *)
(* branches, and Link!WireFair alone is one per packet value.  With a     *)
(* finite set of messages the existential is enough: once the others are  *)
(* done, the starved one is the only one enabled.  Loss is bounded by the  *)
(* instance, not by a fairness formula: see README-Channels.md.            *)
(***************************************************************************)
AnyFirstWrite == FirstWriteUnchanneled \/ \E c \in Chans : FirstWriteOrdered(c)
AnyResend     == \E n \in Nums : Resend(n)
AnyAck        == \E p \in Pids : AckArrives(p)
AnyReceive    == \E p \in Packets : Receive(p)
Tick          == /\ MaxDelay > 0
                 /\ L!Age
                 /\ UNCHANGED <<senderVars, drops, ackVars, rcvVars>>

Fairness ==
    /\ SF_vars(AnyFirstWrite)
    /\ SF_vars(AnyResend)
    /\ SF_vars(AnyAck)
    /\ SF_vars(AnyReceive)
    /\ WF_vars(Tick)

(***************************************************************************)
(* Safety.                                                                 *)
(***************************************************************************)
TypeOK ==
    /\ nextMsg \in 1..NumMsgs+1
    /\ desc \in [Msgs -> [rel : Reliabilities, chan : Chans, ord : 0..NumMsgs, seq : 0..NumMsgs]]
    /\ uq \in Seq(Msgs)
    /\ cq \in [Chans -> Seq(Msgs)]
    /\ ordWrite \in [Chans -> 0..NumMsgs]
    /\ seqWrite \in [Chans -> 0..NumMsgs]
    /\ numOf \in [Msgs -> 0..NumMsgs]
    /\ nextNum \in 1..NumMsgs+1
    /\ acked \subseteq Nums
    /\ sentMap \in Seq([id : Pids, num : Nums])
    /\ L!TypeOK
    /\ drops \in 0..MaxDrops
    /\ ackPending \subseteq Pids
    /\ ackLost \in 0..MaxAckLoss
    /\ opened \subseteq Pids
    /\ R \in 1..NumMsgs+1
    /\ rcvd \subseteq Nums
    /\ ordRead \in [Chans -> 0..NumMsgs]
    /\ seqMark \in [Chans -> 0..NumMsgs]
    /\ held \in [Chans -> SUBSET Msgs]
    /\ heldSeq \in [Chans -> 0..NumMsgs]
    /\ dlog \in [Chans -> Seq(Msgs)]
    /\ delivered \subseteq Msgs
    /\ dup \in BOOLEAN /\ outside \in BOOLEAN /\ behind \in BOOLEAN

\* No message is delivered twice (reliable: 6.5; unreliable: the transport's
\* replay window).
NoDoubleDelivery == ~dup

\* On every channel the ordered messages are delivered in send order: the
\* i-th delivery on c is the message sent with ordering index i-1.
OrderedInSendOrder ==
    \A c \in Chans : \A i \in DOMAIN dlog[c] :
        /\ desc[dlog[c][i]].ord = i - 1
        /\ desc[dlog[c][i]].chan = c

\* A correct sender never trips the receiver's checks (6.5, 7.2).
ReceiverNeverOutsideWindow == ~outside
OrderedNeverBehind == ~behind

\* The allocator's promise (R1, 6.3): nothing grows past what was sized at
\* create.
QueueBounded   == Queued <= QueueCap
WindowBounded  == nextNum - Base <= Window
SentMapBounded == Len(sentMap) <= SentMapCap
HeldBounded    == Cardinality(UNION {held[c] : c \in Chans}) <= Window
NoSlotCollision ==
    \A c1, c2 \in Chans : \A m1 \in held[c1], m2 \in held[c2] :
        m1 # m2 => numOf[m1] % Window # numOf[m2] % Window
ReceivedInWindow == \A n \in rcvd : n >= R /\ n < R + Window
AllocatorPromise ==
    /\ QueueBounded /\ WindowBounded /\ SentMapBounded
    /\ HeldBounded /\ NoSlotCollision /\ ReceivedInWindow

\* Sanity.
OnlyEnabledOnWire == \A i \in DOMAIN wire : desc[wire[i].pkt.msg].rel \in Enabled
UnreliableNeverNumbered == \A m \in Msgs : ~Reliable(desc[m].rel) => numOf[m] = 0
DeliveredWereSent == delivered \subseteq 1..nextMsg-1
HeldAreWaiting ==
    \A c \in Chans : \A m \in held[c] :
        Ordered(desc[m].rel) /\ desc[m].chan = c /\ desc[m].ord > ordRead[c]
OpenedOnWire == opened \subseteq OnWire
MapMatchesWire ==
    \A i \in DOMAIN sentMap : \A j \in DOMAIN wire :
        wire[j].pkt.id = sentMap[i].id => numOf[wire[j].pkt.msg] = sentMap[i].num

(***************************************************************************)
(* Liveness.                                                               *)
(***************************************************************************)
ReliableSent(m) == m < nextMsg /\ Reliable(desc[m].rel)

\* Every reliable message the application sent is delivered ...
EveryReliableDelivered == \A m \in Msgs : ReliableSent(m) ~> (m \in delivered)

\* ... and the sender learns it (the window reopens).
EveryReliableAcked == \A m \in Msgs : ReliableSent(m) ~> (numOf[m] \in acked)

=============================================================================
