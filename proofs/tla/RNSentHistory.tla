------------------------------ MODULE RNSentHistory ------------------------------
(***************************************************************************)
(* rocketnet: the sender's sent history under loss, reordering and ack    *)
(* delay (docs/TLA-INVENTORY.md M8, the sent-record slice).               *)
(*                                                                         *)
(* The sender writes every session packet it sends into a ring of N       *)
(* records, record i holding the newest sequence congruent to i mod N     *)
(* (sdk/rocketnet_ack.cpp, rocketnet_ack_endpoint_record_sent_classified).*)
(* Writing sequence s replaces the record of s - N. When that record was   *)
(* never acknowledged the write is an eviction, and the eviction must be   *)
(* told apart by what the sender knows:                                    *)
(*                                                                         *)
(*   lost     the receiver has acknowledged a sequence at least Threshold  *)
(*            above the evicted one (RFC 9002 section 6.1.1, the packet    *)
(*            threshold, ROCKETNET_LOSS_PACKET_THRESHOLD): the packet was  *)
(*            lost on the path. Counted SEND_HISTORY_LOST, an outcome.     *)
(*   overflow no such later acknowledgment exists: the fate is unknown (a  *)
(*            silent receiver, or a window too small for the path). Counted *)
(*            SEND_HISTORY_OVERFLOW, a refusal of history.                 *)
(*                                                                         *)
(* A write never refuses the send. STANDARD.md "Replay window sizing"      *)
(* says an implementation MUST NOT throttle application sends to preserve  *)
(* history, so Send is enabled whenever there is a sequence left to send.  *)
(*                                                                         *)
(* The path. Packets in flight may be delivered in any order or lost (at   *)
(* most MaxLoss in all, never Threshold + 1 consecutive sequences). The    *)
(* receiver acknowledges by restating its received set inside its window   *)
(* as                                                                      *)
(* an ack block (the last AckRange sequences below its highest received); *)
(* blocks are in flight too, delivered in any order and after any delay,   *)
(* so an ack may arrive after the record it names was evicted, where it is *)
(* ignored. largestAcked is the highest sequence the sender has marked     *)
(* acknowledged: it is folded only for a record that is newly marked, as   *)
(* rocketnet_ack_apply_run folds it.                                       *)
(*                                                                         *)
(* A healthy path is a history variable, not an enabling condition: it     *)
(* stays TRUE while every send was at most Flight ahead of largestAcked    *)
(* (Flight is the path's bandwidth-delay product in packets plus the ack   *)
(* interval; N - Flight >= Threshold is assumed). NoOverflowUnderBounded   *)
(* Loss says that on a healthy path with bounded loss the overflow count   *)
(* stays zero. The counts are carried as sets of sequences so that         *)
(* "exactly one of lost and overflow" is checkable, and the cardinality is *)
(* the counter.                                                            *)
(*                                                                         *)
(* Two constants each reintroduce a defect; TLC must fail each             *)
(* (README-RNSentHistory.md has the traces, replayed by hand):             *)
(*   CountEveryUnacked = TRUE  the rule before the split: every unacked    *)
(*                             eviction is overflow: NoOverflowUnderBounded*)
(*                             Loss (the w3 finding on a 156 ms path)      *)
(*   RefuseWhenFull    = TRUE  Send disabled while every record is unacked *)
(*                             (throttling to keep history): SendNever     *)
(*                             Refused                                     *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    N,                  \* the ring: records held (ROCKETNET_SENT_PACKET_BUFFER_SIZE)
    Threshold,          \* ROCKETNET_LOSS_PACKET_THRESHOLD
    MaxSeq,             \* sequences 1 .. MaxSeq may be sent (a size knob)
    MaxLoss,            \* packets the path may lose in all (a size knob)
    Flight,             \* the healthy bound: sends at most this far ahead of largestAcked
    AckRange,           \* the ack block covers this many sequences below the receiver's highest
    AckSlots,           \* ack blocks in flight at once (a size knob)
    CountEveryUnacked,  \* the defect: no lost class, every unacked eviction is overflow
    RefuseWhenFull      \* the defect: Send disabled while every record is unacked

ASSUME /\ N \in Nat \ {0} /\ Threshold \in Nat \ {0}
       /\ MaxSeq \in Nat \ {0} /\ MaxLoss \in Nat
       /\ Flight \in Nat \ {0} /\ N - Flight >= Threshold
       /\ AckRange \in Nat \ {0} /\ AckSlots \in Nat \ {0}
       /\ CountEveryUnacked \in BOOLEAN /\ RefuseWhenFull \in BOOLEAN

VARIABLES
    next,            \* the next sequence to send
    ring,            \* ring[i] = [seq |-> newest sequence written (0 = empty), acked |-> BOOLEAN]
    largestAcked,    \* the highest sequence marked acknowledged; 0 = none
    inFlight,        \* packets sent and neither delivered nor lost
    rcvd,            \* the receiver's received set
    acks,            \* ack blocks in flight: each a set of sequences the receiver restated
    pathLost,        \* the sequences the path lost
    ackedSet,        \* ghost: every sequence ever marked acknowledged
    evictedUnacked,  \* ghost: every sequence evicted while unacknowledged
    lostSet,         \* the sequences counted SEND_HISTORY_LOST
    overflowSet,     \* the sequences counted SEND_HISTORY_OVERFLOW
    healthy          \* ghost: every send so far was at most Flight ahead of largestAcked

vars == <<next, ring, largestAcked, inFlight, rcvd, acks, pathLost, ackedSet,
          evictedUnacked, lostSet, overflowSet, healthy>>

Seqs == 1 .. MaxSeq
Slots == 0 .. N - 1

Max(S) == CHOOSE x \in S : \A y \in S : y <= x

AllUnacked == \A i \in Slots : ring[i].seq # 0 /\ ~ring[i].acked

\* the send is enabled whenever a sequence is left; RefuseWhenFull is the
\* defect that throttles instead
SendEnabled == /\ next <= MaxSeq
               /\ ~(RefuseWhenFull /\ AllUnacked)

\* a run of k consecutive sequences all in S
HasRun(S, k) == \E a \in S : \A j \in 0 .. k - 1 : a + j \in S

TypeOK ==
    /\ next \in 1 .. MaxSeq + 1
    /\ ring \in [Slots -> [seq : 0 .. MaxSeq, acked : BOOLEAN]]
    /\ largestAcked \in 0 .. MaxSeq
    /\ inFlight \subseteq Seqs /\ rcvd \subseteq Seqs /\ pathLost \subseteq Seqs
    /\ acks \subseteq SUBSET Seqs
    /\ ackedSet \subseteq Seqs /\ evictedUnacked \subseteq Seqs
    /\ lostSet \subseteq Seqs /\ overflowSet \subseteq Seqs
    /\ healthy \in BOOLEAN

Init ==
    /\ next = 1
    /\ ring = [i \in Slots |-> [seq |-> 0, acked |-> FALSE]]
    /\ largestAcked = 0
    /\ inFlight = {} /\ rcvd = {} /\ acks = {} /\ pathLost = {}
    /\ ackedSet = {} /\ evictedUnacked = {}
    /\ lostSet = {} /\ overflowSet = {}
    /\ healthy = TRUE

(***************************************************************************)
(* Send: write sequence next into its record, classify what it evicts.    *)
(* The packet is always sent (it joins inFlight).                          *)
(***************************************************************************)
Send ==
    /\ SendEnabled
    /\ LET s == next
           old == ring[s % N]
           unacked == old.seq # 0 /\ ~old.acked
           known == unacked /\ ~CountEveryUnacked /\ largestAcked >= old.seq + Threshold
       IN /\ ring' = [ring EXCEPT ![s % N] = [seq |-> s, acked |-> FALSE]]
          /\ evictedUnacked' = IF unacked THEN evictedUnacked \cup {old.seq} ELSE evictedUnacked
          /\ lostSet' = IF known THEN lostSet \cup {old.seq} ELSE lostSet
          /\ overflowSet' = IF unacked /\ ~known THEN overflowSet \cup {old.seq} ELSE overflowSet
          /\ healthy' = (healthy /\ s - largestAcked <= Flight)
          /\ inFlight' = inFlight \cup {s}
          /\ next' = s + 1
    /\ UNCHANGED <<largestAcked, rcvd, acks, pathLost, ackedSet>>

\* the path carries a packet to the receiver
Deliver ==
    /\ \E s \in inFlight :
          /\ inFlight' = inFlight \ {s}
          /\ rcvd' = rcvd \cup {s}
    /\ UNCHANGED <<next, ring, largestAcked, acks, pathLost, ackedSet,
                   evictedUnacked, lostSet, overflowSet, healthy>>

\* the path loses a packet: at most MaxLoss in all, and never Threshold + 1
\* consecutive sequences
Lose ==
    /\ \E s \in inFlight :
          /\ Cardinality(pathLost) < MaxLoss
          /\ ~HasRun(pathLost \cup {s}, Threshold + 1)
          /\ inFlight' = inFlight \ {s}
          /\ pathLost' = pathLost \cup {s}
    /\ UNCHANGED <<next, ring, largestAcked, rcvd, acks, ackedSet,
                   evictedUnacked, lostSet, overflowSet, healthy>>

\* the receiver sends an ack block restating its received set inside its window
ReceiverAck ==
    /\ rcvd # {}
    /\ Cardinality(acks) < AckSlots
    /\ LET top == Max(rcvd)
           block == {x \in rcvd : x + AckRange > top}
       IN acks' = acks \cup {block}
    /\ UNCHANGED <<next, ring, largestAcked, inFlight, rcvd, pathLost, ackedSet,
                   evictedUnacked, lostSet, overflowSet, healthy>>

\* the sender reads one ack block (any order, any delay): every named record
\* still held and not yet acknowledged is marked, largestAcked folds to the
\* maximum of the newly marked; a sequence whose record was evicted is ignored
ReceiveAck ==
    /\ \E b \in acks :
          LET fresh == {x \in b : ring[x % N].seq = x /\ ~ring[x % N].acked}
          IN /\ acks' = acks \ {b}
             /\ ring' = [i \in Slots |->
                           IF ring[i].seq \in fresh THEN [ring[i] EXCEPT !.acked = TRUE] ELSE ring[i]]
             /\ ackedSet' = ackedSet \cup fresh
             /\ largestAcked' = IF fresh = {} THEN largestAcked
                                ELSE IF Max(fresh) > largestAcked THEN Max(fresh) ELSE largestAcked
    /\ UNCHANGED <<next, inFlight, rcvd, pathLost, evictedUnacked, lostSet, overflowSet, healthy>>

\* sending is over and the run may stop here (no deadlock to report)
Done ==
    /\ next > MaxSeq
    /\ UNCHANGED vars

Next == Send \/ Deliver \/ Lose \/ ReceiverAck \/ ReceiveAck \/ Done

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Properties.                                                             *)
(***************************************************************************)

\* an unacked eviction counts exactly one of lost and overflow, and
\* nothing else is counted: the two counts sum to the unacked evictions
EveryEvictionClassified ==
    /\ lostSet \cap overflowSet = {}
    /\ lostSet \cup overflowSet = evictedUnacked

\* a sequence counted lost has a later acknowledgment at least Threshold above it,
\* acknowledged for real (the receiver received it) and remembered in largestAcked
LostImpliesLaterAcked ==
    \A s \in lostSet : \E t \in ackedSet : t >= s + Threshold /\ t \in rcvd /\ largestAcked >= t

\* largestAcked is the maximum sequence ever marked acknowledged
LargestAckedIsMaximum ==
    largestAcked = IF ackedSet = {} THEN 0 ELSE Max(ackedSet)

\* with acks flowing (healthy) and the loss bounded and isolated by Lose,
\* the history refuses nothing: overflow stays zero
NoOverflowUnderBoundedLoss ==
    healthy => Cardinality(overflowSet) = 0

\* the send is never refused: whenever a sequence is left, Send is enabled
SendNeverRefused ==
    next <= MaxSeq => ENABLED Send

=============================================================================
