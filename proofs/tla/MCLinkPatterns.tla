-------------------------- MODULE MCLinkPatterns --------------------------
(***************************************************************************)
(* Deterministic loss ("everynth") and window-reversal ("ReverseWindow")   *)
(* checks on top of Link, with reversed witnesses that fail when the       *)
(* drop period or window reversal is broken.                               *)
(***************************************************************************)
EXTENDS Link

CONSTANTS
    MaxSeq,
    Defect

VARIABLES
    nextSend,
    delivered,
    winCount,
    winReversed

patVars == <<wire, sent, pulse, nextSend, delivered, winCount, winReversed>>

PatInit ==
    /\ Init
    /\ nextSend = 1
    /\ delivered = <<>>
    /\ winCount = 0
    /\ winReversed = FALSE

(***************************************************************************)
(* Periodic every-Nth drop: sends 1..MaxSeq in order; under Defect =       *)
(* "none" uses Link!Send(p) with Pattern = "everynth" (dropping BurstLen,  *)
(* 2*BurstLen, ...). Under Defect = "wrong_period" drops NextCount =       *)
(* BurstLen - 1 instead, which violates EveryNthDropped. Under Defect =    *)
(* "overrun" the send counter jumps past MaxSeq, which violates             *)
(* MCPatternsTypeOK.                                                       *)
(***************************************************************************)
SendEveryNth ==
    /\ nextSend <= MaxSeq
    /\ IF Defect = "wrong_period"
       THEN /\ nextSend \in Packets
            /\ InFlight < Capacity
            /\ sent' = NextCount
            /\ IF NextCount = BurstLen - 1
               THEN UNCHANGED wire
               ELSE wire' = Append(wire, [pkt |-> nextSend, due |-> 0])
            /\ UNCHANGED pulse
       ELSE Send(nextSend)
    /\ nextSend' = IF Defect = "overrun" THEN nextSend + 4 ELSE nextSend + 1
    /\ UNCHANGED <<delivered, winCount, winReversed>>

DeliverEveryNth ==
    \E p \in Packets :
        /\ Deliver(p)
        /\ delivered' = Append(delivered, p)
        /\ UNCHANGED <<nextSend, winCount, winReversed>>

DoneStutter ==
    /\ nextSend > MaxSeq
    /\ wire = <<>>
    /\ UNCHANGED patVars

NextEveryNth == SendEveryNth \/ DeliverEveryNth \/ DoneStutter

SpecEveryNth == PatInit /\ [][NextEveryNth]_patVars

(***************************************************************************)
(* Window reversal: buffers each window of BurstLen sends on the wire,     *)
(* reverses the window in place via Link!ReverseWindow(BurstLen) (unless   *)
(* Defect = "unreversed"), and drains the reversed window.                 *)
(***************************************************************************)
SendIntoWindow ==
    /\ nextSend <= MaxSeq
    /\ ~winReversed
    /\ winCount < BurstLen
    /\ Send(nextSend)
    /\ nextSend' = nextSend + 1
    /\ winCount' = winCount + 1
    /\ UNCHANGED <<delivered, winReversed>>

FlushWindow ==
    /\ ~winReversed
    /\ winCount = BurstLen
    /\ IF Defect = "unreversed"
       THEN UNCHANGED <<wire, sent, pulse>>
       ELSE ReverseWindow(BurstLen)
    /\ winReversed' = TRUE
    /\ UNCHANGED <<nextSend, delivered, winCount>>

DeliverFromWindow ==
    /\ winReversed
    /\ \E p \in Packets :
        /\ Deliver(p)
        /\ delivered' = Append(delivered, p)
        /\ winCount' = winCount - 1
        /\ winReversed' = (winCount > 1)
        /\ UNCHANGED nextSend

NextReverseWindow == SendIntoWindow \/ FlushWindow \/ DeliverFromWindow \/ DoneStutter

SpecReverseWindow == PatInit /\ [][NextReverseWindow]_patVars

(***************************************************************************)
(* Invariants.                                                             *)
(***************************************************************************)
MCPatternsTypeOK ==
    /\ TypeOK
    /\ nextSend \in 1..(MaxSeq + 1)
    /\ delivered \in Seq(Packets)
    /\ winCount \in 0..BurstLen
    /\ winReversed \in BOOLEAN

EveryNthDropped ==
    /\ \A i \in DOMAIN delivered : delivered[i] % BurstLen # 0
    /\ (nextSend > MaxSeq /\ wire = <<>>) =>
           Len(delivered) = MaxSeq - (MaxSeq \div BurstLen)

SurvivorsInOrder ==
    \A i \in 1..(Len(delivered) - 1) : delivered[i] < delivered[i + 1]

WindowOrderReversed ==
    \A i \in DOMAIN delivered :
        LET w == (i - 1) \div BurstLen
            k == (i - 1) % BurstLen
        IN delivered[i] = (w + 1) * BurstLen - k

=============================================================================
