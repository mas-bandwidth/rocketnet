-------------------------- MODULE RNAckWalkLayout --------------------------
(***************************************************************************)
(* The sender's walk of an acknowledged run over the sent ring's shadow    *)
(* bitmaps, at the code's layout (sdk/rocketnet_ack.cpp:                   *)
(* rocketnet_ack_apply_run, rn_bitmap_range_popcount,                      *)
(* rn_bitmap_walk_bits_desc and rocketnet_ack_stats_lowest_acked).         *)
(*                                                                         *)
(* The sent ring has N = Words * WordBits slots (ROCKETNET_HISTORY_PACKETS *)
(* in 64-bit words in the code); sequence s lives at slot s % N, word      *)
(* slot >> 6, bit slot & 63.  sent_held has a slot's bit set while the     *)
(* slot holds its in-window sequence, sent_unacked while that sequence is  *)
(* also unacknowledged (the reconcile at the top of process_block keeps    *)
(* them so; SessionAcks.tla checks that, slot by slot).                   *)
(*                                                                         *)
(* Part = "run" checks apply_run's arithmetic for any run [low, high] the  *)
(* block names: the part above the highest sent counted unknown            *)
(* arithmetically, the clamp to the retained window, the slot range split  *)
(* where it wraps at the ring end, the held count by masked popcount over  *)
(* words, and the visit of the unacknowledged bits word by word with clz,  *)
(* newest first, each bit mapped back to its sequence by seq_at_a + (slot  *)
(* - a).  The reference is the run read sequence by sequence.              *)
(*                                                                         *)
(* Part = "lowest" checks rocketnet_ack_stats_lowest_acked over [from, to] *)
(* inside one window: a word of held and not unacknowledged bits, shifted  *)
(* to the cursor's slot, skipped whole when empty (to the next word        *)
(* boundary), else the cursor moved by ctz and the candidate checked       *)
(* against its record, since a slot's bits can name a stale record.  The  *)
(* reference is the lowest present acknowledged sequence in the range.     *)
(*                                                                         *)
(* Broken selects a reversed witness:                                      *)
(*   "wrapseq"   the wrapped low-slot piece mapped from low, not from      *)
(*               high - slot_high (VisitsExact);                           *)
(*   "himask"    the last word's mask stops one bit short (VisitsExact);   *)
(*   "skipword"  an empty word skips a whole word, not to the next word    *)
(*               boundary (LowestExact).                                   *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Words,      \* bitmap words: ROCKETNET_HISTORY_PACKETS / 64 in the code
    WordBits,   \* bits per word: 64 in the code
    MaxHighest, \* the highest sent sequence ranges over 1 .. MaxHighest
    Part,       \* "run" or "lowest"
    Broken      \* "none", or the reversed witness

N == Words * WordBits
WB == WordBits
Full == 2^WB - 1
Slots == 0..N-1

ASSUME /\ Words \in Nat \ {0} /\ WordBits \in Nat \ {0} /\ MaxHighest \in Nat \ {0}
       /\ Part \in {"run", "lowest"}
       /\ Broken \in {"none", "wrapseq", "himask", "skipword"}

BitOf(x, i) == (x \div 2^i) % 2
Bit(bm, slot) == BitOf(bm[slot \div WB], slot % WB) = 1
And(x, m) == LET RECURSIVE Go(_)
                 Go(i) == IF i = WB THEN 0
                          ELSE (IF BitOf(x, i) = 1 /\ BitOf(m, i) = 1 THEN 2^i ELSE 0) + Go(i + 1)
             IN Go(0)
Pop(x) == LET RECURSIVE Go(_)
              Go(i) == IF i = WB THEN 0 ELSE BitOf(x, i) + Go(i + 1)
          IN Go(0)
TopBit(x) == LET RECURSIVE Go(_)
                 Go(i) == IF BitOf(x, i) = 1 THEN i ELSE Go(i - 1)
             IN Go(WB - 1)
Ctz(x) == LET RECURSIVE Go(_)
              Go(i) == IF BitOf(x, i) = 1 THEN i ELSE Go(i + 1)
          IN Go(0)
Shr(x, n) == x \div 2^n
Shl(x, n) == (x * 2^n) % 2^WB
Max(a, b) == IF a > b THEN a ELSE b
Min(a, b) == IF a < b THEN a ELSE b

\* a word of the bitmap masked to the linear slot range [a, b]: the low word
\* from a & 63 up, the high word to b & 63 inclusive
Masked(bm, w, a, b) ==
    LET lo == IF w = a \div WB THEN And(bm[w], Shl(Full, a % WB)) ELSE bm[w]
        hiBit == b % WB
        hiMask == IF Broken = "himask" THEN 2^hiBit - 1
                  ELSE IF hiBit = WB - 1 THEN Full ELSE 2^(hiBit + 1) - 1
    IN IF w = b \div WB THEN And(lo, hiMask) ELSE lo

\* rn_bitmap_range_popcount
RangePop(bm, a, b) ==
    LET RECURSIVE Go(_)
        Go(w) == Pop(Masked(bm, w, a, b)) + (IF w = b \div WB THEN 0 ELSE Go(w + 1))
    IN Go(a \div WB)

\* rn_bitmap_walk_bits_desc: the visited sequences, in visit order
WalkDesc(bm, a, b, seqAtA) ==
    LET RECURSIVE Bits(_, _), Go(_)
        Bits(word, w) == IF word = 0 THEN <<>>
                         ELSE LET bit == TopBit(word)
                              IN <<seqAtA + (w * WB + bit) - a>> \o Bits(word - 2^bit, w)
        Go(w) == Bits(Masked(bm, w, a, b), w) \o (IF w = a \div WB THEN <<>> ELSE Go(w - 1))
    IN Go(b \div WB)

VARIABLES
    highest,    \* endpoint->highest_sequence
    held,       \* endpoint->sent_held, word -> value
    unacked,    \* endpoint->sent_unacked, word -> value
    low, high,  \* the run the block names (Part = "run")
    state       \* slot -> "empty", "acked", "unacked" or "stale" (Part = "lowest")

vars == <<highest, held, unacked, low, high, state>>

WindowLow == IF highest >= N THEN highest - N + 1 ELSE 1

\* ------------------------------------------------------------------ run --

\* rocketnet_ack_apply_run's arithmetic: [unknown count, visit order]
ApplyRun ==
    LET aboveUnknown == IF high > highest THEN high - Max(low, highest + 1) + 1 ELSE 0
        hi == Min(high, highest)
    IN IF low > highest \/ hi < WindowLow THEN [unknown |-> aboveUnknown, visits |-> <<>>]
       ELSE
       LET lo == Max(low, WindowLow)
           len == hi - lo + 1
           sLow == lo % N
           sHigh == hi % N
           heldCount == IF sHigh >= sLow THEN RangePop(held, sLow, sHigh)
                        ELSE RangePop(held, sLow, N - 1) + RangePop(held, 0, sHigh)
           visits == IF sHigh >= sLow THEN WalkDesc(unacked, sLow, sHigh, lo)
                     ELSE WalkDesc(unacked, 0, sHigh, IF Broken = "wrapseq" THEN lo ELSE hi - sHigh)
                          \o WalkDesc(unacked, sLow, N - 1, lo)
       IN [unknown |-> aboveUnknown + len - heldCount, visits |-> visits]

\* the reference: the named run read sequence by sequence
RefNamed == {s \in low..high : s >= WindowLow /\ s <= highest}
RefUnknown == Cardinality({s \in low..high : s > highest})
              + Cardinality({s \in RefNamed : ~Bit(held, s % N)})
RECURSIVE RefVisits(_)
RefVisits(s) == IF s < low THEN <<>>
                ELSE (IF s \in RefNamed /\ Bit(unacked, s % N) THEN <<s>> ELSE <<>>) \o RefVisits(s - 1)

\* --------------------------------------------------------------- lowest --

\* the shadow bits of each slot state; a stale slot is held and not
\* unacknowledged but its record holds another sequence
HeldOf(st) == [w \in 0..Words-1 |-> LET RECURSIVE Go(_)
                                        Go(i) == IF i = WB THEN 0
                                                 ELSE (IF st[w * WB + i] # "empty" THEN 2^i ELSE 0) + Go(i + 1)
                                    IN Go(0)]
UnackedOf(st) == [w \in 0..Words-1 |-> LET RECURSIVE Go(_)
                                           Go(i) == IF i = WB THEN 0
                                                    ELSE (IF st[w * WB + i] = "unacked" THEN 2^i ELSE 0) + Go(i + 1)
                                       IN Go(0)]

\* rocketnet_ack_stats_lowest_acked over [from, to]; present and acked is
\* the slot's state "acked" (its record holds its in-window sequence)
LowestAcked(from, to) ==
    LET h == HeldOf(state)
        u == UnackedOf(state)
        RECURSIVE Go(_)
        Go(s) == IF s > to THEN 0
                 ELSE LET slot == s % N
                          word == Shr(And(h[slot \div WB], Full - u[slot \div WB]), slot % WB)
                      IN IF word = 0
                           THEN Go(s + (IF Broken = "skipword" THEN WB ELSE WB - (slot % WB)))
                           ELSE LET c == s + Ctz(word)
                                IN IF c > to THEN 0
                                   ELSE IF state[c % N] = "acked" THEN c
                                   ELSE Go(c + 1)
    IN Go(from)

RefLowest(from, to) ==
    LET S == {s \in from..to : state[s % N] = "acked"}
    IN IF S = {} THEN 0 ELSE CHOOSE s \in S : \A t \in S : s <= t

\* ---------------------------------------------------------------- spec --

Init ==
    /\ highest \in 1..MaxHighest
    /\ IF Part = "run"
         THEN /\ held \in [0..Words-1 -> 0..Full]
              /\ unacked \in {u \in [0..Words-1 -> 0..Full] : \A w \in 0..Words-1 : And(u[w], Full - held[w]) = 0}
              /\ low \in 1..(highest + 2)
              /\ high \in low..(highest + 2)
              /\ state = [s \in Slots |-> "empty"]
         ELSE /\ held = [w \in 0..Words-1 |-> 0]
              /\ unacked = [w \in 0..Words-1 |-> 0]
              \* from = a window base, to = the window top it ends at
              /\ low \in WindowLow..highest
              /\ high \in low..Min(highest, low + N - 1)
              /\ state \in [Slots -> {"empty", "acked", "unacked", "stale"}]

\* each input is checked where it stands
Next == UNCHANGED vars

Spec == Init /\ [][Next]_vars

TypeOK == /\ highest \in 1..MaxHighest
          /\ low \in 1..(MaxHighest + 2) /\ high \in low..(MaxHighest + 2)

\* the unknown count and the visits, in order, are the run read sequence by sequence
VisitsExact ==
    Part = "run" => ApplyRun = [unknown |-> RefUnknown, visits |-> RefVisits(high)]

\* the lowest acknowledged record found is the lowest there is
LowestExact ==
    Part = "lowest" => LowestAcked(low, high) = RefLowest(low, high)

=============================================================================
