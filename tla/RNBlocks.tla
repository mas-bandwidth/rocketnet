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
(* Every record of message m carries ordering index m-1: a block takes one *)
(* ordering slot.  Fragment i of message m carries the tag <<m, i>>, which *)
(* stands for its bytes.  Acks are not modelled (RNWindow checks them and  *)
(* the U' rule; RNRing checks ring reuse), so nothing is ever acked, U' is *)
(* the first number and the floor is the first span's start; the instance *)
(* is one window wide and one ring long.  The RTO is abstract: any         *)
(* numbered record may be resent at any time.                              *)
(*                                                                         *)
(* The receiver.  A record passes layer 3's duplicate check (a number      *)
(* already received writes nothing, 6.5).  A single record is copied to   *)
(* its ring offset.  A fragment opens or finds its block's anchor by its   *)
(* first number b = n - index (no start packet: any fragment opens the     *)
(* state), is copied to ring + index*F (its own ring offset), and counts   *)
(* in the anchor.  When have = k the block is complete; the message whose  *)
(* ordering index is the channel's read index is delivered when complete,  *)
(* as a view of its span read at stride F, and the read index moves by     *)
(* one.                                                                    *)
(*                                                                         *)
(* The reversed witness is a constant:                                     *)
(*   PlaceByIndex = FALSE   a fragment is copied to base + have*F, the     *)
(*                          next free place in arrival order: fragments    *)
(*                          concatenated in the order they arrived.        *)
(*                          TLC must fail DeliveredEqualsSent.             *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    W,            \* the window: numbers in flight
    F,            \* the fragment stride, in units of 16 bytes
    B,            \* the receive ring, in units
    Small,        \* the largest message that is one record, in units
    Units,        \* Units[m]: message m's length in units, m \in 1..Len(Units)
    PlaceByIndex, \* BOOLEAN: a fragment is copied to ring + index*F (8)
    FwdPattern,   \* Link loss pattern, sender to receiver
    Capacity, MaxDelay, Duplication, Reordering, BurstStart, BurstLen

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

\* what message m's bytes are, fragment by fragment
Sent(m) == [j \in 1..Count(m) |-> <<m, j - 1>>]

ASSUME /\ W \in Nat \ {0}
       /\ F \in Nat \ {0}
       /\ B \in Nat \ {0}
       /\ Small \in Nat \ {0}
       /\ M \in Nat \ {0}
       /\ \A m \in Msgs : Units[m] \in 1..B /\ Count(m) <= W
       /\ PlaceByIndex \in BOOLEAN

\* a record on the wire: its number, ordering index, fragment index, count,
\* ring offset (units, modulo B) and the tag standing for its bytes
Records == [num : Nums, ord : 0..M-1, idx : 0..MaxK-1, cnt : 1..MaxK,
            ring : 0..B-1, tag : Tags]

None == <<0, 0>>   \* an empty ring unit (no message is 0)

VARIABLES
    \* the sender
    nextMsg,   \* the FIFO head: the next message to number
    next,      \* the next reliable number
    ringNext,  \* ring_next, absolute units
    rec,       \* rec[n]: the record numbered n, for n < next
    \* the receiver
    rcvd,      \* the numbers received (layer 3's bitset, never cleared here)
    anchor,    \* anchor[b]: the block whose first number is b: have, cnt, ord, base
    ring,      \* the receive ring, unit -> tag or None
    ordRead,   \* the channel's ordered read index
    \* ghosts
    log,       \* the deliveries, in order: [ord, data]
    \* the link
    fwdWire, fwdSent, fwdPulse

vars == <<nextMsg, next, ringNext, rec, rcvd, anchor, ring, ordRead, log,
          fwdWire, fwdSent, fwdPulse>>

senderVars   == <<nextMsg, next, ringNext, rec>>
receiverVars == <<rcvd, anchor, ring, ordRead, log>>
fwdVars      == <<fwdWire, fwdSent, fwdPulse>>

Fwd == INSTANCE Link WITH wire <- fwdWire, sent <- fwdSent, pulse <- fwdPulse,
                          Packets <- Records, Pattern <- FwdPattern

NoAnchor == [have |-> 0, cnt |-> 0, ord |-> 0, base |-> 0]

Init ==
    /\ nextMsg = 1 /\ next = 0 /\ ringNext = 0
    /\ rec = [n \in {} |-> 0]
    /\ rcvd = {}
    /\ anchor = [b \in Nums |-> NoAnchor]
    /\ ring = [u \in 0..B-1 |-> None]
    /\ ordRead = 0
    /\ log = <<>>
    /\ Fwd!Init

(***************************************************************************)
(* The sender.                                                             *)
(***************************************************************************)

\* 6.2's placement: ring_next, or the next multiple of B when the span
\* would cross one (the skipped units are padding)
Pos(l) == IF (ringNext \div B) # ((ringNext + l - 1) \div B)
          THEN ((ringNext \div B) + 1) * B
          ELSE ringNext

\* nothing is acked here, so U' is the first number and the floor is the
\* first span's start (the start of the record U' names)
Floor(p) == IF next = 0 THEN p ELSE rec[0].ring

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
          /\ nextMsg' = m + 1
    /\ UNCHANGED <<receiverVars, fwdVars>>

\* A numbered record is written into a packet: a first send or a resend,
\* each fragment on its own (8, "First sends and resends").
SendRecord(n) ==
    /\ n \in 0..next-1
    /\ Fwd!Send(rec[n])
    /\ UNCHANGED <<senderVars, receiverVars>>

(***************************************************************************)
(* The receiver.                                                           *)
(***************************************************************************)

\* A record arrives.  A duplicate writes nothing (6.5).  A single record is
\* copied to its ring offset.  A fragment finds or opens its block's anchor
\* by b = n - index and is copied by index (ring + index*F, its own offset),
\* or, in the reversed witness, by arrival (base + have*F).
Receive(r) ==
    /\ Fwd!Deliver(r)
    /\ IF r.num \in rcvd
         THEN UNCHANGED receiverVars
         ELSE LET b    == r.num - r.idx
                  base == (r.ring + B - ((r.idx * F) % B)) % B
                  a    == IF anchor[b].cnt = 0
                            THEN [have |-> 0, cnt |-> r.cnt, ord |-> r.ord, base |-> base]
                            ELSE anchor[b]
                  pos  == IF PlaceByIndex THEN r.ring ELSE (a.base + a.have * F) % B
              IN /\ rcvd' = rcvd \cup {r.num}
                 /\ ring' = [ring EXCEPT ![pos] = r.tag]
                 /\ anchor' = [anchor EXCEPT ![b] = [a EXCEPT !.have = @ + 1]]
                 /\ UNCHANGED <<ordRead, log>>
    /\ UNCHANGED senderVars

\* The message at the read index is complete: it is delivered whole, a view
\* of its span read at stride F (no reassembly buffer), and the read index
\* moves by one: one ordering slot per message (7.2, 8 "Receive").
Deliver(b) ==
    /\ anchor[b].cnt > 0
    /\ anchor[b].have = anchor[b].cnt
    /\ anchor[b].ord = ordRead
    /\ log' = Append(log, [ord |-> anchor[b].ord,
                            data |-> [j \in 1..anchor[b].cnt |->
                                        ring[(anchor[b].base + (j - 1) * F) % B]]])
    /\ ordRead' = ordRead + 1
    /\ UNCHANGED <<rcvd, anchor, ring, senderVars, fwdVars>>

FwdEnvironment == Fwd!Environment /\ UNCHANGED <<senderVars, receiverVars>>

Next ==
    \/ NumberMessage
    \/ \E n \in Nums : SendRecord(n)
    \/ \E r \in Records : Receive(r)
    \/ \E b \in Nums : Deliver(b)
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
