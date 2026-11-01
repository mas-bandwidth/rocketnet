-------------------------- MODULE RNAckBuildLayout --------------------------
(***************************************************************************)
(* The ack block builder at the code's layout (sdk/rocketnet_ack.cpp,      *)
(* rocketnet_ack_block_build).                                             *)
(*                                                                         *)
(* The builder reads the ack-eligible replay window (Words words of        *)
(* WordBits bits; 64 x 64 in the code) from the largest received sequence  *)
(* down to the floor, max(1, largest - W + 1), in one descending pass over *)
(* the words.  Each word is loaded once: bit_index = cursor % W, offset =  *)
(* bit_index % 64, chunk = min(offset + 1, remaining), the mask keeps the  *)
(* bits low_bit .. offset with low_bit = offset - chunk + 1, and the       *)
(* masked word is shifted left by 63 - offset so the cursor's bit is the   *)
(* top bit.  Three phases count leading ones (the first run, then a run)   *)
(* or leading zeros (a gap) with clz, carrying the counts across words; a  *)
(* run that ends emits (gap, run), the range count stops at max_ranges,    *)
(* and a run that reaches the floor is emitted at the end.                 *)
(*                                                                         *)
(* The inputs are any window: any largest, any word values with the       *)
(* largest's bit set (an advance always sets it), and any max_ranges.      *)
(* Bits for sequences outside floor .. largest are arbitrary, so a read    *)
(* past the floor shows.  The reference is the bit-by-bit walk the word    *)
(* pass replaced: from largest down to the floor, the first run, then      *)
(* (gap, run) pairs, a trailing gap dropped, the first max_ranges pairs    *)
(* kept.  ReplayWindow.tla and SessionAcks.tla take the block as that    *)
(* set; this module checks that the word pass builds it.                  *)
(*                                                                         *)
(* Broken selects a reversed witness:                                      *)
(*   "noclamp"  the chunk is not clamped to the remaining count, so the    *)
(*              last word is read past the floor (BlockIsReference);       *)
(*   "cap"      the range count stops one past max_ranges (RangesFit);     *)
(*   "noflush"  a run that reaches the floor is not emitted                *)
(*              (BlockIsReference).                                        *)
(***************************************************************************)
EXTENDS Integers, Sequences

CONSTANTS
    Words,      \* window words: ROCKETNET_HISTORY_PACKETS / 64 in the code
    WordBits,   \* bits per word: 64 in the code
    MaxLargest, \* the largest received sequence ranges over 1 .. MaxLargest
    MaxRanges,  \* max_ranges ranges over 0 .. MaxRanges
    Broken      \* "none", or the reversed witness

W == Words * WordBits
WB == WordBits
Full == 2^WB - 1

ASSUME /\ Words \in Nat \ {0} /\ WordBits \in Nat \ {0}
       /\ MaxLargest \in Nat \ {0} /\ MaxRanges \in Nat
       /\ Broken \in {"none", "noclamp", "cap", "noflush"}

BitOf(x, i) == (x \div 2^i) % 2
And(x, m) == LET RECURSIVE Go(_)
                 Go(i) == IF i = WB THEN 0
                          ELSE (IF BitOf(x, i) = 1 /\ BitOf(m, i) = 1 THEN 2^i ELSE 0) + Go(i + 1)
             IN Go(0)

\* the leading zeros of a WB-bit value; WB for zero (the code's "== 0 ? 64")
Clz(x) == IF x = 0 THEN WB
          ELSE LET RECURSIVE Top(_)
                   Top(i) == IF BitOf(x, i) = 1 THEN i ELSE Top(i - 1)
               IN WB - 1 - Top(WB - 1)

\* x << n in one WB-bit word
Shl(x, n) == IF n >= WB THEN 0 ELSE (x * 2^n) % 2^WB

VARIABLES
    largest, win, maxRanges,                \* the inputs
    cursor, remaining, shifted, wordBits,   \* the pass
    firstRun, curGap, curRun, phase, done,
    ranges,                                 \* block.range_gap / range_length so far
    pc                                      \* "loop" or "end"

vars == <<largest, win, maxRanges, cursor, remaining, shifted, wordBits,
          firstRun, curGap, curRun, phase, done, ranges, pc>>

Floor(h) == IF h >= W THEN h - W + 1 ELSE 1

Rcv(s) == LET b == s % W IN BitOf(win[b \div WB], b % WB) = 1

\* the bit-by-bit walk: the first run, then (gap, run) pairs to the floor
RECURSIVE Walk(_, _, _, _, _, _)
Walk(s, ph, first, gap, run, acc) ==
    IF s < Floor(largest)
      THEN [first |-> first, ranges |-> IF ph = 2 THEN Append(acc, <<gap, run>>) ELSE acc]
    ELSE IF ph = 0
      THEN IF Rcv(s) THEN Walk(s - 1, 0, first + 1, 0, 0, acc)
           ELSE Walk(s - 1, 1, first, 1, 0, acc)
    ELSE IF ph = 1
      THEN IF Rcv(s) THEN Walk(s - 1, 2, first, gap, 1, acc)
           ELSE Walk(s - 1, 1, first, gap + 1, 0, acc)
    ELSE IF Rcv(s) THEN Walk(s - 1, 2, first, gap, run + 1, acc)
         ELSE Walk(s - 1, 1, first, 1, 0, Append(acc, <<gap, run>>))

Reference ==
    LET r == Walk(largest, 0, 0, 0, 0, <<>>)
        n == IF Len(r.ranges) < maxRanges THEN Len(r.ranges) ELSE maxRanges
    IN [first |-> r.first, ranges |-> SubSeq(r.ranges, 1, n)]

Init ==
    /\ largest \in 1..MaxLargest
    /\ win \in [0..Words-1 -> 0..Full]
    /\ Rcv(largest)
    /\ maxRanges \in 0..MaxRanges
    /\ cursor = largest
    /\ remaining = largest - Floor(largest) + 1
    /\ shifted = 0
    /\ wordBits = 0
    /\ firstRun = 0 /\ curGap = 0 /\ curRun = 0
    /\ phase = 0
    /\ done = FALSE
    /\ ranges = <<>>
    /\ pc = "loop"

\* one iteration of the while loop: load a word when the last is used up,
\* then one phase step
Step ==
    /\ pc = "loop"
    /\ remaining > 0 /\ ~done
    /\ LET bitIndex == cursor % W
           offset == bitIndex % WB
           chunk0 == offset + 1
           chunk == IF Broken # "noclamp" /\ chunk0 > remaining THEN remaining ELSE chunk0
           lowBit == offset - chunk + 1
           mask == And(Full - (2^lowBit - 1), IF offset = WB - 1 THEN Full ELSE 2^(offset + 1) - 1)
           sh == IF wordBits = 0 THEN Shl(And(win[bitIndex \div WB], mask), WB - 1 - offset) ELSE shifted
           wbits == IF wordBits = 0 THEN chunk ELSE wordBits
           before == wbits
       IN IF phase = 1
            THEN LET zeros == Clz(sh)
                     take == IF zeros < wbits THEN zeros ELSE wbits
                 IN /\ curGap' = curGap + take
                    /\ shifted' = Shl(sh, take)
                    /\ wordBits' = wbits - take
                    /\ cursor' = cursor - take
                    /\ remaining' = remaining - take
                    /\ phase' = IF zeros < before THEN 2 ELSE 1
                    /\ UNCHANGED <<firstRun, curRun, done, ranges>>
            ELSE LET ones == Clz(Full - sh)
                     take == IF ones < wbits THEN ones ELSE wbits
                     ended == ones < before
                     cap == IF Broken = "cap" THEN Len(ranges) + 1 > maxRanges
                            ELSE Len(ranges) + 1 >= maxRanges
                 IN /\ shifted' = Shl(sh, take)
                    /\ wordBits' = wbits - take
                    /\ cursor' = cursor - take
                    /\ remaining' = remaining - take
                    /\ IF phase = 0
                         THEN /\ firstRun' = firstRun + take
                              /\ curRun' = curRun
                              /\ phase' = IF ended THEN 1 ELSE 0
                              /\ done' = (ended /\ maxRanges = 0)
                              /\ UNCHANGED <<curGap, ranges>>
                         ELSE /\ firstRun' = firstRun
                              /\ IF ended
                                   THEN /\ ranges' = Append(ranges, <<curGap, curRun + take>>)
                                        /\ curGap' = 0
                                        /\ curRun' = 0
                                        /\ phase' = 1
                                        /\ done' = cap
                                   ELSE /\ curRun' = curRun + take
                                        /\ UNCHANGED <<curGap, phase, done, ranges>>
    /\ UNCHANGED <<largest, win, maxRanges, pc>>

\* after the loop: a run that ended at the floor is still stated
Finish ==
    /\ pc = "loop"
    /\ ~(remaining > 0 /\ ~done)
    /\ IF Broken # "noflush" /\ phase = 2 /\ curRun > 0 /\ Len(ranges) < maxRanges
         THEN ranges' = Append(ranges, <<curGap, curRun>>)
         ELSE UNCHANGED ranges
    /\ pc' = "end"
    /\ UNCHANGED <<largest, win, maxRanges, cursor, remaining, shifted, wordBits,
                   firstRun, curGap, curRun, phase, done>>

Next == Step \/ Finish

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ largest \in 1..MaxLargest
    /\ phase \in 0..2
    /\ pc \in {"loop", "end"}
    /\ shifted \in 0..Full

\* never more ranges than the capacity holds
RangesFit == Len(ranges) <= maxRanges

\* the finished block is the bit-by-bit walk's
BlockIsReference ==
    pc = "end" => [first |-> firstRun, ranges |-> ranges] = Reference

\* largest itself is always received, so the first run is at least one
FirstRunNonEmpty == pc = "end" => firstRun >= 1

=============================================================================
