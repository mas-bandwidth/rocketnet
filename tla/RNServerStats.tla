------------------------------ MODULE RNServerStats ------------------------------
(***************************************************************************)
(* rocketnet: the running per-slot statistics aggregates over the older    *)
(* half of a retained history window (sdk/rocketnet_ack.cpp, the running   *)
(* stats aggregates).                                                      *)
(*                                                                         *)
(* A ring of N records holds, at record i, the newest sequence congruent   *)
(* to i mod N that was written there. The reference recompute              *)
(* (rocketnet_ack_endpoint_update_stats) walks the window                  *)
(*                                                                         *)
(*   [base, top],  base = max(1, highest - N + 1),                         *)
(*                 top = min(highest, base + H - 1)                        *)
(*                                                                         *)
(* with H = N / 2, and counts the present records in it: how many, how     *)
(* many unacknowledged, the lowest and highest present and acknowledged    *)
(* sequences, the earliest and latest times, and, for loss, the records    *)
(* in its loss-eligible part (sequence <= highest - H) and how many of     *)
(* those are unacknowledged. The running aggregates keep the same numbers  *)
(* without the walk. Every edge is a function of highest, so when a write  *)
(* raises highest the window moves in one step, before anything is         *)
(* overwritten: the counted records below the new base leave (all of them  *)
(* at once, in O(1), when the new base is past the old top), the counted   *)
(* survivors crossing into the eligible part are counted there, and the    *)
(* present records between the old top and the new one (no higher than    *)
(* the old highest: nothing above it is written yet) enter. Then the write *)
(* retires the record it replaces if that one is counted, writes, and      *)
(* counts the new record if it lands in the window.                        *)
(*                                                                         *)
(* Two sides share the machine. The sent side (Late = FALSE) writes above  *)
(* highest, with gaps (burned sequences) and jumps; with Reorder = TRUE it *)
(* also writes below highest, as concurrent fill writers record their      *)
(* sequences out of order. Acknowledgments arrive for any held record in   *)
(* any order. In-order sends have times that follow their sequences, so    *)
(* the spans are read from the sequence bounds (SeqTimes = TRUE); the code *)
(* gives out-of-order sends of one fill phase one time, which this model's *)
(* write-count clock does not, so the Reorder instance checks the counts   *)
(* and bounds, not the spans. The received side (Late = TRUE) writes any   *)
(* sequence at any time: reordered, late, duplicate, or older than the     *)
(* window. Its times do not follow its sequences, so the counted records   *)
(* are kept on a list ordered by write time (inserted by walking back from *)
(* the tail) and the span is read from the list's ends (SeqTimes = FALSE). *)
(* Times are the write count: one clock, in call order.                    *)
(*                                                                         *)
(* A write runs as two micro-steps (Slide, then Write) under the           *)
(* endpoint's single owner; the properties are stated at rest              *)
(* (pc = "idle"), where the transports read the published numbers.         *)
(*                                                                         *)
(* BrokenEntering is the defect the first incremental version shipped:     *)
(* the record entering the window's top edge is never counted, so the      *)
(* window drains. BrokenCrossing forgets the survivors crossing into the   *)
(* loss-eligible part, so loss is counted over too few records.            *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets, Sequences

CONSTANTS
    N,                  \* ring records (even)
    MaxSeq,             \* highest sequence written
    MaxWrites,          \* writes in one behaviour
    Late,               \* TRUE: the received side's writes, any sequence
    Reorder,            \* TRUE: sent side writes below highest too
    SeqTimes,           \* TRUE: spans read from sequence bounds, not the list
    BrokenEntering,     \* defect switch: the entering records are never counted
    BrokenCrossing      \* defect switch: survivors never cross into the eligible part

ASSUME N \in Nat /\ N >= 2 /\ N % 2 = 0
ASSUME MaxSeq \in Nat /\ MaxSeq >= 1 /\ MaxWrites \in Nat
ASSUME Late \in BOOLEAN /\ Reorder \in BOOLEAN /\ SeqTimes \in BOOLEAN
ASSUME BrokenEntering \in BOOLEAN /\ BrokenCrossing \in BOOLEAN

H == N \div 2
Seqs == 1..MaxSeq

VARIABLES
    ring,       \* record i: [seq, acked, time]; seq 0 = empty
    highest,    \* highest sequence written (raised by Slide, before Write)
    base,       \* low edge of the counted window
    writes,     \* writes so far, also the clock
    agg,        \* the running aggregates
    pc,         \* "idle", "slide" or "write"
    pending     \* the sequence being written

vars == <<ring, highest, base, writes, agg, pc, pending>>

Min(S) == CHOOSE x \in S : \A y \in S : x <= y
Max(S) == CHOOSE x \in S : \A y \in S : x >= y
Target(h) == IF h >= N THEN h - N + 1 ELSE 1
Top(h) == IF h < Target(h) + H - 1 THEN h ELSE Target(h) + H - 1
Eligible(h) == IF h > H THEN h - H ELSE 0

Present(r, s) == s >= 1 /\ r[s % N].seq = s
InWin(s, b, h) == s >= b /\ s <= b + H - 1 /\ s <= h
T(r, s) == r[s % N].time

Empty == [counted |-> {}, cnt |-> 0, unacked |-> 0, ecnt |-> 0, eunacked |-> 0,
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
             el == s <= Eligible(h)
         IN [counted  |-> a.counted \cup {s},
             cnt      |-> a.cnt + 1,
             unacked  |-> IF ak THEN a.unacked ELSE a.unacked + 1,
             ecnt     |-> IF el THEN a.ecnt + 1 ELSE a.ecnt,
             eunacked |-> IF el /\ ~ak THEN a.eunacked + 1 ELSE a.eunacked,
             lo       |-> IF a.cnt = 0 \/ s < a.lo THEN s ELSE a.lo,
             hi       |-> IF a.cnt = 0 \/ s > a.hi THEN s ELSE a.hi,
             alo      |-> IF ak /\ ( AckedCount(a) = 0 \/ s < a.alo ) THEN s ELSE a.alo,
             ahi      |-> IF ak /\ ( AckedCount(a) = 0 \/ s > a.ahi ) THEN s ELSE a.ahi,
             order    |-> InsertByTime(a.order, r, s)]

\* the record s is still in the ring while it is removed
Rem(a, r, b, h, s) ==
    IF ~InWin(s, b, h) THEN a
    ELSE IF a.cnt = 1 THEN Empty
    ELSE LET ak  == r[s % N].acked
             el  == s <= Eligible(h)
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
         IN [counted  |-> a.counted \ {s},
             cnt      |-> a.cnt - 1,
             unacked  |-> u2,
             ecnt     |-> IF el THEN a.ecnt - 1 ELSE a.ecnt,
             eunacked |-> IF el /\ ~ak THEN a.eunacked - 1 ELSE a.eunacked,
             lo       |-> lo2,
             hi       |-> hi2,
             alo      |-> alo2,
             ahi      |-> ahi2,
             order    |-> SelectSeq(a.order, LAMBDA x : x # s)]

\* a counted survivor crossing into the loss-eligible part
Cross(a, r, s) ==
    [a EXCEPT !.ecnt = @ + 1,
              !.eunacked = IF r[s % N].acked THEN @ ELSE @ + 1]

RECURSIVE InsAll(_, _, _, _, _)
InsAll(a, r, b, h, S) ==
    IF S = {} THEN a
    ELSE LET x == Min(S) IN InsAll(Ins(a, r, b, h, x), r, b, h, S \ {x})

RECURSIVE RemAll(_, _, _, _, _)
RemAll(a, r, b, h, S) ==
    IF S = {} THEN a
    ELSE LET x == Min(S) IN RemAll(Rem(a, r, b, h, x), r, b, h, S \ {x})

RECURSIVE CrossAll(_, _, _)
CrossAll(a, r, S) ==
    IF S = {} THEN a
    ELSE LET x == Min(S) IN CrossAll(Cross(a, r, x), r, S \ {x})

Init ==
    /\ ring = [i \in 0..(N - 1) |-> [seq |-> 0, acked |-> FALSE, time |-> 0]]
    /\ highest = 0
    /\ base = 1
    /\ writes = 0
    /\ agg = Empty
    /\ pc = "idle"
    /\ pending = 0

\* a send writes past the high-water mark (gaps and jumps allowed), or with
\* Reorder anywhere above the retained floor whose record is older (the
\* code's stale refusal); a receive writes any sequence
Send(s) ==
    /\ pc = "idle"
    /\ writes < MaxWrites
    /\ IF Late THEN TRUE
       ELSE IF Reorder THEN s >= Target(highest) /\ ring[s % N].seq < s
       ELSE s > highest
    /\ pending' = s
    /\ pc' = "slide"
    /\ UNCHANGED <<ring, highest, base, writes, agg>>

\* rocketnet_ack_stats_sent_advance / rocketnet_ack_stats_received_advance
Slide ==
    /\ pc = "slide"
    /\ LET h0 == highest
           h1 == IF pending > highest THEN pending ELSE highest
           b0 == Target(h0)
           t0 == Top(h0)
           b1 == Target(h1)
           t1 == Top(h1)
           left  == IF b1 > t0 THEN Empty
                    ELSE RemAll(agg, ring, b0, h0, {x \in b0..(b1 - 1) : Present(ring, x)})
           cross == IF b1 > t0 \/ BrokenCrossing THEN left
                    ELSE CrossAll(left, ring,
                                  {x \in Seqs : x > Eligible(h0) /\ x >= b1 /\ x <= Eligible(h1) /\ x <= t0
                                                /\ Present(ring, x)})
           enter == IF BrokenEntering THEN cross
                    ELSE InsAll(cross, ring, b1, h1,
                                {x \in Seqs : x > t0 /\ x >= b1 /\ x <= t1 /\ x <= h0 /\ Present(ring, x)})
       IN /\ highest' = h1
          /\ base' = b1
          /\ agg' = IF h1 = h0 THEN agg ELSE enter
    /\ pc' = "write"
    /\ UNCHANGED <<ring, writes, pending>>

Write ==
    /\ pc = "write"
    /\ LET i  == pending % N
           a1 == IF Present(ring, ring[i].seq) THEN Rem(agg, ring, base, highest, ring[i].seq) ELSE agg
           r2 == [ring EXCEPT ![i] = [seq |-> pending, acked |-> FALSE, time |-> writes + 1]]
       IN /\ ring' = r2
          /\ agg' = Ins(a1, r2, base, highest, pending)
    /\ writes' = writes + 1
    /\ pc' = "idle"
    /\ pending' = 0
    /\ UNCHANGED <<highest, base>>

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
                               !.eunacked = IF s <= Eligible(highest) THEN @ - 1 ELSE @,
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
EligibleWin == {s \in Win : s <= Eligible(highest)}
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
    /\ agg.ecnt \in 0..agg.cnt /\ agg.eunacked \in 0..agg.ecnt

\* the base is the one the high-water mark implies
BaseFollowsHighest == Settled => base = Target(highest)

\* the counters count exactly the reference window, never more, never less
CountedIsWindow ==
    Settled =>
        /\ agg.counted = Win
        /\ agg.cnt = Cardinality(Win)
        /\ agg.unacked = Cardinality(Win \ AckedWin)

\* the loss counters count exactly the reference's eligible sends
EligibleExact ==
    Settled =>
        /\ agg.ecnt = Cardinality(EligibleWin)
        /\ agg.eunacked = Cardinality(EligibleWin \ AckedWin)

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
