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
(* zero here, and their walk (rules 3-5) is the subject of                 *)
(* FragmentsAndAcks.  Forge writes the malformed shapes with a zero first *)
(* range length, so the reader's refusal of them is checked too.          *)
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
    BrokenHalfRange     \* TRUE: the empty test on the reconstructed value

ASSUME Modulus >= 4 /\ Modulus % 2 = 0
ASSUME Start >= 1 /\ MaxSeq >= Start
ASSUME BrokenHalfRange \in BOOLEAN

Half == Modulus \div 2
Seqs == Start..MaxSeq
NoBlock == [wire |-> 0, frl |-> 0, count |-> 0, honest |-> FALSE, true |-> 0, none |-> TRUE]
NoRead == [result |-> "none", largest |-> 0, honest |-> FALSE, true |-> 0]

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
         ELSE IF b.frl = 0 \/ b.frl > L THEN [result |-> "refused", largest |-> 0]
         ELSE [result |-> "nonempty", largest |-> L]
    ELSE IF b.wire = 0 /\ b.frl = 0 /\ b.count = 0 THEN [result |-> "empty", largest |-> 0]
         ELSE IF b.frl = 0 \/ b.frl > L THEN [result |-> "refused", largest |-> 0]
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

\* an outside event: a malformed block with a zero first range length and
\* something else nonzero
Forge(w, c) ==
    /\ flight.none
    /\ ( w # 0 \/ c # 0 )
    /\ flight' = [wire |-> w, frl |-> 0, count |-> c, honest |-> FALSE, true |-> 0, none |-> FALSE]
    /\ UNCHANGED << highestSent, opened, read >>

ReadBlock ==
    /\ ~flight.none
    /\ LET d == Decide(flight, highestSent) IN
       read' = [result |-> d.result, largest |-> d.largest, honest |-> flight.honest, true |-> flight.true]
    /\ flight' = NoBlock
    /\ UNCHANGED << highestSent, opened >>

Next ==
    \/ Send
    \/ \E s \in Seqs : Open(s)
    \/ WriteBlock
    \/ \E w \in 0..(Modulus - 1), c \in 0..1 : Forge(w, c)
    \/ ReadBlock

Spec == Init /\ [][Next]_vars

\* a block written while nothing is opened reads empty, whatever the
\* sender's highest sent
EmptyRecognised == ( read.honest /\ read.true = 0 ) => read.result = "empty"

\* a nonempty block reconstructs to the true highest opened
LargestExact == ( read.honest /\ read.true # 0 ) => ( read.result = "nonempty" /\ read.largest = read.true )

\* a block the builder wrote is never refused
NeverRefusedHonest == read.honest => read.result # "refused"

\* a block with first range length zero and anything else nonzero is refused
MalformedRefused == ( read.result # "none" /\ ~read.honest ) => read.result = "refused"
=============================================================================
