----------------------------- MODULE RNWindow -----------------------------
(***************************************************************************)
(* rocketnet: the message layer's reliable window (docs/CHANNELS.md,       *)
(* layer 3, sections 6 and 15).  Written before the window code; the code  *)
(* now exists and cites it (sdk/rocketnet_window_send.cpp,                 *)
(* rocketnet_window_receive.cpp, rocketnet_window_ack.cpp), bound by       *)
(* citation, not by a trace walk.                                          *)
(*                                                                         *)
(* One sender and one receiver, joined by two instances of Link: the       *)
(* forward link carries data packets (a packet id and the message numbers  *)
(* it carries) and the reverse link carries acks (the packet ids the       *)
(* receiver opened).  The transport underneath is abstracted to its two    *)
(* preconditions, P2 and P6 of CHANNELS.md section 2:                      *)
(*                                                                         *)
(*   P2  opening a packet is what acks it: the receiver puts the id of     *)
(*       every packet it opens into the next ack it sends, whatever this   *)
(*       layer then does with the records inside;                          *)
(*   P6  a reply to a packet opened in update j is opened in update j+1    *)
(*       or later: the receiver opens packets only while its batch is      *)
(*       open ("receive"), fixes the batch, and only then sends acks       *)
(*       ("process"); the next open comes after the update boundary.       *)
(*                                                                         *)
(* The sender (6.1, 6.2, 6.4): next, the next message number; the acked    *)
(* flags of the per-number table; U, the oldest unacked number; U', the    *)
(* window base, which is U except inside a block that is not fully acked,  *)
(* where it is the block's first number; and the sent-packet map, a ring   *)
(* of P entries from packet id to the numbers the packet carried, evicting *)
(* the oldest when full.  A record takes its number late, when it is first *)
(* written, and only while n < U' + W.  Writing a packet gives it a fresh  *)
(* id and any set of unacked numbers: a first send, a resend, or both in   *)
(* one packet.  The RTO is abstract: any unacked number may be resent at   *)
(* any time (6.4, "resent on the RTO"); last_send and the resend FIFO      *)
(* therefore have no counterpart here.  Blocks appear only as their effect *)
(* on U' and on the receiver's holds: one block of BlockLen consecutive    *)
(* numbers, numbered at one moment (section 8).                            *)
(*                                                                         *)
(* The receiver (6.1, 6.3, 6.5): R, the lowest number not yet received;    *)
(* the received bitset, here the set of received numbers in R..R+W-1; the  *)
(* descriptors indexed n % W with their live flag; the numbers held        *)
(* (waiting on an ordering predecessor, or part of an incomplete block);   *)
(* the delivered list of the current update; and the update phase.  One   *)
(* ordered channel is modelled abstractly: the numbers in Ordered are on   *)
(* it, in number order (layer 6 first-writes a channel's records in        *)
(* ordering order, so number order is ordering order), the block is one    *)
(* ordered message at one ordering slot, and every other number is         *)
(* unordered (code 2) and delivered on arrival.  A number is live while it *)
(* is held or delivered in the current update (6.3 step 1).                *)
(*                                                                         *)
(* Packet ids.  The transport's id never repeats (P4).  A finite model    *)
(* cannot mint ids forever, so an id is reused only once no copy of it     *)
(* exists anywhere: not in the sent map, not pending at the receiver, not  *)
(* on either wire.  Nothing can then confuse the old id with the new one,  *)
(* which is all the never-repeats rule is for.  When no id is free the     *)
(* write waits; like Link's Capacity this is a model-size knob, never a    *)
(* loss.                                                                   *)
(*                                                                         *)
(* Wraparound (6.5, 13.5).  Packets carry the number itself, so the ghost  *)
(* checks know which record is which; the receiver sees only the wire      *)
(* value n % NumMod (NumMod stands for 2^24) and reconstructs it against R *)
(* by signed half range.  With MaxNum >= NumMod the numbers really wrap.   *)
(* A reconstruction that names a different number is recorded in misread   *)
(* and the step does nothing else: WireNumberReconstructs fails there.     *)
(*                                                                         *)
(* The reversed witnesses are constants, each the guarding rule it names: *)
(*   BlockUPrime = FALSE    U' ignores partial blocks (U' = U always);     *)
(*   KeepP6 = FALSE         the receiver may open a packet after it fixed  *)
(*                          the batch: a reply opened in the update that   *)
(*                          elicited it;                                   *)
(*   HalfRange = FALSE      unsigned reconstruction, R + ((w - R) % NumMod)*)
(*   HoldOrdered = FALSE    an ordered number is delivered on arrival;     *)
(*   DropDuplicates = FALSE a number below R or with its bit set is taken  *)
(*                          as fresh.                                      *)
(* The first two are section 15's and must fail NoSlotCollision; the       *)
(* others must fail WireNumberReconstructs, OrderedDeliveredInOrder and    *)
(* DeliveredOnce.                                                          *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    W,            \* the window: numbers in flight, slots at the receiver
    MaxNum,       \* message numbers are 0..MaxNum
    BlockFirst,   \* the block's first number
    BlockLen,     \* the block's fragment count k, >= 2
    Ordered,      \* the numbers on the ordered channel; the block is on it
    P,            \* sent-packet map entries
    MaxPerPacket, \* records a packet may carry, >= 1
    MaxPid,       \* packet ids are 0..MaxPid-1
    NumMod,       \* the wire number's modulus, the model's 2^24; even, >= 2W
    BlockUPrime,  \* BOOLEAN: U' stops at a partial block (the rule of 6.2)
    KeepP6,       \* BOOLEAN: the receiver keeps P6
    HalfRange,    \* BOOLEAN: the receiver reconstructs by signed half range (13.5)
    HoldOrdered,  \* BOOLEAN: an ordered number waits for its predecessors (7.2)
    DropDuplicates, \* BOOLEAN: a number below R or with its bit set is dropped (6.5)
    FwdPattern,   \* Link loss pattern, sender to receiver
    RevPattern,   \* Link loss pattern, receiver to sender
    Capacity, MaxDelay, Duplication, Reordering, BurstStart, BurstLen

Nums  == 0..MaxNum
Block == BlockFirst..BlockFirst+BlockLen-1
Slots == 0..W-1
Pids  == 0..MaxPid-1

ASSUME /\ W \in Nat \ {0}
       /\ BlockLen \in Nat \ {0, 1}
       /\ BlockLen <= W
       /\ Block \subseteq Nums
       /\ Block \subseteq Ordered
       /\ Ordered \subseteq Nums
       /\ P \in Nat \ {0}
       /\ MaxPerPacket \in Nat \ {0}
       /\ MaxPid \in Nat \ {0}
       /\ NumMod \in Nat /\ NumMod % 2 = 0 /\ NumMod >= 2 * W
       /\ BlockUPrime \in BOOLEAN
       /\ KeepP6 \in BOOLEAN
       /\ HalfRange \in BOOLEAN
       /\ HoldOrdered \in BOOLEAN
       /\ DropDuplicates \in BOOLEAN

\* a packet carries one to MaxPerPacket numbers
Loads       == {s \in SUBSET Nums : Cardinality(s) \in 1..MaxPerPacket}
DataPackets == [id : Pids, nums : Loads]
AckPackets  == (SUBSET Pids) \ {{}}

VARIABLES
    \* the sender
    next,       \* the next number to give out
    acked,      \* the numbers marked acked in the per-number table
    smap,       \* the sent-packet map: Seq of [id, nums], oldest first
    \* the receiver
    R,          \* the lowest number not yet received
    rbits,      \* the received bitset: the received numbers in R..R+W-1
    desc,       \* the descriptors: slot -> [num, live]
    held,       \* numbers held: waiting on a predecessor, or an incomplete block
    delivered,  \* the delivered list of the current update
    phase,      \* "receive" (batch open) or "process" (batch fixed)
    pending,    \* ids opened this update, to go in the next ack
    \* ghosts
    everDelivered, \* every number ever delivered
    dup,           \* a number was delivered twice
    violations,    \* the receiver's checks that fired (6.5)
    evicted,       \* the sent map evicted an entry at least once
    misread,       \* a wire number was reconstructed as a different number
    \* the links
    fwdWire, fwdSent, fwdPulse,
    revWire, revSent, revPulse

vars == <<next, acked, smap, R, rbits, desc, held, delivered, phase, pending,
          everDelivered, dup, violations, evicted, misread,
          fwdWire, fwdSent, fwdPulse, revWire, revSent, revPulse>>

senderVars   == <<next, acked, smap>>
receiverVars == <<R, rbits, desc, held, delivered, phase, pending>>
ghostVars    == <<everDelivered, dup, violations, evicted, misread>>
fwdVars      == <<fwdWire, fwdSent, fwdPulse>>
revVars      == <<revWire, revSent, revPulse>>

Fwd == INSTANCE Link WITH wire <- fwdWire, sent <- fwdSent, pulse <- fwdPulse,
                          Packets <- DataPackets, Pattern <- FwdPattern
Rev == INSTANCE Link WITH wire <- revWire, sent <- revSent, pulse <- revPulse,
                          Packets <- AckPackets, Pattern <- RevPattern

(***************************************************************************)
(* The sender's derived state (6.2).                                       *)
(***************************************************************************)
Min(S) == CHOOSE m \in S : \A x \in S : m <= x

Numbered == 0..next-1
Unacked  == Numbered \ acked

\* U, the oldest unacked number; next when nothing is outstanding.
U == IF Unacked = {} THEN next ELSE Min(Unacked)

\* U', the window base: U, except inside a block that is not fully acked.
\* U \in Block means U, unacked, is inside the block, so the block is not
\* fully acked.  BlockUPrime = FALSE is reversed witness 1.
UPrime == IF BlockUPrime /\ U \in Block THEN BlockFirst ELSE U

MapIds   == {smap[i].id : i \in DOMAIN smap}
UsedPids == MapIds \cup pending
           \cup {fwdWire[i].pkt.id : i \in DOMAIN fwdWire}
           \cup UNION {revWire[i].pkt : i \in DOMAIN revWire}
FreePids == Pids \ UsedPids

(***************************************************************************)
(* The receiver's derived state (6.3, 7.2).                                *)
(***************************************************************************)

\* R advances past every set bit: the least r >= from not in bits.
RECURSIVE Advance(_, _)
Advance(bits, from) == IF from \in bits THEN Advance(bits, from + 1) ELSE from

\* every number the receiver has received: below R, or a set bit
ReceivedSet(r, bits) == (0..r-1) \cup bits

\* the ordered records that must be delivered before n: the ordered numbers
\* below it, and, for a fragment, the rest of its block (one message, one
\* ordering slot, delivered whole: section 8)
OrderedPred(n) == {m \in Ordered : m < n} \cup (IF n \in Block THEN Block \ {n} ELSE {})

\* the live numbers (6.3 step 1)
Live == held \cup delivered

\* The wire value w = n % NumMod reconstructed against r (6.5, 13.5):
\* n = r + delta, delta the signed difference in -NumMod/2 .. NumMod/2-1.
\* HalfRange = FALSE is the unsigned reading r + ((w - r) % NumMod).
Recon(w, r) ==
    LET d == (((w - r) % NumMod) + NumMod) % NumMod
    IN  IF HalfRange /\ d >= NumMod \div 2 THEN r + d - NumMod ELSE r + d

(***************************************************************************)
(* Init.                                                                   *)
(***************************************************************************)
Init ==
    /\ next = 0 /\ acked = {} /\ smap = <<>>
    /\ R = 0 /\ rbits = {}
    /\ desc = [s \in Slots |-> [num |-> 0, live |-> FALSE]]
    /\ held = {} /\ delivered = {} /\ phase = "receive" /\ pending = {}
    /\ everDelivered = {} /\ dup = FALSE /\ violations = {} /\ evicted = FALSE
    /\ misread = FALSE
    /\ Fwd!Init /\ Rev!Init

(***************************************************************************)
(* The sender's operations.                                                *)
(***************************************************************************)

\* A record takes the next number, late, under the count bound n < U' + W
\* (6.2).  The block takes all its numbers at once, under one check of its
\* last number (section 8).  The byte bound is RNRing's.
\* WouldNumber is the count half of the code's admit test
\* (rn_window_send_fits, asked by rn_window_send_would_number and
\* rn_window_send_stall_unless_numbered).  The code's test also needs the
\* record's span to fit in the send ring above the ring floor (U''s
\* ring_start) for every holdable record (RNRing Admits); that half is not
\* here.  So NumberRecord is enabled in states where the code stalls on ring
\* bytes: safety only gains behaviours, and the liveness below assumes the
\* ring fits the record once acks move U' (the ring and the window are not
\* composed in any model).
\* (Before revision 1.2 this comment said WouldNumber asks exactly the test
\* NumberRecord applies in the code; it asks the count half.)
WouldNumber ==
    IF next = BlockFirst
      THEN BlockFirst + BlockLen - 1 < UPrime + W
      ELSE next < UPrime + W

NumberRecord ==
    /\ next <= MaxNum
    /\ WouldNumber
    /\ next' = IF next = BlockFirst THEN BlockFirst + BlockLen ELSE next + 1
    /\ UNCHANGED <<acked, smap, receiverVars, ghostVars, fwdVars, revVars>>

\* A packet is written: a fresh id, carrying any unacked numbered records,
\* first sends and resends alike (the RTO is abstract: any unacked number
\* may be resent at any time).  The sent map records it, evicting the
\* oldest entry when full (6.4); the evicted entry's numbers keep their
\* state and stay resendable.
WritePacket(S, pid) ==
    /\ S \subseteq Unacked /\ S # {} /\ Cardinality(S) <= MaxPerPacket
    /\ pid \in FreePids
    /\ Fwd!Send([id |-> pid, nums |-> S])
    /\ LET e == [id |-> pid, nums |-> S]
       IN IF Len(smap) < P
            THEN smap' = Append(smap, e) /\ UNCHANGED evicted
            ELSE smap' = Append(Tail(smap), e) /\ evicted' = TRUE
    /\ UNCHANGED <<next, acked, receiverVars, everDelivered, dup, violations, misread, revVars>>

WriteAny == \E S \in SUBSET Unacked, pid \in Pids : WritePacket(S, pid)

\* a packet carrying n is written, lost at Send or not
WriteNum(n) == \E S \in SUBSET Unacked, pid \in Pids : n \in S /\ WritePacket(S, pid)

\* a packet carrying n is written and enters the wire
WriteCarrying(n) == WriteNum(n) /\ fwdWire' # fwdWire

\* The transport's acked packet ids reach the sender (6.4, "Ack"): each
\* id still in the map marks its numbers acked and leaves the map; an id
\* not in the map (evicted, or already acked) is ignored.
ReceiveAck(q) ==
    /\ Rev!Deliver(q)
    /\ LET hit == {i \in DOMAIN smap : smap[i].id \in q}
       IN /\ acked' = acked \cup UNION {smap[i].nums : i \in hit}
          /\ smap' = SelectSeq(smap, LAMBDA e : e.id \notin q)
    /\ UNCHANGED <<next, receiverVars, ghostVars, fwdVars>>

ReceiveAckAny == \E q \in AckPackets : ReceiveAck(q)

\* an ack carrying an id the map still holds
ReceiveUsefulAck == \E q \in AckPackets : q \cap MapIds # {} /\ ReceiveAck(q)

(***************************************************************************)
(* The receiver's operations.                                              *)
(***************************************************************************)


\* The receiver opens a packet (P2: the id goes into the next ack whatever
\* happens below) and runs 6.5 on each number in turn, as rn_ordered_receive
\* does record by record (sdk/rocketnet_channel.cpp, through
\* rn_window_receive_accept): a duplicate is dropped; a number outside the
\* window, or whose slot is live with another number, is a violation, after
\* which nothing more is parsed (R6), and the records before it in the packet
\* stay accepted and dispatched.  An accepted number sets its bit and claims
\* its descriptor; R advances past the set bits, clearing them.  Then
\* dispatch (7.2): an unordered number is delivered now; an ordered number is
\* delivered when every ordered predecessor has been received, and it drains
\* the holds that become releasable with it; otherwise it is held.  Delivered
\* and held numbers are live.  Under P6 this happens only while the batch is
\* open.
\*
\* Each number is reconstructed from its wire value against R as it stands
\* when the record is read (6.5), R having advanced over the packet's
\* earlier records, as the code does.  The order of the records in the packet
\* is any order of its numbers (the writer's order is the scheduler's).  A
\* reconstruction that names a different number sets misread and ends the
\* packet.  (Revision 1.2: before it every number of the packet was read
\* against the R the packet found, and a violation discarded the whole
\* packet; with MaxPerPacket = 1 in every case the two readings agree.)

\* the receiver's state as one packet's records pass over it
RecvState(r, bits, d, h, dl, ed, du, vi) ==
    [R |-> r, bits |-> bits, desc |-> d, held |-> h, deliv |-> dl, ever |-> ed,
     dup |-> du, viol |-> vi, misread |-> FALSE, stop |-> FALSE]

\* 6.5 and 7.2 on one record n
AcceptOne(st, n) ==
    IF st.stop THEN st
    ELSE IF Recon(n % NumMod, st.R) # n
      THEN [st EXCEPT !.misread = TRUE, !.stop = TRUE]
    ELSE IF DropDuplicates /\ (n < st.R \/ n \in st.bits)
      THEN st
    ELSE IF n >= st.R + W
      THEN [st EXCEPT !.viol = @ \cup {"outside"}, !.stop = TRUE]
    ELSE IF st.desc[n % W].live /\ st.desc[n % W].num # n
      THEN [st EXCEPT !.viol = @ \cup {"slot"}, !.stop = TRUE]
    ELSE LET bits1   == st.bits \cup {n}
             R1      == Advance(bits1, st.R)
             rbits1  == {m \in bits1 : m >= R1}
             rcvd    == ReceivedSet(R1, rbits1)
             cand    == IF n \in Ordered THEN st.held \cup {n} ELSE st.held
             release == IF HoldOrdered
                          THEN {m \in cand : OrderedPred(m) \subseteq rcvd}
                          ELSE cand
             now     == (IF n \in Ordered THEN {} ELSE {n}) \cup release
         IN  [st EXCEPT !.R = R1, !.bits = rbits1,
                        !.desc = [@ EXCEPT ![n % W] = [num |-> n, live |-> TRUE]],
                        !.held = cand \ release,
                        !.deliv = @ \cup now,
                        !.ever = @ \cup now,
                        !.dup = @ \/ (now \cap st.ever # {})]

RECURSIVE AcceptFrom(_, _, _)
AcceptFrom(ord, i, st) ==
    IF i > Len(ord) THEN st ELSE AcceptFrom(ord, i + 1, AcceptOne(st, ord[i]))

\* the orders a packet's records can come in
Orders(S) == {s \in [1..Cardinality(S) -> S] : \A x \in S : \E i \in 1..Cardinality(S) : s[i] = x}

OpenPacket(p) ==
    /\ phase = "receive" \/ ~KeepP6
    /\ Fwd!Deliver(p)
    /\ pending' = pending \cup {p.id}
    /\ \E ord \in Orders(p.nums) :
         LET f == AcceptFrom(ord, 1, RecvState(R, rbits, desc, held, delivered,
                                               everDelivered, dup, violations))
         IN  /\ R' = f.R
             /\ rbits' = f.bits
             /\ desc' = f.desc
             /\ held' = f.held
             /\ delivered' = f.deliv
             /\ everDelivered' = f.ever
             /\ dup' = f.dup
             /\ violations' = f.viol
             /\ misread' = (misread \/ f.misread)
    /\ UNCHANGED <<senderVars, phase, evicted, revVars>>

OpenAny  == \E p \in DataPackets : OpenPacket(p)
OpenHead == fwdWire # <<>> /\ OpenPacket(Head(fwdWire).pkt)

\* the receiver opens a packet carrying n
OpenCarrying(n) == OpenHead /\ n \in Head(fwdWire).pkt.nums

\* The batch is fixed: no more packets enter this update (P6).
FixBatch ==
    /\ phase = "receive"
    /\ phase' = "process"
    /\ UNCHANGED <<senderVars, R, rbits, desc, held, delivered, pending, ghostVars, fwdVars, revVars>>

\* The acks for the opened packets leave, after the batch is fixed (P6:
\* "update j's batch is fixed before any packet that update builds can
\* leave").  Acks the link has no room for wait for a later update.
SendAck ==
    /\ phase = "process"
    /\ pending # {}
    /\ Rev!Send(pending)
    /\ pending' = {}
    /\ UNCHANGED <<senderVars, R, rbits, desc, held, delivered, phase, ghostVars, fwdVars>>

SendAckThrough == SendAck /\ revWire' # revWire

\* The update boundary: the delivered list expires and its descriptors
\* stop being live; held numbers stay live.
EndUpdate ==
    /\ phase = "process"
    /\ phase' = "receive"
    /\ delivered' = {}
    /\ desc' = [s \in Slots |->
                  IF desc[s].live /\ desc[s].num \in delivered
                    THEN [desc[s] EXCEPT !.live = FALSE]
                    ELSE desc[s]]
    /\ UNCHANGED <<senderVars, R, rbits, held, pending, ghostVars, fwdVars, revVars>>

(***************************************************************************)
(* The outside events: the two paths.                                      *)
(***************************************************************************)
FwdEnvironment == Fwd!Environment /\ UNCHANGED <<senderVars, receiverVars, ghostVars, revVars>>
RevEnvironment == Rev!Environment /\ UNCHANGED <<senderVars, receiverVars, ghostVars, fwdVars>>

Next ==
    \/ NumberRecord
    \/ WriteAny
    \/ ReceiveAckAny
    \/ OpenAny
    \/ FixBatch
    \/ SendAck
    \/ EndUpdate
    \/ FwdEnvironment
    \/ RevEnvironment

(***************************************************************************)
(* Fairness.  Link's Fairness is restated at this model's size, as         *)
(* ClientConnection does: per-value strong fairness over every data packet *)
(* value and every ack value exhausts TLC's tableau.                       *)
(*                                                                         *)
(* SendFair: records are numbered, the oldest unacked number is written    *)
(* again while it can be, acks go out, the receiver's update loop turns.   *)
(* The write and the ack send are strongly fair twice: as plain actions    *)
(* (a lost send still happens, and burns its index in the pattern; the ack *)
(* send is enabled only in the "process" phase, which weak fairness lets   *)
(* the update loop leave first) and in the form "enabled with the pattern  *)
(* letting it through": Link's EventualDelivery stated on the sender, as   *)
(* ClientConnection does.  TLC found both lassos the plain forms exclude;  *)
(* README-RNWindow.md, "What TLC found".                                   *)
(*                                                                         *)
(* DeliveryFairHead: the head of each wire is delivered.  Enough for the   *)
(* deterministic patterns on a FIFO link without duplication.              *)
(*                                                                         *)
(* AcksLand: acks that still matter land.  An ASSUMPTION of the liveness   *)
(* below, which the code does not keep in general.  The model's stand-in   *)
(* for real time: with the RTO abstract, the sender may write faster than  *)
(* any ack returns and evict every entry before its ack lands.  RNSentMap  *)
(* shows when acks land before eviction: P >= W with acks in send order,   *)
(* RTO >= RTT and no receipt-only packets.  The code also commits          *)
(* receipt-only packets into the same P-entry map, which W does not bound, *)
(* and then evicts entries whose acks are on their way                     *)
(* (MCRNSentMapReceiptEviction.cfg; rn_window_ack_commit_receipt_packet).  *)
(* That is the documented contract of packets_in_flight (sdk/rocketnet.h,  *)
(* standard/MESSAGES.md m9.1), and the repair is docs/roadmap.sexp         *)
(* sent-map-receipts-give-way.  So EveryReliableAckedAndDelivered holds    *)
(* here only while the map holds every tracked packet sent between a send  *)
(* and the processing of its ack.  In Fairness, because the abstract RTO   *)
(* outpaces every pattern, not only "arbitrary".                           *)
(*                                                                         *)
(* OldestLands (FairnessTyped) adds what "arbitrary" needs, where Drop     *)
(* removes copies in flight: the oldest number, written without bound, is  *)
(* opened without bound.                                                   *)
(***************************************************************************)
\* The oldest unacked number is written: the resend FIFO's guarantee that
\* the oldest record reaches the head and is resent on the RTO.  Fairness
\* on this one action, rather than on every number, is enough (once U is
\* acked the next number is the oldest) and costs TLC a fraction of the
\* per-number conditions, which ran the liveness check for twenty minutes
\* at the committed size.
WriteOldest        == WriteNum(U)
WriteOldestThrough == WriteOldest /\ fwdWire' # fwdWire
OpenOldest         == OpenHead /\ U \in Head(fwdWire).pkt.nums

\* The update loop: fix the batch, or end the update.  Exactly one of the
\* two is enabled in every state (by phase), so weak fairness on the
\* disjunction is weak fairness on each, at one conjunct of TLC's tableau.
UpdateLoop == FixBatch \/ EndUpdate

SendFair ==
    /\ WF_vars(NumberRecord)
    /\ SF_vars(WriteOldest) /\ SF_vars(WriteOldestThrough)
    /\ SF_vars(SendAck) /\ SF_vars(SendAckThrough)
    /\ WF_vars(UpdateLoop)

DeliveryFairHead == SF_vars(OpenHead) /\ SF_vars(ReceiveAckAny)

\* Acks that still matter land: if acks leave without bound, acks naming an
\* id the map still holds arrive without bound.  The model's stand-in for
\* the RTO being above the RTT (and P above the packets written per RTT):
\* with the RTO abstract, TLC found the sender writing a number twice
\* between the receiver opening it and its ack landing, so every ack named
\* an evicted id and the number was never acked (README-RNWindow.md).
\* An assumption, not kept by the code under receipt-only traffic past P
\* (the header of Fairness above; RNSentMap).
AcksLand == ([]<> <<SendAckThrough>>_vars) => ([]<> <<ReceiveUsefulAck>>_vars)

\* The oldest number, written without bound, is opened without bound:
\* Link's EventualDelivery stated on the oldest number, which "arbitrary"
\* needs (Drop removes copies before they reach the head).
OldestLands == ([]<> <<WriteOldestThrough>>_vars) => ([]<> <<OpenOldest>>_vars)

Fairness      == SendFair /\ DeliveryFairHead /\ AcksLand
FairnessTyped == Fairness /\ OldestLands

Spec      == Init /\ [][Next]_vars /\ Fairness
SpecTyped == Init /\ [][Next]_vars /\ FairnessTyped

(***************************************************************************)
(* Safety.                                                                 *)
(***************************************************************************)
TypeOK ==
    /\ next \in 0..MaxNum+1
    /\ acked \subseteq Numbered
    /\ smap \in Seq([id : Pids, nums : Loads])
    /\ R \in 0..MaxNum+1
    /\ rbits \subseteq Nums
    /\ desc \in [Slots -> [num : Nums, live : BOOLEAN]]
    /\ held \subseteq Nums /\ delivered \subseteq Nums
    /\ phase \in {"receive", "process"}
    /\ pending \subseteq Pids
    /\ everDelivered \subseteq Nums /\ dup \in BOOLEAN
    /\ violations \subseteq {"outside", "slot"} /\ evicted \in BOOLEAN
    /\ misread \in BOOLEAN
    /\ Fwd!TypeOK /\ Rev!TypeOK

\* every table inside its configured bound (R1, R7)
TablesBounded ==
    /\ Len(smap) <= P                        \* the sent-packet map
    /\ next <= UPrime + W                    \* the sender's per-number table
    /\ Cardinality(Unacked) <= W
    /\ \A n \in rbits : n >= R /\ n < R + W  \* the bitset has W bits
    /\ Cardinality(Live) <= W                \* descriptors, holds, delivered list (6.3 step 4)
    /\ Fwd!InFlightBounded /\ Rev!InFlightBounded

\* a correct sender never trips the outside-window check (6.5)
ReceiverNeverOutsideWindow == "outside" \notin violations

\* no two live numbers share a slot n % W (6.3 step 1), and the receiver's
\* own check never fires
NoSlotCollision ==
    /\ "slot" \notin violations
    /\ \A m, n \in Live : m # n => m % W # n % W

\* an acked number was received (P2 carried through the map)
AckedImpliesReceived == acked \subseteq ReceivedSet(R, rbits)

\* no number is delivered twice, and only numbered records are delivered
DeliveredOnce == ~dup /\ everDelivered \subseteq Numbered

\* In order: an ordered number is delivered only once every ordered number
\* below it, and the rest of its block, is delivered too.  Deliveries in
\* one step are a set here; the code drains them in ordering order (7.2).
OrderedDeliveredInOrder ==
    \A n \in everDelivered \cap Ordered : OrderedPred(n) \subseteq everDelivered

\* every wire number the receiver read was reconstructed as itself (13.5)
WireNumberReconstructs == ~misread

\* a held number is received and not yet releasable; a delivered one was
\* released; the descriptors name the live numbers
HeldIsUnreleasable ==
    /\ held \subseteq ReceivedSet(R, rbits)
    /\ \A n \in held : ~(OrderedPred(n) \subseteq ReceivedSet(R, rbits))
    /\ \A n \in Live : desc[n % W].live /\ desc[n % W].num = n

(***************************************************************************)
(* Liveness.  Stated as "eventually all": acked, everDelivered and next    *)
(* only grow and Nums is finite, so in this instance each is the same as   *)
(* its per-number form \A n : (n < next) ~> (n \in acked), and costs TLC   *)
(* one tableau branch instead of one per number.                           *)
(***************************************************************************)
EveryRecordEventuallyNumbered    == <>(next = MaxNum + 1)
EveryReliableEventuallyAcked     == <>(acked = Nums)
EveryReliableEventuallyDelivered == <>(everDelivered = Nums)

\* the three at once, in one branch: every number is numbered, acked and
\* delivered (acked \subseteq Numbered, so the first follows)
EveryReliableAckedAndDelivered   == <>(acked = Nums /\ everDelivered = Nums)

=============================================================================
