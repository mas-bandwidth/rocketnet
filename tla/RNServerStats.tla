------------------------------ MODULE RNServerStats ------------------------------
(***************************************************************************)
(* rocketnet: the running per-slot statistics aggregates over the older    *)
(* half of a retained history window (sdk/rocketnet_ack.cpp, the           *)
(* incremental running aggregates).                                        *)
(*                                                                         *)
(* A ring of N records holds, at record i, the newest sequence congruent   *)
(* to i mod N that was written there. The reference recompute              *)
(* (rocketnet_ack_endpoint_update_stats) walks the window                  *)
(*                                                                         *)
(*   [base, min(base + H - 1, highest)],  base = max(1, highest - N + 1)   *)
(*                                                                         *)
(* with H = N / 2, and counts the present records in it: how many, how     *)
(* many unacknowledged, the lowest and highest present and acknowledged    *)
(* sequences, and the earliest and latest times. The running aggregates    *)
(* keep the same numbers without the walk: a write first slides the window *)
(* to the base its sequence implies (each step removes the record leaving  *)
(* at the low edge and inserts the one entering at the top edge; a jump of *)
(* H or more re-counts the new window), then retires the record it         *)
(* replaces if that one is counted, writes, raises highest and counts the  *)
(* new record if it lands in the window. Removal moves the bounds by       *)
(* scanning the ring for the next present sequence, as the code does.      *)
(*                                                                         *)
(* Two sides share the machine. The sent side (Late = FALSE) writes in     *)
(* increasing sequence order, with gaps (burned sequences) and jumps, and  *)
(* acknowledgments arrive for any held record in any order; its times      *)
(* follow its sequences, so the published spans are read from the          *)
(* sequence bounds (SeqTimes = TRUE). The received side (Late = TRUE)      *)
(* writes any sequence at any time: reordered, late, duplicate, or older   *)
(* than the window. Its times do not follow its sequences, so the counted  *)
(* records are kept on a list ordered by write time (inserted by walking   *)
(* back from the tail) and the span is read from the list's ends           *)
(* (SeqTimes = FALSE). Times are the write count: one clock, in call       *)
(* order.                                                                  *)
(*                                                                         *)
(* A write runs as micro-steps (Send, then Slide steps, then Write) under  *)
(* the endpoint's single owner; the properties are stated at rest          *)
(* (pc = "idle"), where the transports read the published numbers.         *)
(*                                                                         *)
(* BrokenEntering is the defect the first incremental version shipped:     *)
(* the entering record was tested against the window before the base       *)
(* moved, so it was never counted and the window drained.                  *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets, Sequences

CONSTANTS
    N,                  \* ring records (even)
    MaxSeq,             \* highest sequence written
    MaxWrites,          \* writes in one behaviour
    Late,               \* TRUE: the received side's writes, any sequence
    SeqTimes,           \* TRUE: spans read from sequence bounds, not the list
    BrokenEntering      \* defect switch: entering record tested before the base moves

ASSUME N \in Nat /\ N >= 2 /\ N % 2 = 0
ASSUME MaxSeq \in Nat /\ MaxSeq >= 1 /\ MaxWrites \in Nat
ASSUME Late \in BOOLEAN /\ SeqTimes \in BOOLEAN /\ BrokenEntering \in BOOLEAN

H == N \div 2
Seqs == 1..MaxSeq

VARIABLES
    ring,       \* record i: [seq, acked, time]; seq 0 = empty
    highest,    \* highest sequence written
    base,       \* low edge of the counted window
    writes,     \* writes so far, also the clock
    agg,        \* the running aggregates
    pc,         \* "idle", "slide" or "write"
    pending     \* the sequence being written

vars == <<ring, highest, base, writes, agg, pc, pending>>

Min(S) == CHOOSE x \in S : \A y \in S : x <= y
Max(S) == CHOOSE x \in S : \A y \in S : x >= y
Target(h) == IF h >= N THEN h - N + 1 ELSE 1

Present(r, s) == s >= 1 /\ r[s % N].seq = s
InWin(s, b, h) == s >= b /\ s <= b + H - 1 /\ s <= h
T(r, s) == r[s % N].time

Empty == [counted |-> {}, cnt |-> 0, unacked |-> 0,
          lo |-> 0, hi |-> 0, alo |-> 0, ahi |-> 0, order |-> << >>]

AckedCount(a) == a.cnt - a.unacked

\* insert after the last list entry written no later than s (the walk back
\* from the tail stops at the first such entry)
InsertByTime(o, r, s) ==
    LET k == Max({0} \cup {i \in 1..Len(o) : T(r, o[i]) <= T(r, s)})
    IN SubSeq(o, 1, k) \o << s >> \o SubSeq(o, k + 1, Len(o))

Ins(a, r, b, h, s) ==
    IF ~InWin(s, b, h) THEN a
    ELSE LET ak == r[s % N].acked
         IN [counted |-> a.counted \cup {s},
             cnt     |-> a.cnt + 1,
             unacked |-> IF ak THEN a.unacked ELSE a.unacked + 1,
             lo      |-> IF a.cnt = 0 \/ s < a.lo THEN s ELSE a.lo,
             hi      |-> IF a.cnt = 0 \/ s > a.hi THEN s ELSE a.hi,
             alo     |-> IF ak /\ ( AckedCount(a) = 0 \/ s < a.alo ) THEN s ELSE a.alo,
             ahi     |-> IF ak /\ ( AckedCount(a) = 0 \/ s > a.ahi ) THEN s ELSE a.ahi,
             order   |-> InsertByTime(a.order, r, s)]

\* the record s is still in the ring while it is removed
Rem(a, r, b, h, s) ==
    IF ~InWin(s, b, h) THEN a
    ELSE IF a.cnt = 1 THEN Empty
    ELSE LET ak  == r[s % N].acked
             u2  == IF ak THEN a.unacked ELSE a.unacked - 1
             lo2 == IF s = a.lo THEN Min({x \in (s + 1)..MaxSeq : Present(r, x)}) ELSE a.lo
             hi2 == IF s # a.lo /\ s = a.hi THEN Max({x \in 1..(s - 1) : Present(r, x)}) ELSE a.hi
             ac2 == ( a.cnt - 1 ) - u2
             AckedAt(x) == Present(r, x) /\ r[x % N].acked
             alo2 == IF ~ak THEN a.alo
                     ELSE IF ac2 = 0 THEN 0
                     ELSE IF s = a.alo THEN Min({x \in (s + 1)..MaxSeq : AckedAt(x)})
                     ELSE a.alo
             ahi2 == IF ~ak THEN a.ahi
                     ELSE IF ac2 = 0 THEN 0
                     ELSE IF s # a.alo /\ s = a.ahi THEN Max({x \in 1..(s - 1) : AckedAt(x)})
                     ELSE a.ahi
         IN [counted |-> a.counted \ {s},
             cnt     |-> a.cnt - 1,
             unacked |-> u2,
             lo      |-> lo2,
             hi      |-> hi2,
             alo     |-> alo2,
             ahi     |-> ahi2,
             order   |-> SelectSeq(a.order, LAMBDA x : x # s)]

RECURSIVE InsAll(_, _, _, _, _)
InsAll(a, r, b, h, S) ==
    IF S = {} THEN a
    ELSE LET x == Min(S) IN InsAll(Ins(a, r, b, h, x), r, b, h, S \ {x})

Init ==
    /\ ring = [i \in 0..(N - 1) |-> [seq |-> 0, acked |-> FALSE, time |-> 0]]
    /\ highest = 0
    /\ base = 1
    /\ writes = 0
    /\ agg = Empty
    /\ pc = "idle"
    /\ pending = 0

\* a send writes past the high-water mark (gaps and jumps allowed); a
\* receive writes any sequence
Send(s) ==
    /\ pc = "idle"
    /\ writes < MaxWrites
    /\ IF Late THEN TRUE ELSE s > highest
    /\ pending' = s
    /\ pc' = "slide"
    /\ UNCHANGED <<ring, highest, base, writes, agg>>

Slide ==
    /\ pc = "slide"
    /\ LET h  == IF pending > highest THEN pending ELSE highest
           tg == Target(h)
       IN IF base >= tg THEN
              /\ pc' = "write"
              /\ UNCHANGED <<ring, highest, base, writes, agg, pending>>
          ELSE IF tg - base >= H THEN
              /\ base' = tg
              /\ agg' = InsAll(Empty, ring, tg, highest,
                               {x \in tg..(tg + H - 1) : x <= highest /\ Present(ring, x)})
              /\ pc' = "write"
              /\ UNCHANGED <<ring, highest, writes, pending>>
          ELSE
              LET a1 == IF Present(ring, base) THEN Rem(agg, ring, base, highest, base) ELSE agg
                  a2 == IF BrokenEntering
                        THEN Ins(a1, ring, base, highest, base + H)
                        ELSE IF Present(ring, base + H) THEN Ins(a1, ring, base + 1, highest, base + H)
                        ELSE a1
              IN /\ base' = base + 1
                 /\ agg' = a2
                 /\ UNCHANGED <<ring, highest, writes, pc, pending>>

Write ==
    /\ pc = "write"
    /\ LET i  == pending % N
           a1 == IF Present(ring, ring[i].seq) THEN Rem(agg, ring, base, highest, ring[i].seq) ELSE agg
           r2 == [ring EXCEPT ![i] = [seq |-> pending, acked |-> FALSE, time |-> writes + 1]]
           h2 == IF pending > highest THEN pending ELSE highest
       IN /\ ring' = r2
          /\ highest' = h2
          /\ agg' = Ins(a1, r2, base, h2, pending)
    /\ writes' = writes + 1
    /\ pc' = "idle"
    /\ pending' = 0
    /\ UNCHANGED base

\* an acknowledgment for any held, unacknowledged record of the retained
\* window, in any order (sent side only)
Ack(s) ==
    /\ ~Late
    /\ pc = "idle"
    /\ s >= Target(highest)
    /\ Present(ring, s)
    /\ ~ring[s % N].acked
    /\ ring' = [ring EXCEPT ![s % N].acked = TRUE]
    /\ agg' = IF ~InWin(s, base, highest) THEN agg
              ELSE [agg EXCEPT !.unacked = @ - 1,
                               !.alo = IF AckedCount(agg) = 0 \/ s < @ THEN s ELSE @,
                               !.ahi = IF AckedCount(agg) = 0 \/ s > @ THEN s ELSE @]
    /\ UNCHANGED <<highest, base, writes, pc, pending>>

Done ==
    /\ pc = "idle"
    /\ UNCHANGED vars

Next == ( \E s \in Seqs : Send(s) ) \/ Slide \/ Write \/ ( \E s \in Seqs : Ack(s) ) \/ Done

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
\* the reference: what rocketnet_ack_endpoint_update_stats walks

Win == {s \in Seqs : InWin(s, base, highest) /\ Present(ring, s)}
AckedWin == {s \in Win : ring[s % N].acked}
MinT(S) == Min({T(ring, x) : x \in S})
MaxT(S) == Max({T(ring, x) : x \in S})
Settled == pc = "idle"

TypeOK ==
    /\ ring \in [0..(N - 1) -> [seq : 0..MaxSeq, acked : BOOLEAN, time : 0..MaxWrites]]
    /\ highest \in 0..MaxSeq
    /\ base \in 1..MaxSeq
    /\ writes \in 0..MaxWrites
    /\ pc \in {"idle", "slide", "write"}
    /\ pending \in 0..MaxSeq
    /\ agg.cnt \in 0..MaxSeq /\ agg.unacked \in 0..agg.cnt

\* the base is the one the high-water mark implies
BaseFollowsHighest == Settled => base = Target(highest)

\* the counters count exactly the reference window, never more, never less
CountedIsWindow ==
    Settled =>
        /\ agg.counted = Win
        /\ agg.cnt = Cardinality(Win)
        /\ agg.unacked = Cardinality(Win \ AckedWin)

\* the bounds are the lowest and highest present and acknowledged sequences
BoundsExact ==
    Settled =>
        /\ Win # {} => agg.lo = Min(Win) /\ agg.hi = Max(Win)
        /\ AckedWin # {} => agg.alo = Min(AckedWin) /\ agg.ahi = Max(AckedWin)
        /\ AckedWin = {} => AckedCount(agg) = 0

\* the list holds the counted records once each, in write-time order
OrderedByTime ==
    Settled =>
        /\ {agg.order[i] : i \in 1..Len(agg.order)} = agg.counted
        /\ Len(agg.order) = Cardinality(agg.counted)
        /\ \A i \in 1..(Len(agg.order) - 1) : T(ring, agg.order[i]) <= T(ring, agg.order[i + 1])

\* the published spans are the reference's earliest and latest times
PublishedSpanExact ==
    Settled /\ Win # {} /\ Len(agg.order) > 0 =>
        LET lt == IF SeqTimes THEN T(ring, agg.lo) ELSE T(ring, agg.order[1])
            ht == IF SeqTimes THEN T(ring, agg.hi) ELSE T(ring, agg.order[Len(agg.order)])
        IN lt = MinT(Win) /\ ht = MaxT(Win)

AckedSpanExact ==
    Settled /\ SeqTimes /\ AckedWin # {} =>
        T(ring, agg.alo) = MinT(AckedWin) /\ T(ring, agg.ahi) = MaxT(AckedWin)
=============================================================================
