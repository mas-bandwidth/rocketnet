------------------------------ MODULE RNBlocks ------------------------------
(***************************************************************************)
(* rocketnet: layer 5, blocks (docs/CHANNELS.md section 8; section 15, the *)
(* RNBlocks row).  A reliable ordered message whose record would not fit   *)
(* one packet is a block: split at the fixed stride F, numbered at once    *)
(* with k consecutive reliable numbers, placed in one contiguous ring span *)
(* with fragment i at p + i*F, and delivered whole, at one ordering slot,  *)
(* once every fragment is in.                                              *)
(*                                                                         *)
(* One sender and one receiver over one instance of Link (loss,            *)
(* duplication, reorder).  Units are 16 bytes: F, B and every ring offset  *)
(* are in units, as the record's ring field is.                            *)
(*                                                                         *)
(* The sender.  Messages 1..M wait in the channel's FIFO in order; message *)
(* m has Units[m] units.  NumberMessage takes the FIFO head: a message of  *)
(* at most Small units is one record (one number); a larger one is split   *)
(* into k = ceil(Units[m] / F) fragments, fragments 0..k-2 of F units and  *)
(* the last of the rest, and takes the numbers next..next+k-1 at once and  *)
(* one span [p, p + (k-1)*F + last) under one check of both bounds of 6.2  *)
(* (count: next + k <= U' + W; bytes: the span inside B from the floor).   *)
(* p is ring_next, or the next multiple of B when the span would cross one *)
(* (rn_window_send_position: the skipped units are padding), so a span     *)
(* never wraps the ring; ring offsets are absolute units modulo B.         *)
(* Every record of message m carries ordering index m-1: a block takes one *)
(* ordering slot.  Fragment i of message m carries the tag <<m, i>>, which *)
(* stands for its bytes.  Acks are not modelled (RNWindow checks them and  *)
(* the U' rule; RNRing checks ring reuse), so nothing is acked here: U' is *)
(* the first number and the floor is the first span's start.  The         *)
(* instance starts with the window empty and ring_next at RingStart, as    *)
(* earlier traffic, all acked, left it; RingStart > 0 is what reaches the  *)
(* padding branch and the wrap of offsets past B (MCRNBlocksWrap.cfg).     *)
(* The RTO is abstract: any numbered record may be resent at any time.     *)
(*                                                                         *)
(* The receiver.  A record passes layer 3's duplicate check (a number      *)
(* already received writes nothing, 6.5).  A single record is copied to   *)
(* its ring offset.  A fragment opens or finds its block's anchor by its   *)
(* ordering index, the code's held key (channel, ordering) (no start       *)
(* packet: any fragment opens the state).  The fragment is refused, a      *)
(* protocol violation that ends the connection, when its index is not     *)
(* below its count, when its block's span base = ring - index*F lies      *)
(* outside the ring (rn_block_shape_valid and check 8 in                   *)
(* rn_channels_dispatch_chunk), or when its count, first number n - index  *)
(* or base disagrees with the anchor's (rn_channels_block_collect,         *)
(* MESSAGE_BAD_BLOCK).  Otherwise it is copied to ring + index*F (its own  *)
(* ring offset) and counts in the anchor.  When the chunks in reach the   *)
(* arriving chunk's count k the block is complete (rn_channels_block_      *)
(* settle); the message whose ordering index is the channel's read index   *)
(* is delivered when complete, as a view of its span read at stride F, and *)
(* the read index moves by one.                                            *)
(*                                                                         *)
(* A sender that lies.  With Forge, the outside world may put one record  *)
(* on the wire that copies a numbered record but claims another count     *)
(* (2..k of the largest block): what a remote endpoint whose chunk        *)
(* belongs to something else sends.  At most one is forged, since a       *)
(* sender that lies the same way about every chunk is only sending a      *)
(* different message.  The held                                           *)
(* index's own refusal (an insert past W entries) is RNHeldOwnership's     *)
(* Hold; nothing here fills the index.                                     *)
(*                                                                         *)
(* The reversed witnesses are constants and switches:                      *)
(*   PlaceByIndex = FALSE   a fragment is copied to base + have*F, the     *)
(*                          next free place in arrival order: fragments    *)
(*                          concatenated in the order they arrived.        *)
(*                          TLC must fail DeliveredEqualsSent.             *)
(*   Pad <- NoPad           the sender never pads: a span that crosses a   *)
(*                          multiple of B wraps the ring.  TLC must fail   *)
(*                          SpanInsideRing.                                *)
(*   AnchorCheck <- AnchorCheckOff with Forge: the receiver never compares *)
(*                          a chunk with its anchor, so a forged count     *)
(*                          that arrives after a correct chunk completes   *)
(*                          the block early.  TLC must fail                *)
(*                          NoPartialDelivery.                             *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    W,            \* the window: numbers in flight
    F,            \* the fragment stride, in units of 16 bytes
    B,            \* the receive ring, in units
    Small,        \* the largest message that is one record, in units
    Units,        \* Units[m]: message m's length in units, m \in 1..Len(Units)
    PlaceByIndex, \* BOOLEAN: a fragment is copied to ring + index*F (8)
    FwdPattern,   \* Link loss pattern, sender to receiver
    Capacity, MaxDelay, Duplication, Reordering, BurstStart, BurstLen

\* Switches a cfg overrides by definition (X <- XOther), so the committed
\* cfgs need no new constants.
RingStart      == 0        \* ring_next when the instance starts, window empty
Pad            == TRUE     \* rn_window_send_position pads a crossing span
NoPad          == FALSE
AnchorCheck    == TRUE     \* rn_channels_block_collect compares with the anchor
AnchorCheckOff == FALSE
Forge          == FALSE    \* the outside world may forge one record
ForgeOn        == TRUE

M == Len(Units)
Msgs == 1..M

CeilDiv(a, b) == (a + b - 1) \div b

\* k, the records message m takes: one, or one per fragment (8, "Split")
Count(m) == IF Units[m] <= Small THEN 1 ELSE CeilDiv(Units[m], F)

\* fragment i's units: F, except the last, which holds the rest
FragUnits(m, i) == IF Count(m) = 1 THEN Units[m]
                   ELSE IF i < Count(m) - 1 THEN F
                   ELSE Units[m] - (Count(m) - 1) * F

\* the span message m takes: (k-1)*F plus the last fragment
SpanUnits(m) == IF Count(m) = 1 THEN Units[m] ELSE (Count(m) - 1) * F + FragUnits(m, Count(m) - 1)

RECURSIVE SumCounts(_)
SumCounts(n) == IF n = 0 THEN 0 ELSE Count(n) + SumCounts(n - 1)

NumCount == SumCounts(M)
Nums     == 0..NumCount-1
MaxK     == CHOOSE k \in {Count(m) : m \in Msgs} : \A m \in Msgs : Count(m) <= k
Tags     == Msgs \X (0..MaxK-1)
Ords     == 0..M-1

\* what message m's bytes are, fragment by fragment
Sent(m) == [j \in 1..Count(m) |-> <<m, j - 1>>]

ASSUME /\ W \in Nat \ {0}
       /\ F \in Nat \ {0}
       /\ B \in Nat \ {0}
       /\ Small \in Nat \ {0}
       /\ M \in Nat \ {0}
       /\ \A m \in Msgs : Units[m] \in 1..B /\ Count(m) <= W
       /\ PlaceByIndex \in BOOLEAN
       /\ RingStart \in 0..B-1
       /\ Pad \in BOOLEAN /\ AnchorCheck \in BOOLEAN /\ Forge \in BOOLEAN

\* a record on the wire: its number, ordering index, fragment index, count,
\* ring offset (units, modulo B) and the tag standing for its bytes
Records == [num : Nums, ord : Ords, idx : 0..MaxK-1, cnt : 1..MaxK,
            ring : 0..B-1, tag : Tags]

None == <<0, 0>>   \* an empty ring unit (no message is 0)

VARIABLES
    \* the sender
    nextMsg,   \* the FIFO head: the next message to number
    next,      \* the next reliable number
    ringNext,  \* ring_next, absolute units
    low,       \* the absolute ring start of number 0 (the floor, U' = 0)
    rec,       \* rec[n]: the record numbered n, for n < next
    \* the receiver
    rcvd,      \* the numbers received (layer 3's bitset, never cleared here)
    anchor,    \* anchor[o]: the block at ordering index o: have, cnt, first, base
    ring,      \* the receive ring, unit -> tag or None
    ordRead,   \* the channel's ordered read index
    violated,  \* the receiver refused a chunk: the connection ended
    \* ghosts
    log,       \* the deliveries, in order: [ord, data]
    forged,    \* the one forged record has been put on the wire
    \* the link
    fwdWire, fwdSent, fwdPulse

vars == <<nextMsg, next, ringNext, low, rec, rcvd, anchor, ring, ordRead,
          violated, log, forged, fwdWire, fwdSent, fwdPulse>>

senderVars   == <<nextMsg, next, ringNext, low, rec>>
receiverVars == <<rcvd, anchor, ring, ordRead, violated, log>>
fwdVars      == <<fwdWire, fwdSent, fwdPulse>>

Fwd == INSTANCE Link WITH wire <- fwdWire, sent <- fwdSent, pulse <- fwdPulse,
                          Packets <- Records, Pattern <- FwdPattern

NoAnchor == [have |-> 0, cnt |-> 0, first |-> 0, base |-> 0, done |-> FALSE]

Init ==
    /\ nextMsg = 1 /\ next = 0 /\ ringNext = RingStart /\ low = RingStart
    /\ rec = [n \in {} |-> 0]
    /\ rcvd = {}
    /\ anchor = [o \in Ords |-> NoAnchor]
    /\ ring = [u \in 0..B-1 |-> None]
    /\ ordRead = 0
    /\ violated = FALSE
    /\ log = <<>>
    /\ forged = FALSE
    /\ Fwd!Init

(***************************************************************************)
(* The sender.                                                             *)
(***************************************************************************)

\* 6.2's placement (rn_window_send_position): ring_next, or the next
\* multiple of B when the span would cross one (the skipped units are
\* padding).  Pad <- NoPad is the witness that never pads.
Pos(l) == IF Pad /\ (ringNext \div B) # ((ringNext + l - 1) \div B)
          THEN ((ringNext \div B) + 1) * B
          ELSE ringNext

\* nothing is acked here, so U' is the first number and the floor is the
\* first span's absolute start (rn_window_send_fits: the ring_start of the
\* record U' names, or p while nothing is in flight)
Floor(p) == IF next = 0 THEN p ELSE low

\* The FIFO head is numbered (rn_ordered_write): a single record takes one
\* number; a block takes k consecutive numbers and one contiguous span,
\* fragment i at p + i*F, both bounds checked once for the whole block
\* (section 8, "Numbering").  While it cannot be admitted, numbering stops.
NumberMessage ==
    /\ nextMsg <= M
    /\ LET m == nextMsg
           k == Count(m)
           l == SpanUnits(m)
           p == Pos(l)
       IN /\ next + k <= W                      \* count bound, U' = 0
          /\ p + l - Floor(p) <= B              \* byte bound
          /\ rec' = [n \in 0..next + k - 1 |->
                       IF n >= next
                       THEN [num |-> n, ord |-> m - 1, idx |-> n - next, cnt |-> k,
                             ring |-> (p + (n - next) * F) % B, tag |-> <<m, n - next>>]
                       ELSE rec[n]]
          /\ next' = next + k
          /\ ringNext' = p + l
          /\ low' = IF next = 0 THEN p ELSE low
          /\ nextMsg' = m + 1
    /\ UNCHANGED <<receiverVars, forged, fwdVars>>

\* A numbered record is written into a packet: a first send or a resend,
\* each fragment on its own (8, "First sends and resends").
SendRecord(n) ==
    /\ n \in 0..next-1
    /\ Fwd!Send(rec[n])
    /\ UNCHANGED <<senderVars, receiverVars, forged>>

(***************************************************************************)
(* The outside world: a sender that lies about one chunk's count.         *)
(***************************************************************************)
ForgeRecord(n, c) ==
    /\ Forge /\ ~forged
    /\ n \in 0..next-1
    /\ c # rec[n].cnt /\ rec[n].idx < c
    /\ Fwd!Send([rec[n] EXCEPT !.cnt = c])
    /\ forged' = TRUE
    /\ UNCHANGED <<senderVars, receiverVars>>

(***************************************************************************)
(* The receiver.                                                           *)
(***************************************************************************)

\* the span base a chunk names: its ring offset less index * F, without a
\* modulo, as rn_block_shape_valid computes base_units
BaseOf(r) == r.ring - r.idx * F

\* the anchor a chunk finds, or the one it opens
AnchorFor(r) == IF anchor[r.ord].cnt = 0
                  THEN [have |-> 0, cnt |-> r.cnt, first |-> r.num - r.idx, base |-> BaseOf(r),
                        done |-> FALSE]
                  ELSE anchor[r.ord]

\* a chunk (count 2 or more: the code's block path) the receiver refuses:
\* rn_block_shape_valid's index below count, check 8's span inside the
\* ring (every chunk checked to within one unit of the end, as the code
\* checks a chunk not the last; the code checks the last chunk exactly, so
\* this is weaker for it, and SpanInsideRing covers a correct sender), and
\* rn_channels_block_collect's test that the anchor is still collecting and
\* agrees on count, first number and base (MESSAGE_BAD_BLOCK), which
\* AnchorCheck <- AnchorCheckOff drops
Refused(r) ==
    /\ r.cnt >= 2
    /\ LET a == AnchorFor(r)
       IN \/ r.idx >= r.cnt
          \/ BaseOf(r) < 0
          \/ BaseOf(r) + (r.cnt - 1) * F + 1 > B
          \/ AnchorCheck /\ (a.done \/ r.cnt # a.cnt \/ r.num - r.idx # a.first \/ BaseOf(r) # a.base)

\* A record arrives.  A duplicate writes nothing (6.5).  A refused chunk
\* ends the connection and writes nothing.  Otherwise a single record is
\* copied to its ring offset, and a fragment finds or opens its block's
\* anchor by its ordering index and is copied by index (ring + index*F, its
\* own offset), or, in the reversed witness, by arrival (base + have*F).
\* The block is complete when the chunks in reach the arriving chunk's own
\* count, and takes that count (rn_channels_dispatch_chunk's block_have <
\* count, then rn_channels_block_settle); it equals the anchor's whenever
\* the agreement test passed.
Receive(r) ==
    /\ ~violated
    /\ Fwd!Deliver(r)
    /\ IF r.num \in rcvd
         THEN UNCHANGED receiverVars
         ELSE IF Refused(r)
         THEN /\ violated' = TRUE
              /\ UNCHANGED <<rcvd, anchor, ring, ordRead, log>>
         ELSE LET a   == AnchorFor(r)
                  pos == IF PlaceByIndex THEN r.ring ELSE (a.base + a.have * F) % B
                  fin == a.have + 1 >= r.cnt
              IN /\ rcvd' = rcvd \cup {r.num}
                 /\ ring' = [ring EXCEPT ![pos] = r.tag]
                 /\ anchor' = [anchor EXCEPT ![r.ord] =
                                 [a EXCEPT !.have = @ + 1,
                                           !.cnt = IF fin THEN r.cnt ELSE @,
                                           !.done = fin]]
                 /\ UNCHANGED <<ordRead, violated, log>>
    /\ UNCHANGED <<senderVars, forged>>

\* The message at the read index is complete: it is delivered whole, a view
\* of its span read at stride F (no reassembly buffer), and the read index
\* moves by one: one ordering slot per message (7.2, 8 "Receive").
Deliver(o) ==
    /\ ~violated
    /\ anchor[o].done
    /\ o = ordRead
    /\ log' = Append(log, [ord |-> o,
                            data |-> [j \in 1..anchor[o].cnt |->
                                        ring[(anchor[o].base + (j - 1) * F) % B]]])
    /\ ordRead' = ordRead + 1
    /\ UNCHANGED <<rcvd, anchor, ring, violated, senderVars, forged, fwdVars>>

FwdEnvironment == Fwd!Environment /\ UNCHANGED <<senderVars, receiverVars, forged>>

Next ==
    \/ NumberMessage
    \/ \E n \in Nums : SendRecord(n)
    \/ \E n \in Nums, c \in 2..MaxK : ForgeRecord(n, c)
    \/ \E r \in Records : Receive(r)
    \/ \E o \in Ords : Deliver(o)
    \/ FwdEnvironment

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Safety.                                                                 *)
(***************************************************************************)
TypeOK ==
    /\ nextMsg \in 1..M+1
    /\ next \in 0..NumCount
    /\ DOMAIN rec = 0..next-1
    /\ \A n \in 0..next-1 : rec[n] \in Records
    /\ rcvd \subseteq Nums
    /\ ordRead \in 0..M
    /\ violated \in BOOLEAN
    /\ forged \in BOOLEAN
    /\ Len(log) <= M

\* the sender's split: consecutive numbers, one ordering index per message,
\* fragment i at the block's start plus i*F, the indices 0..k-1
SplitConsecutive ==
    \A n \in 0..next-1 :
        /\ rec[n].idx < rec[n].cnt
        /\ rec[n].cnt = Count(rec[n].ord + 1)
        /\ \A i \in 0..rec[n].cnt-1 :
              LET q == n - rec[n].idx + i
              IN  /\ q \in 0..next-1
                  /\ rec[q].ord = rec[n].ord
                  /\ rec[q].idx = i
                  /\ rec[q].ring = (rec[n - rec[n].idx].ring + i * F) % B

\* every message's span lies inside one lap of the ring, from its first
\* record's offset: the padding keeps the receive view contiguous and keeps
\* the receiver's span check (check 8) from refusing a correct sender
SpanInsideRing ==
    \A n \in 0..next-1 :
        rec[n].idx = 0 => rec[n].ring + SpanUnits(rec[n].ord + 1) <= B

\* a correct sender is never refused: every fragment agrees with its anchor
\* and lies inside the ring, at every offset, padded or wrapped
NeverRefused == ~violated

\* a delivered message's bytes are the bytes that were sent
DeliveredEqualsSent ==
    \A i \in DOMAIN log : log[i].data = Sent(log[i].ord + 1)

\* no part of a block is ever delivered: a delivery is the whole message,
\* and every one of its numbers was received before it
NoPartialDelivery ==
    \A i \in DOMAIN log :
        /\ Len(log[i].data) = Count(log[i].ord + 1)
        /\ \A n \in 0..next-1 : rec[n].ord = log[i].ord => n \in rcvd

\* a block takes one ordering slot: the deliveries take the ordering indices
\* 0, 1, 2, ... one each, so every message is delivered once and a block
\* moves the read index by one, never by its fragment count
OneOrderingSlotPerBlock ==
    /\ ordRead = Len(log)
    /\ \A i \in DOMAIN log : log[i].ord = i - 1

=============================================================================
