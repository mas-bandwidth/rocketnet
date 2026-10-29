------------------------------ MODULE RNRing ------------------------------
(***************************************************************************)
(* rocketnet: the receive ring's placement rule (docs/CHANNELS.md 6.2, the *)
(* byte bound; 6.3 step 3; 8, a block's span; 15, the RNRing row).         *)
(*                                                                         *)
(* The receiver's ring of B bytes has no allocator (section 4, item 3).    *)
(* The sender places every holdable record (code 3, code 4, any BLOCK) by  *)
(* giving it an absolute byte span [p, p + len16) when it is numbered, and *)
(* the receiver only checks that the span, taken modulo B, is 16-aligned   *)
(* and inside the ring (sdk/rocketnet_allocator.cpp,                       *)
(* rn_region_ring_span_valid, the only ring code on main).  This module    *)
(* is the sender's rule and the receiver's live spans, so that TLC can     *)
(* check section 3's layer 3 guarantee: "Its slot and ring writes never    *)
(* touch a live message."                                                  *)
(*                                                                         *)
(* The rule (6.2).  len16 is the record's length rounded up to 16.         *)
(* p = ring_next unless [ring_next, ring_next + len16) crosses a multiple  *)
(* of B; then p is the next multiple of B and the skipped bytes are        *)
(* padding.  The span is admitted only if p + len16 - floor <= B, where    *)
(* floor is ring_start(U') while something is outstanding (U' < next) and  *)
(* p when nothing is.  U is the lowest unacked number; U' = U, except when *)
(* U lies inside a block that is not fully acked, when U' is that block's  *)
(* first number.  A block of k fragments at stride F takes k consecutive   *)
(* numbers and one contiguous span of k * F bytes, fragment i at p + i*F,  *)
(* both bounds checked once for the whole block (section 8).               *)
(*                                                                         *)
(* The receiver (6.3 step 1).  A number is live from the moment its        *)
(* record is opened (its bytes are written into the ring) until it is      *)
(* released: delivered, and the update that delivered it ended.  A block   *)
(* is released whole, once every fragment has been opened.  A record       *)
(* arriving again is a duplicate and writes nothing (6.5).                 *)
(*                                                                         *)
(* Acks are abstracted, and one assumption is stated here rather than      *)
(* proved: U' advances only past numbers whose records the receiver has    *)
(* opened and released.  That is what 6.3 step 2 argues from P2, P6 and    *)
(* the scheduler's first-write order, and it is RNWindow's to check.  In   *)
(* this module Ack(n) needs n opened (P2: acked means opened), and the ack  *)
(* that completes a block, or acks a single record, needs the record       *)
(* released.  A fragment of an incomplete block may be acked while it is   *)
(* still held, which is exactly why U' exists.  The transport's link is    *)
(* not instantiated: placement is decided by the sender alone and the      *)
(* receiver's check is local, so loss and reordering appear only as the    *)
(* freedom to open and ack numbers in any order.                           *)
(*                                                                         *)
(* Four constants each remove one guarding rule; TLC must fail each        *)
(* (README-RNRing.md has the traces, replayed by hand):                    *)
(*   FloorAtUPrime = FALSE   floor = ring_start(U): LiveSpansDisjoint      *)
(*   PadAtBoundary = FALSE   no padding: SpanInsideRing                    *)
(*   IdleFloorAtP  = FALSE   floor = ring_start(next-1) when idle:         *)
(*                           EveryAdmissibleMessagePlaced                  *)
(*   Round16       = FALSE   len16 = len: Aligned                          *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    B,              \* the ring, in bytes: a multiple of 16
    Lengths,        \* lengths in bytes a single record may have; each <= B
    FragBytes,      \* F, the block stride: a multiple of 16
    BlockFragments, \* k, fragments per block, >= 2; k * F <= B
    MaxBlocks,      \* how many blocks a behaviour may number (a size knob)
    MaxNumbers,     \* numbers 0 .. MaxNumbers-1 may be given (a size knob)
    FloorAtUPrime,  \* 6.2: the floor is ring_start(U'), not ring_start(U)
    PadAtBoundary,  \* 6.2: a span that would cross a multiple of B is moved up
    IdleFloorAtP,   \* 6.2: the floor is p when nothing is outstanding
    Round16         \* 6.2: len16 is the length rounded up to 16

ASSUME /\ B \in Nat \ {0} /\ B % 16 = 0
       /\ Lengths \subseteq 1..B /\ Lengths # {}
       /\ FragBytes \in Nat \ {0} /\ FragBytes % 16 = 0
       /\ BlockFragments \in Nat \ {0, 1} /\ BlockFragments * FragBytes <= B
       /\ MaxBlocks \in Nat /\ MaxNumbers \in Nat \ {0}
       /\ FloorAtUPrime \in BOOLEAN /\ PadAtBoundary \in BOOLEAN
       /\ IdleFloorAtP \in BOOLEAN /\ Round16 \in BOOLEAN

VARIABLES
    next,      \* the sender's next number
    ringNext,  \* the sender's ring_next: an absolute byte position
    span,      \* span[n] = [s |-> start, e |-> end), absolute bytes, for n < next
    first,     \* first[n]: the first number of n's block; n itself for a single
    acked,     \* the numbers the sender has seen acked
    opened,    \* the numbers the receiver has opened (written into the ring)
    live,      \* the numbers whose spans are live at the receiver
    pending,   \* the record waiting to be numbered, or NoPending
    blocks     \* how many blocks have been numbered

vars == <<next, ringNext, span, first, acked, opened, live, pending, blocks>>

NoPending == [kind |-> "none", len |-> 0]
Pending   == [kind : {"none", "single", "block"}, len : {0} \cup Lengths]

Numbered    == 0 .. next - 1
Outstanding == Numbered \ acked
Block(n)    == { m \in Numbered : first[m] = first[n] }

Min(S) == CHOOSE x \in S : \A y \in S : x <= y

\* U, the lowest unacked number, and U' (6.2); both are next when idle
U      == IF Outstanding = {} THEN next ELSE Min(Outstanding)
UPrime == IF U = next THEN next ELSE first[U]

(***************************************************************************)
(* The placement rule, as functions of the sender's state.                 *)
(***************************************************************************)
Len16(len) == IF Round16 THEN ((len + 15) \div 16) * 16 ELSE len

\* does [p, p + l) contain a multiple of B strictly inside it?
Crosses(p, l) == (p \div B) # ((p + l - 1) \div B)

\* p for a span of l bytes: ring_next, or the next multiple of B
Pos(l) == IF PadAtBoundary /\ Crosses(ringNext, l)
          THEN ((ringNext \div B) + 1) * B
          ELSE ringNext

Base == IF FloorAtUPrime THEN UPrime ELSE U

Floor(p) == IF Base < next THEN span[Base].s
            ELSE IF IdleFloorAtP \/ next = 0 THEN p
            ELSE span[next - 1].s

Admits(p, l) == p + l - Floor(p) <= B

BlockBytes == BlockFragments * FragBytes

(***************************************************************************)
(* The operations.                                                         *)
(***************************************************************************)
Init == /\ next = 0 /\ ringNext = 0
        /\ span = [n \in {} |-> 0] /\ first = [n \in {} |-> 0]
        /\ acked = {} /\ opened = {} /\ live = {}
        /\ pending = NoPending /\ blocks = 0

\* A message arrives at the sender and waits for its number.  Only one
\* waits at a time, and only one the numbering budget can still give.
WantSingle(len) ==
    /\ pending.kind = "none" /\ next < MaxNumbers
    /\ pending' = [kind |-> "single", len |-> len]
    /\ UNCHANGED <<next, ringNext, span, first, acked, opened, live, blocks>>

WantBlock ==
    /\ pending.kind = "none" /\ blocks < MaxBlocks
    /\ next + BlockFragments <= MaxNumbers
    /\ pending' = [kind |-> "block", len |-> 0]
    /\ UNCHANGED <<next, ringNext, span, first, acked, opened, live, blocks>>

\* The waiting single record is numbered and placed (6.2).
PlaceSingle ==
    /\ pending.kind = "single"
    /\ LET l == Len16(pending.len)
           p == Pos(l)
       IN /\ Admits(p, l)
          /\ span' = [n \in 0..next |-> IF n = next THEN [s |-> p, e |-> p + l] ELSE span[n]]
          /\ first' = [n \in 0..next |-> IF n = next THEN next ELSE first[n]]
          /\ next' = next + 1
          /\ ringNext' = p + l
    /\ pending' = NoPending
    /\ UNCHANGED <<acked, opened, live, blocks>>

\* The waiting block is numbered: k consecutive numbers, one contiguous
\* span, fragment i at p + i*F, both bounds checked once (section 8).
PlaceBlock ==
    /\ pending.kind = "block"
    /\ LET l == BlockBytes
           p == Pos(l)
           k == BlockFragments
       IN /\ Admits(p, l)
          /\ span' = [n \in 0..next + k - 1 |->
                        IF n >= next
                        THEN [s |-> p + (n - next) * FragBytes, e |-> p + (n - next + 1) * FragBytes]
                        ELSE span[n]]
          /\ first' = [n \in 0..next + k - 1 |-> IF n >= next THEN next ELSE first[n]]
          /\ next' = next + k
          /\ ringNext' = p + l
    /\ blocks' = blocks + 1
    /\ pending' = NoPending
    /\ UNCHANGED <<acked, opened, live>>

(***************************************************************************)
(* The outside events: the receiver and the acks, in any order.            *)
(***************************************************************************)
\* The receiver opens record n: its bytes are written into the ring at its
\* span and n is live from here.  A second arrival is a duplicate (6.5).
Open(n) ==
    /\ n \in Numbered \ opened
    /\ opened' = opened \cup {n}
    /\ live' = live \cup {n}
    /\ UNCHANGED <<next, ringNext, span, first, acked, pending, blocks>>

\* The receiver delivered n's message and the update ended: its span, and
\* for a block every fragment's span, stops being live.  A block is
\* delivered only once every fragment has been opened (section 8).
Release(n) ==
    /\ n \in live
    /\ Block(n) \subseteq opened
    /\ live' = live \ Block(n)
    /\ UNCHANGED <<next, ringNext, span, first, acked, opened, pending, blocks>>

\* The sender learns n was acked.  P2: acked means opened.  The assumption
\* of 6.3 step 2, stated in the header: the ack that moves U' past a record
\* (a single, or the last fragment of a block) was built after the record
\* was released.
Ack(n) ==
    /\ n \in Outstanding
    /\ n \in opened
    /\ (Block(n) \ {n} \subseteq acked) => (Block(n) \cap live = {})
    /\ acked' = acked \cup {n}
    /\ UNCHANGED <<next, ringNext, span, first, opened, live, pending, blocks>>

PlaceAny   == PlaceSingle \/ PlaceBlock
OpenAny    == \E n \in Numbered : Open(n)
ReleaseAny == \E n \in Numbered : Release(n)
AckAny     == \E n \in Numbered : Ack(n)

\* The numbering budget is finite, so a behaviour ends: once every number is
\* given and acked and nothing waits, the model stutters here.  TLC's
\* deadlock check is then left for a real stall: a record that waits with no
\* step enabled (the third witness).
Done == /\ next = MaxNumbers /\ Outstanding = {} /\ pending.kind = "none"
        /\ UNCHANGED vars

Next == \/ \E len \in Lengths : WantSingle(len)
        \/ WantBlock
        \/ PlaceAny \/ OpenAny \/ ReleaseAny \/ AckAny
        \/ Done

\* Fairness for liveness only: the sender places what fits, the receiver
\* opens and releases what arrives, and acks arrive.  No safety property
\* depends on it.
Fairness == /\ WF_vars(PlaceAny) /\ WF_vars(OpenAny)
            /\ WF_vars(ReleaseAny) /\ WF_vars(AckAny)

Spec == Init /\ [][Next]_vars /\ Fairness

-----------------------------------------------------------------------------
(***************************************************************************)
(* Safety.                                                                 *)
(***************************************************************************)
TypeOK ==
    /\ next \in 0..MaxNumbers
    /\ ringNext \in Nat
    /\ span \in [Numbered -> [s : Nat, e : Nat]]
    /\ \A n \in Numbered : span[n].s < span[n].e
    /\ first \in [Numbered -> Numbered]
    /\ acked \subseteq opened /\ live \subseteq opened /\ opened \subseteq Numbered
    /\ pending \in Pending
    /\ blocks \in 0..MaxBlocks

\* the bytes of the ring a span occupies
RingBytes(n) == { x % B : x \in span[n].s .. span[n].e - 1 }

\* 6.3 step 3 and the layer 3 guarantee: no two live spans share a ring byte
LiveSpansDisjoint ==
    \A m, n \in live : m # n => RingBytes(m) \cap RingBytes(n) = {}

\* every span, taken modulo B, lies inside [0, B) without wrapping: the
\* receiver's check, rn_region_ring_span_valid, passes for a correct sender
SpanInsideRing ==
    \A n \in Numbered : (span[n].s % B) + (span[n].e - span[n].s) <= B

\* every placement is a multiple of 16 (section 4: the ring base is
\* 16-aligned, so every placement is 16-aligned too)
Aligned == \A n \in Numbered : span[n].s % 16 = 0

\* 6.2's last paragraph as a state predicate: with nothing outstanding the
\* ring admits any single record and a block, wherever ring_next is
IdleAdmitsAnyMessage ==
    Outstanding = {} =>
        /\ \A len \in Lengths : Admits(Pos(Len16(len)), Len16(len))
        /\ Admits(Pos(BlockBytes), BlockBytes)

(***************************************************************************)
(* Liveness.                                                               *)
(***************************************************************************)
Waiting == pending.kind # "none"

\* 6.2: when nothing is outstanding, any message of at most
\* max_message_bytes <= B is placed
EveryAdmissibleMessagePlaced == (Outstanding = {} /\ Waiting) ~> ~Waiting

\* 6.2: "No record waits forever", given the receiver and the acks are fair
NoRecordWaitsForever == Waiting ~> ~Waiting

=============================================================================
