-------------------------- MODULE RNSequencingWrap --------------------------
(***************************************************************************)
(* One sequenced channel's receive side at the code's widths: the 24-bit   *)
(* ordering and sequencing indices compared modulo 2^24 by half range      *)
(* (here Mod and Half = Mod \div 2), against the 32-bit read index         *)
(* ordered_read (rn_channel_receive_t, sdk/rocketnet_allocator.h) and the  *)
(* 24-bit sequenced_watermark.  Each record carries its true ordering and  *)
(* sequencing (naturals); the code sees only their low bits.  The ghosts   *)
(* compare every verdict of the code with the true order.                  *)
(*                                                                         *)
(* The code (sdk/rocketnet_channel_sequenced.cpp):                         *)
(*   rn_channels_dispatch_sequenced  delta = (ordering - ordered_read)     *)
(*       & mask; stale at or past Half; in turn at 0; else the future      *)
(*       hold: gap = (ordering - held ordering) & mask and sequence_gap =  *)
(*       (sequencing - held sequencing) & mask, the newer by ordering then *)
(*       sequencing replaces the hold, an older or equal one is superseded *)
(*   rn_sequenced_in_turn  held_gap = (sequencing - in-turn block's        *)
(*       sequencing) & mask against rx->block_in_turn, then sequence_gap = *)
(*       (sequencing - sequenced_watermark) & mask, stale at or past Half  *)
(*   rn_channels_drain_sequenced  after an ordered delivery: the in-turn   *)
(*       block of the closed epoch is stale; the hold's gap =              *)
(*       (ordering - ordered_read) & mask; a hold in turn is delivered, or *)
(*       an incomplete block moves to block_in_turn                        *)
(* and rn_channels_deliver_ordered / rn_channels_deliver_sequenced         *)
(* (sdk/rocketnet_channel.cpp): read + 1 and watermark 0; the delivery's   *)
(* own masked check, then watermark = (sequencing + 1) & mask.             *)
(*                                                                         *)
(* The honest network (the protocol's half-range assumption): the sender   *)
(* runs fewer than Half ahead of the receiver (an ordered send while       *)
(* ordered_write + 1 - ordered_read <= Half - 1; a sequenced send while its *)
(* index is within Half - 1 of the epoch's watermark, 0 for an epoch not   *)
(* yet open), and a record in flight is never Half or more behind the      *)
(* sender's next index (a send drops from the network what falls out).     *)
(* Under it every compare's operands lie in -Half .. Half - 1 of each      *)
(* other, so the modular verdict equals the true one.                      *)
(*                                                                         *)
(* A block is two fragments of one (ordering, sequencing); the second to   *)
(* arrive completes it (rn_sequenced_block).  The delivery table's room    *)
(* (rn_sequenced_room) is RNOrderingWrap's; here it always has room.       *)
(*                                                                         *)
(* Broken names one real one-line defect at one compare:                   *)
(*   "deltaUnmasked"     dispatch delta without & mask (32-bit arithmetic) *)
(*   "deltaBoundary"     dispatch stale test delta > Half, not >=          *)
(*   "blockGapUnmasked"  in-turn held_gap without & mask                   *)
(*   "watermarkUnmasked" in-turn sequence_gap without & mask               *)
(*   "watermarkBoundary" in-turn stale test sequence_gap > Half, not >=    *)
(*   "holdGapUnmasked"   future-hold gap without & mask                    *)
(*   "drainGapUnmasked"  drain's hold gap without & mask                   *)
(* An unmasked difference is uint32 arithmetic: modulo CounterMod (2^32 in *)
(* the code), a multiple of Mod.                                           *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Mod,          \* the wire modulus of ordering and sequencing (2^24 in the code)
    CounterMod,   \* uint32 arithmetic (2^32 in the code), a multiple of Mod
    Net,          \* records in flight at once
    MaxSeqSends,  \* sequenced messages a behaviour sends
    MaxOrdSends,  \* ordered messages a behaviour sends
    Starts,       \* the read index and sequencing index a behaviour may start from
    Broken        \* "none", or the one compare written wrong (above)

Half == Mod \div 2

\* every true index stays below CounterMod, so ordered_read is the true read index
MaxO == Mod - 1 + MaxOrdSends
MaxS == Mod - 1 + MaxSeqSends

ASSUME /\ Mod >= 4 /\ Mod % 2 = 0
       /\ CounterMod % Mod = 0 /\ CounterMod > Mod /\ CounterMod > MaxO
       /\ Net >= 2 /\ MaxSeqSends >= 0 /\ MaxOrdSends >= 0
       /\ Starts \subseteq 0..Mod-1

U24(x) == x % Mod

\* ( a - b ) & ROCKETNET_MESSAGE_U24_MASK, or the uint32 ( a - b ) with the mask left out
Sub(a, b, site) == IF Broken = site THEN (a - b) % CounterMod ELSE (a - b) % Mod

\* gap >= ROCKETNET_MESSAGE_HALF_RANGE, or gap > ROCKETNET_MESSAGE_HALF_RANGE
Behind(g, site) == IF Broken = site THEN g > Half ELSE g >= Half

NoTurn == [on |-> FALSE, s |-> 0, have |-> 0]
NoHold == [on |-> FALSE, o |-> 0, s |-> 0, blk |-> FALSE, have |-> 0]

Records == [o : 0..MaxO, s : 0..MaxS, k : {"one", "f0", "f1"}]

VARIABLES
    read,        \* rx->ordered_read (32-bit)
    wm,          \* rx->sequenced_watermark (24-bit)
    inTurn,      \* rx->block_in_turn: the in-turn block collecting, its true sequencing
    hold,        \* rx->held_sequenced: the one future state, its true ordering and sequencing
    ow,          \* the sender's ordered_write
    sw,          \* the sender's sequenced_write
    net,         \* records in flight
    wmT,         \* ghost: the true watermark
    lostNewer,   \* ghost: a verdict discarded a state truly newer than the one kept
    outOfOrder,  \* ghost: a sequenced delivery out of its epoch or behind the watermark
    silent,      \* ghost: a record dropped with no count (DISPATCH_NONE)
    seqSent,     \* ghost: sequenced sends so far
    ordSent      \* ghost: ordered sends so far

vars == <<read, wm, inTurn, hold, ow, sw, net, wmT, lostNewer, outOfOrder, silent, seqSent, ordSent>>

TypeOK ==
    /\ read \in 0..MaxO
    /\ wm \in 0..Mod-1
    /\ inTurn \in [on : BOOLEAN, s : 0..MaxS, have : 0..2]
    /\ hold \in [on : BOOLEAN, o : 0..MaxO, s : 0..MaxS, blk : BOOLEAN, have : 0..2]
    /\ ow \in 0..MaxO
    /\ sw \in 0..MaxS
    /\ net \subseteq Records
    /\ wmT \in 0..MaxS + 1
    /\ lostNewer \in BOOLEAN
    /\ outOfOrder \in BOOLEAN
    /\ silent \in BOOLEAN
    /\ seqSent \in 0..MaxSeqSends
    /\ ordSent \in 0..MaxOrdSends

\* a quiescent channel anywhere on the ring: every earlier record delivered
Init ==
    /\ read \in Starts
    /\ ow = read
    /\ sw \in Starts
    /\ wmT = sw
    /\ wm = sw
    /\ inTurn = NoTurn
    /\ hold = NoHold
    /\ net = {}
    /\ lostNewer = FALSE
    /\ outOfOrder = FALSE
    /\ silent = FALSE
    /\ seqSent = 0
    /\ ordSent = 0

\* rn_channels_deliver_sequenced (sdk/rocketnet_channel.cpp): in this epoch and
\* not behind the watermark, both masked
DeliverOK(o, s, rd, w) == U24(o) = U24(rd) /\ U24(U24(s) - w) < Half

\* -------------------------------------------------------------------------
\* rn_sequenced_in_turn: r is in its epoch (delta = 0)
InTurn(r) ==
    LET blk    == r.k # "one"
        hg     == Sub(U24(r.s), U24(inTurn.s), "blockGapUnmasked")      \* held_gap
        vsBlk  == inTurn.on /\ ~(blk /\ hg = 0)
        older  == vsBlk /\ (hg = 0 \/ hg >= Half)
        after  == IF vsBlk THEN NoTurn ELSE inTurn                       \* rn_sequenced_release_in_turn
        dropB  == vsBlk /\ ~older /\ r.s < inTurn.s                      \* a truly newer block released
        wg     == Sub(U24(r.s), wm, "watermarkUnmasked")                 \* sequence_gap
    IN
    IF older
      THEN /\ lostNewer' = (lostNewer \/ r.s > inTurn.s)                 \* SEQUENCED_SUPERSEDED
           /\ UNCHANGED <<wm, wmT, inTurn, hold, outOfOrder, silent>>
    ELSE IF Behind(wg, "watermarkBoundary")
      THEN /\ lostNewer' = (lostNewer \/ dropB \/ r.s >= wmT)            \* SEQUENCED_STALE
           /\ inTurn' = NoTurn
           /\ UNCHANGED <<wm, wmT, hold, outOfOrder, silent>>
    ELSE IF blk /\ ~after.on
      THEN /\ lostNewer' = (lostNewer \/ dropB)                          \* rn_sequenced_block: the anchor
           /\ inTurn' = [on |-> TRUE, s |-> r.s, have |-> 1]
           /\ UNCHANGED <<wm, wmT, hold, outOfOrder, silent>>
    ELSE \* a whole record, or the block's last fragment
         /\ lostNewer' = (lostNewer \/ dropB)
         /\ UNCHANGED hold
         /\ IF DeliverOK(r.o, r.s, read, wm)
              THEN /\ wm' = U24(U24(r.s) + 1)
                   /\ wmT' = r.s + 1
                   /\ inTurn' = NoTurn
                   /\ outOfOrder' = (outOfOrder \/ r.o # read \/ r.s < wmT)
                   /\ UNCHANGED silent
              ELSE /\ silent' = TRUE                                     \* DISPATCH_NONE, nothing counted
                   /\ inTurn' = IF blk THEN [after EXCEPT !.have = 2] ELSE after
                   /\ UNCHANGED <<wm, wmT, outOfOrder>>

\* rn_channels_dispatch_sequenced: r is ahead of its epoch, against the one hold
Future(r) ==
    LET g     == Sub(U24(r.o), U24(hold.o), "holdGapUnmasked")           \* gap
        sg    == U24(U24(r.s) - U24(hold.s))                             \* sequence_gap
        cont  == r.k # "one" /\ hold.on /\ hold.blk /\ g = 0 /\ sg = 0
        older == hold.on /\ (g >= Half \/ (g = 0 /\ (sg >= Half \/ (sg = 0 /\ ~cont))))
        newer == ~hold.on \/ r.o > hold.o \/ (r.o = hold.o /\ r.s > hold.s)
    IN
    /\ UNCHANGED <<wm, wmT, inTurn, outOfOrder, silent>>
    /\ IF older
         THEN /\ lostNewer' = (lostNewer \/ newer)                       \* SEQUENCED_SUPERSEDED
              /\ UNCHANGED hold
       ELSE IF cont
         THEN /\ hold' = [hold EXCEPT !.have = 2]                        \* the block completes, still held
              /\ UNCHANGED lostNewer
       ELSE /\ lostNewer' = (lostNewer \/ ~newer)                        \* rn_sequenced_release_hold
            /\ hold' = [on |-> TRUE, o |-> r.o, s |-> r.s, blk |-> r.k # "one", have |-> 1]

\* one record through rn_channels_dispatch_sequenced
Arrive(r) ==
    /\ r \in net
    /\ net' = net \ {r}
    /\ UNCHANGED <<read, ow, sw, seqSent, ordSent>>
    /\ LET delta == Sub(U24(r.o), read, "deltaUnmasked") IN
       IF Behind(delta, "deltaBoundary")
         THEN /\ lostNewer' = (lostNewer \/ r.o >= read)                 \* SEQUENCED_STALE
              /\ UNCHANGED <<wm, wmT, inTurn, hold, outOfOrder, silent>>
       ELSE IF delta = 0
         THEN InTurn(r)
         ELSE Future(r)

\* an ordered delivery (rn_channels_deliver_ordered: read + 1, watermark 0),
\* then rn_channels_drain_sequenced: the closed epoch's in-turn block is
\* released stale (its ordering is no longer the read index), then the hold
OpenEpoch ==
    /\ read < ow
    /\ read' = read + 1
    /\ UNCHANGED <<ow, sw, net, seqSent, ordSent, silent>>
    /\ LET g == Sub(U24(hold.o), read + 1, "drainGapUnmasked") IN
       IF ~hold.on \/ (g # 0 /\ g < Half)
         THEN /\ wm' = 0 /\ wmT' = 0 /\ inTurn' = NoTurn
              /\ UNCHANGED <<hold, lostNewer, outOfOrder>>
       ELSE IF g # 0
         THEN /\ wm' = 0 /\ wmT' = 0 /\ inTurn' = NoTurn               \* SEQUENCED_STALE
              /\ hold' = NoHold
              /\ lostNewer' = (lostNewer \/ hold.o >= read + 1)
              /\ UNCHANGED outOfOrder
       ELSE IF hold.blk /\ hold.have < 2
         THEN /\ wm' = 0 /\ wmT' = 0                                     \* to block_in_turn
              /\ inTurn' = [on |-> TRUE, s |-> hold.s, have |-> 1]
              /\ hold' = NoHold
              /\ UNCHANGED <<lostNewer, outOfOrder>>
       ELSE IF DeliverOK(hold.o, hold.s, read + 1, 0)
         THEN /\ wm' = U24(U24(hold.s) + 1)
              /\ wmT' = hold.s + 1
              /\ inTurn' = NoTurn
              /\ hold' = NoHold
              /\ outOfOrder' = (outOfOrder \/ hold.o # read + 1)
              /\ UNCHANGED lostNewer
       ELSE /\ wm' = 0 /\ wmT' = 0 /\ inTurn' = NoTurn                 \* refused: SEQUENCED_STALE
            /\ hold' = NoHold
            /\ lostNewer' = TRUE
            /\ UNCHANGED outOfOrder

\* -------------------------------------------------------------------------
\* the sender and the network (environment)

\* the watermark the sender's epoch is measured against
Base == IF ow = read THEN wmT ELSE 0

CanSeq(n) ==
    /\ seqSent < MaxSeqSends
    /\ Cardinality(net) + n <= Net
    /\ sw - Base <= Half - 1

CanOrd ==
    /\ ordSent < MaxOrdSends
    /\ ow + 1 - read <= Half - 1

\* a record of this epoch Half or more behind the next index has left the network
Fresh(S) == {x \in S : ~(x.o = ow /\ x.s + Half <= sw)}

SendOne ==
    /\ CanSeq(1)
    /\ net' = Fresh(net) \cup {[o |-> ow, s |-> sw, k |-> "one"]}
    /\ sw' = sw + 1
    /\ seqSent' = seqSent + 1
    /\ UNCHANGED <<read, wm, inTurn, hold, ow, wmT, lostNewer, outOfOrder, silent, ordSent>>

SendBlock ==
    /\ CanSeq(2)
    /\ net' = Fresh(net) \cup {[o |-> ow, s |-> sw, k |-> "f0"], [o |-> ow, s |-> sw, k |-> "f1"]}
    /\ sw' = sw + 1
    /\ seqSent' = seqSent + 1
    /\ UNCHANGED <<read, wm, inTurn, hold, ow, wmT, lostNewer, outOfOrder, silent, ordSent>>

\* rn_channels_send_ordered: ordered_write + 1, sequenced_write = 0; the ordered
\* record itself is delivered in order by OpenEpoch
SendOrdered ==
    /\ CanOrd
    /\ ow' = ow + 1
    /\ sw' = 0
    /\ net' = {x \in net : x.o + Half >= ow + 1}
    /\ ordSent' = ordSent + 1
    /\ UNCHANGED <<read, wm, inTurn, hold, wmT, lostNewer, outOfOrder, silent, seqSent>>

Lose(r) ==
    /\ r \in net
    /\ net' = net \ {r}
    /\ UNCHANGED <<read, wm, inTurn, hold, ow, sw, wmT, lostNewer, outOfOrder, silent, seqSent, ordSent>>

\* nothing in flight, every ordered record delivered, the sender spent or paced out
Done ==
    /\ net = {}
    /\ read = ow
    /\ ~CanSeq(1)
    /\ ~CanOrd
    /\ UNCHANGED vars

Next ==
    \/ \E r \in net : Arrive(r)
    \/ OpenEpoch
    \/ SendOne
    \/ SendBlock
    \/ SendOrdered
    \/ \E r \in net : Lose(r)
    \/ Done

Spec == Init /\ [][Next]_vars

\* -------------------------------------------------------------------------
\* no stale or superseded verdict discards a state truly newer than the one kept
NewerNeverDiscarded == ~lostNewer

\* every sequenced delivery is in its epoch and at or after the true watermark
SequencedInOrder == ~outOfOrder

\* every record reaching the dispatch is delivered, held or counted
NothingSilent == ~silent

\* the hold is truly ahead of the read index
HoldAhead == hold.on => hold.o > read

\* the in-turn block is never behind the watermark
InTurnBlockFresh == inTurn.on => inTurn.s >= wmT

\* the 24-bit watermark is the true watermark's low bits
WatermarkAligned == wm = U24(wmT)
=============================================================================
