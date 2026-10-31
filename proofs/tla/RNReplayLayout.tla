--------------------------- MODULE RNReplayLayout ---------------------------
(***************************************************************************)
(* The replay window of one encryption mapping at the code's layout        *)
(* (sdk/rocketnet_replay.cpp: rocketnet_replay_bit,                        *)
(* rocketnet_replay_set_bit, rocketnet_replay_clear_range,                 *)
(* rocketnet_replay_already_received and rocketnet_replay_advance).        *)
(*                                                                         *)
(* The ring is Words words of WordBits bits (64 x 64 in the code; here     *)
(* small).  A sequence lives at bit s % W, in word bit div WordBits at     *)
(* position bit % WordBits.  An advance past the high-water mark clears    *)
(* the bits of the sequences it skips: a skip of W or more clears every    *)
(* word; a shorter one clears the ring positions highest+1 .. s-1, which   *)
(* the code computes as begin = (highest + 1) % W and end = begin +        *)
(* skipped - 1 and clears as [begin, end) when end <= W, else as           *)
(* [begin, W) and [0, end - W).  rocketnet_replay_clear_range clears a     *)
(* range in three parts: a partial first word with the mask                *)
(* ((1 << count) - 1) << offset, whole words, and a partial last word      *)
(* with the mask (1 << (end - begin)) - 1.                                 *)
(*                                                                         *)
(* ReplayWindow.tla checks the protocol with the skipped bits cleared one  *)
(* by one; this module carries the word arithmetic that clears them.       *)
(* received is the ghost: the opened sequences still inside the retained   *)
(* range (highest - W, highest].  WindowIsTruth says each ring bit is set  *)
(* exactly when a received sequence lives there; RefusesExactly says       *)
(* already_received refuses exactly the received and the too old.         *)
(*                                                                         *)
(* Broken selects a reversed witness (WindowIsTruth fails for each):       *)
(*   "nowrap"   a skip that crosses the ring end clears only [begin, W),   *)
(*              so the bits at the start of the ring keep the last lap;    *)
(*   "begin"    begin taken from highest instead of highest + 1, the       *)
(*              cleared range one early: the bit of highest is cleared and *)
(*              the bit of s - 1 kept;                                     *)
(*   "noshift"  the partial first word's mask is not shifted by its        *)
(*              offset, so the wrong bits of that word are cleared.        *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Words,      \* words in the ring: ROCKETNET_HISTORY_PACKETS / 64 in the code
    WordBits,   \* bits per word: 64 in the code
    MaxSeq,     \* the highest sequence this instance opens
    Broken      \* "none", or the reversed witness

W == Words * WordBits   \* ROCKETNET_REPLAY_WINDOW_BITS

ASSUME /\ Words \in Nat \ {0} /\ WordBits \in Nat \ {0}
       /\ MaxSeq \in Nat /\ MaxSeq >= W
       /\ Broken \in {"none", "nowrap", "begin", "noshift"}

Full == 2^WordBits - 1   \* ~0 in one word

\* bit i of x, and x with the bits of mask m cleared: x & ~m
BitOf(x, i) == (x \div 2^i) % 2
AndNot(x, m) == LET RECURSIVE Go(_)
                    Go(i) == IF i = WordBits THEN 0
                             ELSE (IF BitOf(x, i) = 1 /\ BitOf(m, i) = 0 THEN 2^i ELSE 0) + Go(i + 1)
                IN Go(0)

VARIABLES
    highest,    \* replay->highest_opened
    window,     \* replay->window: word index -> the word's value
    received    \* ghost: the opened sequences inside (highest - W, highest]

vars == <<highest, window, received>>

Seqs == 1..MaxSeq

\* rocketnet_replay_bit
TestBit(w, s) == LET bit == s % W IN BitOf(w[bit \div WordBits], bit % WordBits) = 1

\* rocketnet_replay_set_bit
SetBit(w, s) ==
    LET bit == s % W
        k == bit \div WordBits
        p == bit % WordBits
    IN IF BitOf(w[k], p) = 1 THEN w ELSE [w EXCEPT ![k] = @ + 2^p]

\* rocketnet_replay_clear_range: the partial first word, whole words, the
\* partial last word
ClearRange(w0, begin0, end) ==
    IF begin0 = end THEN w0
    ELSE
    LET offset == begin0 % WordBits
        available == WordBits - offset
        count == IF end - begin0 < available THEN end - begin0 ELSE available
        firstMask == IF Broken = "noshift" THEN 2^count - 1 ELSE (2^count - 1) * 2^offset
        w1 == IF offset # 0 THEN [w0 EXCEPT ![begin0 \div WordBits] = AndNot(@, firstMask)] ELSE w0
        begin1 == IF offset # 0 THEN begin0 + count ELSE begin0
        \* while ( end - begin >= 64 ): whole words
        RECURSIVE Whole(_, _)
        Whole(w, b) == IF end - b >= WordBits THEN Whole([w EXCEPT ![b \div WordBits] = 0], b + WordBits)
                       ELSE <<w, b>>
        r == Whole(w1, begin1)
        w2 == r[1]
        begin2 == r[2]
    IN IF begin2 < end
         THEN [w2 EXCEPT ![begin2 \div WordBits] = AndNot(@, 2^(end - begin2) - 1)]
         ELSE w2

\* rocketnet_replay_already_received
AlreadyReceived(s) ==
    IF s > highest THEN FALSE
    ELSE IF highest >= W /\ s <= highest - W THEN TRUE
    ELSE TestBit(window, s)

\* rocketnet_replay_advance, the clear before the set
Cleared(s) ==
    LET skipped == s - highest IN
    IF skipped >= W THEN [k \in 0..Words-1 |-> 0]
    ELSE
    LET begin == IF Broken = "begin" THEN highest % W ELSE (highest + 1) % W
        end == begin + skipped - 1
    IN IF end <= W THEN ClearRange(window, begin, end)
       ELSE IF Broken = "nowrap" THEN ClearRange(window, begin, W)
       ELSE ClearRange(ClearRange(window, begin, W), 0, end - W)

Init ==
    /\ highest = 0
    /\ window = [k \in 0..Words-1 |-> 0]
    /\ received = {}

\* a sequence the window does not refuse opens and is marked: past the
\* high-water mark the skipped bits are cleared and the mark advances; at or
\* below it only the bit is set (the caller checked already_received first)
Advance(s) ==
    /\ ~AlreadyReceived(s)
    /\ IF s > highest
         THEN /\ window' = SetBit(Cleared(s), s)
              /\ highest' = s
         ELSE /\ window' = SetBit(window, s)
              /\ highest' = highest
    /\ received' = {t \in received \cup {s} : t + W > highest'}

\* nothing left to open: a terminal stutter
Done ==
    /\ \A s \in Seqs : AlreadyReceived(s)
    /\ UNCHANGED vars

Next == (\E s \in Seqs : Advance(s)) \/ Done

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ highest \in 0..MaxSeq
    /\ window \in [0..Words-1 -> 0..Full]
    /\ received \subseteq Seqs

\* each ring bit is set exactly when a received sequence lives there
WindowIsTruth ==
    \A b \in 0..W-1 :
        (BitOf(window[b \div WordBits], b % WordBits) = 1) <=> (\E t \in received : t % W = b)

\* already_received refuses exactly the received sequences and the too old
RefusesExactly ==
    \A s \in Seqs : AlreadyReceived(s) <=> (s \in received \/ (s <= highest /\ s + W <= highest))

=============================================================================
