---------------------------- MODULE AckBlockWrap ----------------------------
(***************************************************************************)
(* The ack block's largest received across the wire's sequence wrap       *)
(* (standard/TRANSPORT.md t15; sdk/rocketnet_ack.cpp                       *)
(* rocketnet_ack_block_build, rocketnet_ack_block_write and                *)
(* rocketnet_ack_block_read).                                              *)
(*                                                                         *)
(* One direction of one session.  The sender's session sequences start at *)
(* Start (one in production, any value through the DEV seam               *)
(* ROCKETNET_DEV_SESSION_SEQUENCE_START) and are naturals; the wire       *)
(* carries the low bits, here largest mod Modulus in place of mod 2^32.   *)
(* The receiver opens some of them and answers with an ack block: the     *)
(* wire largest, the first range length (the received run ending at       *)
(* largest) and a range count.  The sender reads the block against its    *)
(* own highest sent sequence, reconstructing the largest as the candidate *)
(* nearest to it (rocketnet_reconstruct_near, sdk/rocketnet_protocol.cpp; *)
(* the larger on a tie).                                                   *)
(*                                                                         *)
(* Ranges below the first are abstracted: the honest builder's count is   *)
(* zero here, and their walk (t15 rules 4 and 5) is the subject of         *)
(* SessionAcks.  Forge writes any block that is not the all-zero one and  *)
(* needs no walk: a zero first range length with a nonzero wire largest   *)
(* or range count, or a first range length of 1 or 2 against any wire     *)
(* largest with no ranges, so a first run longer than the reconstructed   *)
(* largest is on the wire too.  A forged block with both a first run and  *)
(* ranges is not written: its fate is the walk's.  MalformedRefused       *)
(* states t15 rules 2 and 3, the two on the fixed fields, on the block's  *)
(* own fields rather than through Decide; rules 1 (size), 4 and 5 are     *)
(* abstracted.                                                             *)
(*                                                                         *)
(* RunBoundChecked = FALSE drops the reader's refusal of a first range    *)
(* length above the reconstructed largest; the reversed witness            *)
(* MCAckBlockWrapBrokenRunBound.cfg must then violate MalformedRefused.   *)
(*                                                                         *)
(* BrokenHalfRange selects the rule before 2026-10-24: the empty test made *)
(* on the reconstructed largest, so a wire zero read by a sender whose    *)
(* highest sent is at or past Modulus/2 reconstructs to Modulus, reads as *)
(* a nonzero largest with first range length zero, and is refused.        *)
(* The rule is the empty block recognised on its wire fields: wire        *)
(* largest 0, first range length 0, range count 0, at any highest sent.   *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Modulus,            \* the wire field's range (2^32 in the code)
    Start,              \* the first session sequence the sender sends
    MaxSeq,             \* the instance's bound on the sender's sequence
    BrokenHalfRange,    \* TRUE: the empty test on the reconstructed value
    RunBoundChecked     \* TRUE (the code): a first range length above the
                        \* reconstructed largest is refused

ASSUME Modulus >= 4 /\ Modulus % 2 = 0
ASSUME Start >= 1 /\ MaxSeq >= Start
ASSUME BrokenHalfRange \in BOOLEAN
ASSUME RunBoundChecked \in BOOLEAN

Half == Modulus \div 2
Seqs == Start..MaxSeq
NoBlock == [wire |-> 0, frl |-> 0, count |-> 0, honest |-> FALSE, true |-> 0, none |-> TRUE]
NoRead == [result |-> "none", largest |-> 0, honest |-> FALSE, true |-> 0, frl |-> 0, near |-> 0]

VARIABLES
    highestSent,        \* the sender's highest sent session sequence (Start - 1: none yet)
    opened,             \* the sequences the receiver has opened
    flight,             \* the block in flight to the sender, or NoBlock
    read                \* the reader's last decision, with the block's ghost truth

vars == << highestSent, opened, flight, read >>

HighestOpened == IF opened = {} THEN 0
                 ELSE CHOOSE s \in opened : \A t \in opened : t <= s

\* the received run ending at the highest opened: rocketnet_ack_block_build's
\* first range length
RunLength(h) == Cardinality({ s \in opened : s <= h /\ \A t \in s..h : t \in opened })

\* rocketnet_reconstruct_near: the candidate sharing the wire value that is
\* nearest to expected, the larger on a tie
Candidates(w) == { k * Modulus + w : k \in 0..(MaxSeq \div Modulus + 1) }
Distance(a, b) == IF a > b THEN a - b ELSE b - a
Near(w, e) == CHOOSE c \in Candidates(w) :
                  \A d \in Candidates(w) :
                      Distance(c, e) < Distance(d, e) \/ ( Distance(c, e) = Distance(d, e) /\ c >= d )

\* rocketnet_ack_block_read's decision on a block at highest sent hs
Decide(b, hs) ==
    LET L == Near(b.wire, hs) IN
    IF BrokenHalfRange
    THEN IF L = 0
         THEN IF b.frl = 0 /\ b.count = 0 THEN [result |-> "empty", largest |-> 0]
              ELSE [result |-> "refused", largest |-> 0]
         ELSE IF b.frl = 0 \/ (RunBoundChecked /\ b.frl > L) THEN [result |-> "refused", largest |-> 0]
         ELSE [result |-> "nonempty", largest |-> L]
    ELSE IF b.wire = 0 /\ b.frl = 0 /\ b.count = 0 THEN [result |-> "empty", largest |-> 0]
         ELSE IF b.frl = 0 \/ (RunBoundChecked /\ b.frl > L) THEN [result |-> "refused", largest |-> 0]
         ELSE [result |-> "nonempty", largest |-> L]

TypeOK ==
    /\ highestSent \in (Start - 1)..MaxSeq
    /\ opened \subseteq Seqs
    /\ flight.wire \in 0..(Modulus - 1)
    /\ read.result \in { "none", "empty", "nonempty", "refused" }

Init ==
    /\ highestSent = Start - 1
    /\ opened = {}
    /\ flight = NoBlock
    /\ read = NoRead

\* the session window: the sender never runs Half or more past a sequence
\* an ack can still name (the highest opened, or the block in flight)
WindowAllows(hs) ==
    /\ ( opened = {} \/ hs - HighestOpened < Half )
    /\ ( flight.none \/ ~flight.honest \/ flight.true = 0 \/ hs - flight.true < Half )

Send ==
    /\ highestSent < MaxSeq
    /\ WindowAllows(highestSent + 1)
    /\ highestSent' = highestSent + 1
    /\ UNCHANGED << opened, flight, read >>

Open(s) ==
    /\ s >= Start /\ s <= highestSent
    /\ s \notin opened
    /\ highestSent - s < Half
    /\ opened' = opened \cup { s }
    /\ UNCHANGED << highestSent, flight, read >>

\* rocketnet_ack_block_build then rocketnet_ack_block_write: the all-zero
\* block while nothing is opened, else the wire largest and its run
WriteBlock ==
    /\ flight.none
    /\ LET h == HighestOpened IN
       flight' = [wire |-> h % Modulus,
                  frl |-> IF h = 0 THEN 0 ELSE RunLength(h),
                  count |-> 0, honest |-> TRUE, true |-> h, none |-> FALSE]
    /\ UNCHANGED << highestSent, opened, read >>

\* an outside event: a block the builder did not write, any shape but the
\* all-zero empty block, and no ranges behind a first run (the walk, t15
\* rules 4 and 5, is abstracted)
Forge(w, f, c) ==
    /\ flight.none
    /\ ( w # 0 \/ f # 0 \/ c # 0 )
    /\ ( f = 0 \/ c = 0 )
    /\ flight' = [wire |-> w, frl |-> f, count |-> c, honest |-> FALSE, true |-> 0, none |-> FALSE]
    /\ UNCHANGED << highestSent, opened, read >>

ReadBlock ==
    /\ ~flight.none
    /\ LET d == Decide(flight, highestSent) IN
       read' = [result |-> d.result, largest |-> d.largest, honest |-> flight.honest, true |-> flight.true,
                frl |-> flight.frl, near |-> Near(flight.wire, highestSent)]
    /\ flight' = NoBlock
    /\ UNCHANGED << highestSent, opened >>

Next ==
    \/ Send
    \/ \E s \in Seqs : Open(s)
    \/ WriteBlock
    \/ \E w \in 0..(Modulus - 1), f \in 0..2, c \in 0..1 : Forge(w, f, c)
    \/ ReadBlock

Spec == Init /\ [][Next]_vars

\* a block written while nothing is opened reads empty, whatever the
\* sender's highest sent
EmptyRecognised == ( read.honest /\ read.true = 0 ) => read.result = "empty"

\* a nonempty block reconstructs to the true highest opened
LargestExact == ( read.honest /\ read.true # 0 ) => ( read.result = "nonempty" /\ read.largest = read.true )

\* a block the builder wrote is never refused
NeverRefusedHonest == read.honest => read.result # "refused"

\* a malformed forged block is refused: first range length zero with
\* anything else nonzero (t15 rule 2; Forge never writes the all-zero
\* block), or a first run longer than the largest it reconstructs to (t15
\* rule 3: the run cannot reach below sequence 1).  A forged block of
\* neither shape has no ranges here, is well formed and may read nonempty.
MalformedRefused ==
    ( read.result # "none" /\ ~read.honest /\ ( read.frl = 0 \/ read.frl > read.near ) )
        => read.result = "refused"
=============================================================================
