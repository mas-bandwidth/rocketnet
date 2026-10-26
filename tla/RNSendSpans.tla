----------------------------- MODULE RNSendSpans -----------------------------
(***************************************************************************)
(* The sender's span ring, as sdk/rocketnet_allocator.cpp builds it:       *)
(* rn_span_place, rn_span_link and rn_span_unlink, reached through         *)
(* rn_region_send_alloc_span (Alloc / Exhaust), rn_region_send_free and    *)
(* rn_region_send_free_fragment's last fragment (Free), and the region     *)
(* reset (Reset).                                                          *)
(*                                                                         *)
(* tla/RNRing.tla models the receiver's ring with monotonic positions and  *)
(* a floor at U'.  This module models the other representation: positions *)
(* are offsets in 0..SR-1, the floor is head (the oldest live span's       *)
(* start), the next span goes at tail, the live spans order by the         *)
(* allocation ticket each span's descriptor carries (first..last name the  *)
(* smallest and largest live tickets), frees come in any order, a span     *)
(* that would cross the end of the ring goes to 0 and leaves the skipped   *)
(* bytes as padding, tail = head on a non-empty ring means full, and the   *)
(* ring resets head and tail to 0 when it empties.  Tickets renumber in    *)
(* order on wraparound, which the model abstracts as unbounded tickets.    *)
(*                                                                         *)
(* Descriptors are 1..Q; NONE = 0 stands for RN_DESCRIPTOR_NONE.  The      *)
(* free-descriptor list is abstracted to "any descriptor not live", a      *)
(* superset of the code's LIFO choice.  order is a ghost: the live         *)
(* descriptors in the order they were allocated, which the list must       *)
(* equal.                                                                  *)
(*                                                                         *)
(* Two constants flip one rule each for the reversed witnesses:            *)
(*   FloorAtOldest = FALSE  the floor (head) follows the newest span's     *)
(*                          start instead of the oldest's;                 *)
(*   PadOnWrap     = FALSE  a span that does not fit before the end goes   *)
(*                          at tail and crosses the end, no padding.       *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    SR,             \* region->span_bytes, a multiple of 16
    Lengths,        \* message byte counts offered to rn_region_send_alloc_span
    Q,              \* send descriptors (region->queue)
    FloorAtOldest,  \* TRUE in the code
    PadOnWrap       \* TRUE in the code

ASSUME SR > 0 /\ SR % 16 = 0
ASSUME Lengths \subseteq 1..SR
ASSUME Q \in Nat \ {0}

NONE == 0
D == 1..Q

VARIABLES
    head,       \* region->span_head
    tail,       \* region->span_tail
    first,      \* region->span_first
    last,       \* region->span_last
    start,      \* descriptor->span_start
    len,        \* the span's rounded length (len16)
    ticket,     \* descriptor->span_ticket: the span's allocation ticket
    ticketNext, \* region->span_ticket_next
    live,       \* descriptors with RN_SEND_FLAG_SPAN set
    order,      \* ghost: live descriptors in allocation order
    exhausted   \* rn_region_exhaust has fired (sticky until Reset)

vars == <<head, tail, first, last, start, len, ticket, ticketNext, live, order, exhausted>>

\* rn_span_round16
Round16(b) == ((b + 15) \div 16) * 16

\* rn_span_place: [ok, at]
Place(l16) ==
    IF l16 = 0 \/ l16 > SR THEN [ok |-> FALSE, at |-> 0]
    ELSE IF first = NONE THEN [ok |-> TRUE, at |-> 0]          \* nothing live: start over
    ELSE IF tail > head THEN
        IF l16 <= SR - tail THEN [ok |-> TRUE, at |-> tail]
        ELSE IF PadOnWrap /\ l16 <= head THEN [ok |-> TRUE, at |-> 0]   \* pad to the end
        ELSE IF ~PadOnWrap /\ l16 <= (SR - tail) + head THEN [ok |-> TRUE, at |-> tail]
        ELSE [ok |-> FALSE, at |-> 0]
    \* the tail has wrapped behind the head, or met it (full)
    ELSE IF l16 <= head - tail THEN [ok |-> TRUE, at |-> tail]
    ELSE [ok |-> FALSE, at |-> 0]

Init ==
    /\ head = 0
    /\ tail = 0
    /\ first = NONE
    /\ last = NONE
    /\ start = [d \in D |-> 0]
    /\ len = [d \in D |-> 0]
    /\ ticket = [d \in D |-> 0]
    /\ ticketNext = 1
    /\ live = {}
    /\ order = <<>>
    /\ exhausted = FALSE

\* rn_region_send_alloc_span with room: rn_span_place then rn_span_link
Alloc(l, d) ==
    LET l16 == Round16(l)
        r   == Place(l16)
        s   == r.at
        e   == s + l16
    IN
    /\ ~exhausted
    /\ d \in D \ live
    /\ r.ok
    /\ start' = [start EXCEPT ![d] = s]
    /\ len'   = [len EXCEPT ![d] = l16]
    /\ ticket' = [ticket EXCEPT ![d] = ticketNext]
    /\ ticketNext' = ticketNext + 1
    /\ first' = IF last = NONE THEN d ELSE first
    /\ head'  = IF last = NONE THEN s
                ELSE IF FloorAtOldest THEN head ELSE s
    /\ last'  = d
    /\ tail'  = IF e = SR THEN 0 ELSE e
    /\ live'  = live \cup {d}
    /\ order' = Append(order, d)
    /\ UNCHANGED exhausted

\* rn_region_send_alloc_span without room: QUEUE_FULL or STORE_FULL, then
\* rn_region_exhaust; the connection is kicked and nothing more is sent.
Exhaust(l) ==
    /\ ~exhausted
    /\ \/ D \ live = {}
       \/ ~Place(Round16(l)).ok
    /\ exhausted' = TRUE
    /\ UNCHANGED <<head, tail, first, last, start, len, ticket, ticketNext, live, order>>

\* rn_span_unlink, from rn_region_send_free or the last fragment's
\* rn_region_send_free_fragment; any live span, in any order.  The freed
\* descriptor's ticket clears; when an edge span goes, the new edge is the
\* live span with the smallest (or largest) remaining ticket.
Free(d) ==
    /\ d \in live
    /\ LET rest == live \ {d}
           ord2 == SelectSeq(order, LAMBDA x : x # d)
           f2 == IF rest = {} THEN NONE
                 ELSE CHOOSE x \in rest : \A y \in rest : ticket[x] <= ticket[y]
           l2 == IF rest = {} THEN NONE
                 ELSE CHOOSE x \in rest : \A y \in rest : ticket[y] <= ticket[x]
       IN /\ first' = f2
          /\ last' = l2
          /\ ticket' = [x \in D |-> IF x \in rest THEN (CHOOSE i \in 1..Len(ord2) : ord2[i] = x) ELSE 0]
          /\ ticketNext' = Len(ord2) + 1
          /\ IF f2 = NONE
               THEN /\ head' = 0                                   \* empty: the floor is the start
                    /\ tail' = 0
               ELSE /\ head' = IF ~FloorAtOldest THEN start[l2]
                               ELSE IF d = first THEN start[f2] ELSE head
                    /\ UNCHANGED tail
          /\ live' = rest
          /\ order' = ord2
          /\ UNCHANGED <<start, len, exhausted>>

\* the region reset (rocketnet_allocator.cpp, "the send spans: empty")
Reset ==
    /\ head' = 0
    /\ tail' = 0
    /\ first' = NONE
    /\ last' = NONE
    /\ start' = [d \in D |-> 0]
    /\ len' = [d \in D |-> 0]
    /\ ticket' = [d \in D |-> 0]
    /\ ticketNext' = 1
    /\ live' = {}
    /\ order' = <<>>
    /\ exhausted' = FALSE

Next ==
    \/ \E l \in Lengths, d \in D : Alloc(l, d)
    \/ \E l \in Lengths : Exhaust(l)
    \/ \E d \in D : Free(d)
    \/ Reset

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
\* Invariants

TypeOK ==
    /\ head \in 0..(SR - 1)
    /\ tail \in 0..(2 * SR)          \* loose so the no-padding witness meets SpanInsideRing
    /\ first \in D \cup {NONE}
    /\ last \in D \cup {NONE}
    /\ start \in [D -> 0..(SR - 1)]
    /\ len \in [D -> 0..SR]
    /\ ticket \in [D -> Nat]
    /\ ticketNext \in Nat \ {0}
    /\ live \subseteq D
    /\ exhausted \in BOOLEAN

Bytes(d) == { (start[d] + i) % SR : i \in 0..(len[d] - 1) }

\* No two live spans share a byte.
LiveSpansDisjoint ==
    \A a, b \in live : a # b => Bytes(a) \cap Bytes(b) = {}

\* Every live span lies inside the ring without crossing its end.
SpanInsideRing ==
    \A d \in live : len[d] > 0 /\ start[d] + len[d] <= SR

\* Spans start and end on 16-byte boundaries, and so do head and tail.
Aligned ==
    /\ \A d \in live : start[d] % 16 = 0 /\ len[d] % 16 = 0
    /\ head % 16 = 0
    /\ tail % 16 = 0

\* The floor is the oldest live span's start.
HeadIsOldestStart ==
    first # NONE => head = start[first]

\* first..last name the live spans with the smallest and largest tickets,
\* tickets increase in allocation order, and the ghost order is the live
\* spans sorted by ticket.
TicketOrder ==
    /\ { order[i] : i \in 1..Len(order) } = live
    /\ first = IF order = <<>> THEN NONE ELSE order[1]
    /\ last = IF order = <<>> THEN NONE ELSE order[Len(order)]
    /\ \A i \in 1..Len(order) : ticket[order[i]] > 0
    /\ \A i, j \in 1..Len(order) : i < j => ticket[order[i]] < ticket[order[j]]

\* An empty ring is all zero (the reset in rn_span_unlink).
EmptyIsZero ==
    first = NONE => head = 0 /\ tail = 0 /\ last = NONE /\ live = {}

\* tail = head on a non-empty ring means full: nothing is admitted.
FullAdmitsNothing ==
    (first # NONE /\ tail = head) => \A l \in Lengths : ~Place(Round16(l)).ok

\* An empty ring admits a maximal message (SR bytes) and every length offered.
IdleAdmitsAnyMessage ==
    first = NONE => /\ Place(SR).ok
                    /\ \A l \in Lengths : Place(Round16(l)).ok

\* The live spans lie in ring order from head to tail: unwrapped (tail >
\* head, or tail = 0 with the newest span ending at SR) they sit inside
\* [head, tail); wrapped they sit in [head, SR) or [0, tail).  This is the
\* fact rn_span_place's two branches rely on.
LiveBetweenHeadAndTail ==
    first # NONE =>
        \A d \in live :
            LET s == start[d] e == start[d] + len[d] IN
            IF tail > head THEN head <= s /\ e <= tail
            ELSE (head <= s) \/ (e <= tail)
=============================================================================
