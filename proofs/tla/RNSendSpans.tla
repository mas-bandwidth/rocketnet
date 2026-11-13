----------------------------- MODULE RNSendSpans -----------------------------
(***************************************************************************)
(* The sender's span ring, as sdk/rocketnet_allocator.cpp builds it:       *)
(* rn_span_place, rn_span_link and rn_span_unlink, reached through         *)
(* rn_region_send_alloc_span (Alloc / Refuse), rn_region_send_free and     *)
(* rn_region_send_free_fragment's last fragment (Free), and the region     *)
(* reset (Reset).                                                          *)
(*                                                                         *)
(* tla/RNRing.tla models the receiver's ring with monotonic positions and  *)
(* a floor at U'.  This module models the other representation: positions *)
(* are offsets in 0..SR-1, the floor is head (the oldest live span's       *)
(* start), the next span goes at tail, the live spans form a list in      *)
(* allocation order linked by descriptor index (span_prev/span_next, ends  *)
(* first and last), frees come in any order and splice out through their  *)
(* links, a span                                                           *)
(* that would cross the end of the ring goes to 0 and leaves the skipped   *)
(* bytes as padding, tail = head on a non-empty ring means full, and the   *)
(* ring resets head and tail to 0 when it empties.                        *)
(*                                                                         *)
(* Descriptors are 1..Q; NONE = 0 stands for RN_DESCRIPTOR_NONE.  The      *)
(* free-descriptor stack is abstracted to "any descriptor not live", a     *)
(* superset of the code's LIFO choice.  order is a ghost: the live         *)
(* descriptors in the order they were allocated, which the links must     *)
(* spell out (ListInAllocationOrder).                                      *)
(*                                                                         *)
(* The region reset (Reset, rn_region_carve) clears the roots only: the    *)
(* descriptors keep their span fields and links as dead bytes, and a span  *)
(* linked later writes every one of its own before anything reads it.     *)
(*                                                                         *)
(* These constants flip one rule each for the reversed witnesses:          *)
(*   FloorAtOldest = FALSE  the floor (head) follows the newest span's     *)
(*                          start instead of the oldest's;                 *)
(*   PadOnWrap     = FALSE  a span that does not fit before the end goes   *)
(*                          at tail and crosses the end, no padding;       *)
(*   SpliceBoth    = FALSE  a free repairs its predecessor's next link but *)
(*                          not its successor's prev link;                 *)
(*   RefuseExhausts = TRUE  a span with no room exhausts the region, the   *)
(*                          v1.0 send-side kick (standard/MESSAGES.md      *)
(*                          m1.7 makes it a refusal that changes nothing); *)
(*   LinkFirst     = TRUE   the start takes a descriptor and links its span *)
(*                          (rn_span_link) before the floor test, and on   *)
(*                          no room undoes it with rn_span_unlink;         *)
(*   UndoTail       = FALSE  freeing the newest span leaves the tail where  *)
(*                          it was (v1.0.0 to v1.1.3): an aborted start's  *)
(*                          bytes stay taken until the older spans go;     *)
(*   LinkClearsNext = FALSE  the link leaves the new span's span_next as   *)
(*                          the descriptor held it, as if the pool were    *)
(*                          clear; a carve leaves a span live at it with   *)
(*                          its links (Reset), so the new newest span      *)
(*                          names a dead next.                             *)
(*                                                                         *)
(* A refused start runs the code's steps in the code's order:              *)
(* rn_region_send_fit_span takes a const region (exhausted, then           *)
(* descriptors_free, then rn_span_place, all reads) and                   *)
(* rn_region_send_refuse counts the refusal: refused is the region's       *)
(* refusal counters summed, modulo CountMod as the code's counters wrap,   *)
(* and the ghost calls counts the application's starts, so that a start is *)
(* a step even when it changes nothing else.  CountMod = 1 drops both.     *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    SR,             \* region->span_bytes, a multiple of 16
    Lengths,        \* message byte counts offered to rn_region_send_alloc_span
    Q,              \* send descriptors (region->queue)
    FloorAtOldest,  \* TRUE in the code
    PadOnWrap,      \* TRUE in the code
    SpliceBoth,     \* TRUE in the code
    RefuseExhausts, \* FALSE in the code (TRUE: v1.0's send-side kick)
    LinkFirst,      \* FALSE in the code (TRUE: link before the floor test, unlink on no room)
    CountMod,       \* the refusal counter and the calls ghost run 0..CountMod-1
    UndoTail,       \* TRUE in the code (FALSE: v1.0.0 to v1.1.3)
    LinkClearsNext  \* TRUE in the code: rn_span_link writes span_next = RN_DESCRIPTOR_NONE

ASSUME SR > 0 /\ SR % 16 = 0
ASSUME Lengths \subseteq 1..SR
ASSUME Q \in Nat \ {0}
ASSUME LinkFirst \in BOOLEAN /\ CountMod \in Nat \ {0} /\ LinkClearsNext \in BOOLEAN

NONE == 0
D == 1..Q
NoUndo == 2 * SR + 1   \* region->span_tail_undo spent (0xFFFFFFFF in the code)

VARIABLES
    head,       \* region->span_head
    tail,       \* region->span_tail
    first,      \* region->span_first
    last,       \* region->span_last
    start,      \* descriptor->span_start
    len,        \* the span's rounded length (len16)
    prv,        \* descriptor->span_prev: the next older live span, NONE at first
    nxt,        \* descriptor->span_next: the next newer live span, NONE at last
    live,       \* descriptors with RN_SEND_FLAG_SPAN set
    order,      \* ghost: live descriptors in allocation order
    exhausted,  \* the region's sticky exhausted flag (sticky until Reset); only
                \* the receive side sets it in the code, so here only the broken
                \* RefuseExhausts does
    refused,    \* the region's refusal counters, summed, modulo CountMod
    calls,      \* ghost: the application's span starts, modulo CountMod
    tailUndo,   \* region->span_tail_undo: the tail before the newest span was linked
    fresh,      \* ghost: the last step was an Alloc (a start not yet finished or aborted)
    admitted    \* ghost: the lengths Place admitted just before that Alloc

vars == <<head, tail, first, last, start, len, prv, nxt, live, order, exhausted, refused, calls,
          tailUndo, fresh, admitted>>

\* The ring as the code reads it: a start, a link and a free read start,
\* len and the links of live spans only (a descriptor's span fields are
\* written by the take's memset and the link before any read), so those of
\* a descriptor that is not live are left out; span_tail_undo is the
\* region's too.
Ring == <<head, tail, first, last, live, order,
          [d \in live |-> prv[d]], [d \in live |-> nxt[d]],
          [d \in live |-> start[d]], [d \in live |-> len[d]], exhausted, tailUndo>>

\* The fingerprint for the passing configurations (VIEW View): a descriptor
\* that is not live holds dead span fields (a carve leaves them; Reset), and
\* with LinkClearsNext no action and no property reads them before the link
\* writes them, so states that differ only there are one state.  The
\* LinkClearsNext = FALSE witness reads a dead span_next and runs without
\* the view.
View == <<Ring, refused, calls, fresh, admitted>>

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
    /\ prv = [d \in D |-> NONE]
    /\ nxt = [d \in D |-> NONE]
    /\ live = {}
    /\ order = <<>>
    /\ exhausted = FALSE
    /\ refused = 0
    /\ calls = 0
    /\ tailUndo = NoUndo
    /\ fresh = FALSE
    /\ admitted = {}

\* rn_region_send_fit_span's decision, read off a const region before
\* anything is taken: not exhausted, a free descriptor, and rn_span_place
FitOK(l) == ~exhausted /\ D \ live # {} /\ Place(Round16(l)).ok

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
    /\ prv' = [prv EXCEPT ![d] = last]
    /\ nxt' = [x \in D |-> IF x = d THEN (IF LinkClearsNext THEN NONE ELSE nxt[d])
                           ELSE IF x = last THEN d ELSE nxt[x]]
    /\ first' = IF last = NONE THEN d ELSE first
    /\ head'  = IF last = NONE THEN s
                ELSE IF FloorAtOldest THEN head ELSE s
    /\ last'  = d
    /\ tail'  = IF e = SR THEN 0 ELSE e
    /\ live'  = live \cup {d}
    /\ order' = Append(order, d)
    /\ calls' = (calls + 1) % CountMod
    /\ tailUndo' = tail
    /\ fresh' = TRUE
    /\ admitted' = {x \in Lengths : Place(Round16(x)).ok}
    /\ UNCHANGED <<exhausted, refused>>

\* rn_region_send_alloc_span without room: QUEUE_FULL or STORE_FULL (or a
\* start on an exhausted region), counted by rn_region_send_refuse. In the
\* code the fit only reads, so the ring, its links and the region are as
\* they were, and the connection goes on (standard/MESSAGES.md m1.7).
\* RefuseExhausts is v1.0's rn_region_exhaust here, the send-side kick.
\* LinkFirst (broken): with a descriptor free, the start links its span at
\* the place it would take without the floor test (rn_span_link: the tail,
\* or 0 when the span would cross the end), then finds no room and undoes
\* the link with rn_span_unlink, the same unlink Free is: the link keeps
\* the tail in span_tail_undo and moves the tail past the span; the unlink
\* splices the newest span out and, the ring not empty, gives the tail back
\* from span_tail_undo and spends it (UndoTail), or leaves the tail where
\* the link put it (UndoTail = FALSE).  The tail undo of the start before
\* is spent either way.
LinkThenUnlink(l, d) ==
    LET l16 == Round16(l)
        s   == IF first = NONE \/ l16 > SR - tail THEN 0 ELSE tail
        e   == s + l16
    IN /\ start' = [start EXCEPT ![d] = s]
       /\ len'   = [len EXCEPT ![d] = l16]
       /\ prv' = [prv EXCEPT ![d] = NONE]
       /\ nxt' = [nxt EXCEPT ![d] = NONE]
       /\ IF first = NONE
            THEN /\ head' = 0                       \* the unlink empties the ring
                 /\ tail' = 0
                 /\ tailUndo' = tail
            ELSE /\ UNCHANGED head
                 /\ IF UndoTail
                      THEN /\ tail' = tail          \* given back from span_tail_undo
                           /\ tailUndo' = NoUndo
                      ELSE /\ tail' = IF e = SR THEN 0 ELSE e
                           /\ tailUndo' = tail
       /\ UNCHANGED <<first, last, live, order>>

Refuse(l) ==
    /\ ~FitOK(l)
    /\ IF LinkFirst /\ ~exhausted /\ D \ live # {}
         THEN \E d \in D \ live : LinkThenUnlink(l, d)
         ELSE UNCHANGED <<head, tail, first, last, start, len, prv, nxt, live, order, tailUndo>>
    /\ exhausted' = (exhausted \/ RefuseExhausts)
    /\ refused' = (refused + 1) % CountMod
    /\ calls' = (calls + 1) % CountMod
    /\ UNCHANGED <<fresh, admitted>>

\* rn_span_unlink, from rn_region_send_free or the last fragment's
\* rn_region_send_free_fragment; any live span, in any order.  The span
\* splices out through its own links: its predecessor (or first) takes its
\* next, its successor (or last) takes its prev, and its links clear.
Free(d) ==
    /\ d \in live
    /\ LET p == prv[d]
           n == nxt[d]
           f2 == IF p = NONE THEN n ELSE first
           l2 == IF n = NONE THEN p ELSE last
       IN /\ first' = f2
          /\ last' = l2
          /\ nxt' = [x \in D |-> IF x = d THEN NONE ELSE IF x = p THEN n ELSE nxt[x]]
          /\ prv' = [x \in D |-> IF x = d THEN NONE ELSE IF x = n /\ SpliceBoth THEN p ELSE prv[x]]
          /\ IF f2 = NONE
               THEN /\ head' = 0                                   \* empty: the floor is the start
                    /\ tail' = 0
                    /\ UNCHANGED tailUndo
               ELSE /\ head' = IF ~FloorAtOldest THEN start[l2]
                               ELSE IF p = NONE THEN start[f2] ELSE head
                    \* TailUndo: the newest span, freed before anything was
                    \* linked after it, gives its bytes back (an aborted start)
                    /\ IF UndoTail /\ n = NONE /\ tailUndo # NoUndo
                         THEN /\ tail' = tailUndo
                              /\ tailUndo' = NoUndo
                         ELSE UNCHANGED <<tail, tailUndo>>
          /\ live' = live \ {d}
          /\ order' = SelectSeq(order, LAMBDA x : x # d)
          /\ fresh' = FALSE
          /\ UNCHANGED <<start, len, exhausted, refused, calls, admitted>>

\* the region reset, rn_region_carve ("the send spans: empty, nothing
\* live"): the roots (span_head, span_tail, span_tail_undo, span_first,
\* span_last) and the exhausted flag only.  The descriptors keep their
\* bytes: a span live at the carve keeps its span_start and its links as
\* dead values, and the carve's descriptors_fresh = 0 forgets every
\* descriptor, so none is live; a later take memsets its descriptor and the
\* link writes every span field before any read.
Reset ==
    /\ head' = 0
    /\ tail' = 0
    /\ first' = NONE
    /\ last' = NONE
    /\ live' = {}
    /\ order' = <<>>
    /\ exhausted' = FALSE
    /\ tailUndo' = NoUndo
    /\ fresh' = FALSE
    /\ admitted' = {}
    /\ UNCHANGED <<start, len, prv, nxt, refused, calls>>

Next ==
    \/ \E l \in Lengths, d \in D : Alloc(l, d)
    \/ \E l \in Lengths : Refuse(l)
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
    /\ prv \in [D -> D \cup {NONE}]
    /\ nxt \in [D -> D \cup {NONE}]
    /\ live \subseteq D
    /\ exhausted \in BOOLEAN
    /\ refused \in 0..(CountMod - 1) /\ calls \in 0..(CountMod - 1)
    /\ tailUndo \in 0..(2 * SR + 1)
    /\ fresh \in BOOLEAN
    /\ admitted \subseteq Lengths

Bytes(d) == { (start[d] + i) % SR : i \in 0..(len[d] - 1) }

\* A span with no room never exhausts the region: only the receive side does
\* (standard/MESSAGES.md m1.7). Reversed by RefuseExhausts.
SpansNeverExhaust == ~exhausted

\* A span start: the alloc with room, or the start the region refuses.
StartStep(l) == (\E d \in D : Alloc(l, d)) \/ Refuse(l)

\* A start the region has no room for, by the fit's decision on the state
\* before it, leaves the ring, its links and the region exactly as they
\* were (standard/MESSAGES.md m1.7, rule 2), whatever it did on the way;
\* the refusal counter alone moves, by one (design (L)1). Reversed by
\* LinkFirst (span_tail left past the unlinked span) and RefuseExhausts.
RefuseLeavesRing ==
    [][\A l \in Lengths : StartStep(l) /\ ~FitOK(l) =>
          /\ Ring' = Ring
          /\ refused' = (refused + 1) % CountMod]_vars

\* An aborted start gives back what it took (rocketnet_client_abort_message,
\* rocketnet_server_abort_message: "releases its memory"): freeing the span
\* the last step allocated leaves the ring admitting every length it admitted
\* before that Alloc. Reversed by UndoTail.
AbortReturnsRoom ==
    [][\A d \in D : (fresh /\ d = last /\ Free(d)) => \A l \in admitted : (Place(Round16(l)).ok)']_vars

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

\* The links spell out the live spans in allocation order: first and last
\* are its ends, and each live span's prev and next are its neighbours in
\* the ghost order.  A span that is not live may keep dead links (a span
\* live at a carve keeps them; an unlinked one has none): nothing reads them.
ListInAllocationOrder ==
    /\ { order[i] : i \in 1..Len(order) } = live
    /\ first = IF order = <<>> THEN NONE ELSE order[1]
    /\ last = IF order = <<>> THEN NONE ELSE order[Len(order)]
    /\ \A i \in 1..Len(order) :
          /\ prv[order[i]] = IF i = 1 THEN NONE ELSE order[i - 1]
          /\ nxt[order[i]] = IF i = Len(order) THEN NONE ELSE order[i + 1]

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
