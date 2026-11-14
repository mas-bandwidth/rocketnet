------------------------------ MODULE RNSentHistory ------------------------------
(***************************************************************************)
(* rocketnet: the sender's sent history under loss, reordering and ack    *)
(* delay (docs/TLA-INVENTORY.md M8, the sent-record slice).               *)
(*                                                                         *)
(* The sender writes every session packet it sends into a ring of N       *)
(* records, record i holding the newest sequence congruent to i mod N     *)
(* (sdk/rocketnet_ack.cpp, rocketnet_ack_endpoint_record_sent_classified).*)
(* A write of sequence s has one of four outcomes                          *)
(* (enum rocketnet_ack_sent_result_t):                                     *)
(*                                                                         *)
(*   recorded the record it replaces was empty or acknowledged.            *)
(*   lost     it replaces an unacknowledged record, and the receiver has   *)
(*            acknowledged a sequence at least Threshold above it (RFC     *)
(*            9002 section 6.1.1, the packet threshold,                    *)
(*            ROCKETNET_LOSS_PACKET_THRESHOLD): the packet was lost on the *)
(*            path. Counted SEND_HISTORY_LOST, an outcome.                 *)
(*   overflow it replaces an unacknowledged record and no such later       *)
(*            acknowledgment exists: the fate is unknown (a silent         *)
(*            receiver, or a window too small for the path). Counted       *)
(*            SEND_HISTORY_OVERFLOW, a refusal of history.                 *)
(*   stale    the write is refused and the ring is left alone: the slot    *)
(*            already holds s or a newer sequence, or s is a ring or more  *)
(*            behind the highest recorded sequence. Counted                *)
(*            SEND_HISTORY_OVERFLOW too (rocketnet_ack_count_sent_result). *)
(*                                                                         *)
(* The writers. The fill-phase threads take sequences in order and record  *)
(* them concurrently (each record under sent_lock, the high-water mark an *)
(* atomic max), so records land in any order among the sequences taken and *)
(* not yet recorded. Writers bounds how many are outstanding at once:      *)
(* Send(s) may record s while fewer than Writers sequences below it are    *)
(* still unsent. A writer that stalls while the others record a ring's     *)
(* worth of later sequences finds its slot newer and is refused (stale).   *)
(* Writers = 1 is the single in-order writer. The update thread applies    *)
(* acks only between fill phases, when every taken sequence is recorded:   *)
(* ReceiveAck needs the sent sequences to be a prefix 1..k.                *)
(*                                                                         *)
(* A write never refuses the send. STANDARD.md "Replay window sizing"      *)
(* says an implementation MUST NOT throttle application sends to preserve  *)
(* history, so a send is enabled whenever there is a sequence left, and a  *)
(* stale refusal still transmits the packet.                               *)
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
(* (evictions of unknown fate and stale refusals) stays zero. The counts   *)
(* are carried as sets of sequences so that "exactly one of lost and       *)
(* overflow" is checkable, and the cardinality is the counter.             *)
(*                                                                         *)
(* Three constants each reintroduce a defect; TLC must fail each           *)
(* (README-RNSentHistory.md has the traces, replayed by hand):             *)
(*   CountEveryUnacked = TRUE  the rule before the split: every unacked    *)
(*                             eviction is overflow: NoOverflowUnderBounded*)
(*                             Loss (the w3 finding on a 156 ms path)      *)
(*   RefuseWhenFull    = TRUE  Send disabled while every record is unacked *)
(*                             (throttling to keep history): SendNever     *)
(*                             Refused                                     *)
(*   WriteStale        = TRUE  no stale refusal: a stalled writer          *)
(*                             overwrites the newer record in its slot:    *)
(*                             RingHoldsNewest                             *)
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
    Writers,            \* sequences taken and not yet recorded at once (fill-phase writers)
    CountEveryUnacked,  \* the defect: no lost class, every unacked eviction is overflow
    RefuseWhenFull,     \* the defect: Send disabled while every record is unacked
    WriteStale          \* the defect: no stale refusal, the write replaces a newer record

ASSUME /\ N \in Nat \ {0} /\ Threshold \in Nat \ {0}
       /\ MaxSeq \in Nat \ {0} /\ MaxLoss \in Nat
       /\ Flight \in Nat \ {0} /\ N - Flight >= Threshold
       /\ AckRange \in Nat \ {0} /\ AckSlots \in Nat \ {0}
       /\ Writers \in Nat \ {0}
       /\ CountEveryUnacked \in BOOLEAN /\ RefuseWhenFull \in BOOLEAN /\ WriteStale \in BOOLEAN

VARIABLES
    sent,            \* the sequences handed to Send so far (recorded or refused)
    ring,            \* ring[i] = [seq |-> newest sequence written (0 = empty), acked |-> BOOLEAN]
    highest,         \* endpoint->highest_sequence: the highest sequence recorded; 0 = none
    largestAcked,    \* the highest sequence marked acknowledged; 0 = none
    inFlight,        \* packets sent and neither delivered nor lost
    rcvd,            \* the receiver's received set
    acks,            \* ack blocks in flight: each a set of sequences the receiver restated
    pathLost,        \* the sequences the path lost
    ackedSet,        \* ghost: every sequence ever marked acknowledged
    evictedUnacked,  \* ghost: every sequence evicted while unacknowledged
    lostSet,         \* the sequences counted SEND_HISTORY_LOST
    overflowSet,     \* the evictions counted SEND_HISTORY_OVERFLOW
    staleSet,        \* the refused writes, counted SEND_HISTORY_OVERFLOW too
    healthy          \* ghost: every send so far was at most Flight ahead of largestAcked

vars == <<sent, ring, highest, largestAcked, inFlight, rcvd, acks, pathLost, ackedSet,
          evictedUnacked, lostSet, overflowSet, staleSet, healthy>>

Seqs == 1 .. MaxSeq
Slots == 0 .. N - 1

Max(S) == CHOOSE x \in S : \A y \in S : y <= x

AllUnacked == \A i \in Slots : ring[i].seq # 0 /\ ~ring[i].acked

\* s may be recorded now: unsent, and fewer than Writers sequences below it
\* are still unsent (each held by a writer that took it before s)
Outstanding(s) == {x \in 1 .. s - 1 : x \notin sent}
Recordable(s) == s \notin sent /\ Cardinality(Outstanding(s)) < Writers

\* the send is enabled whenever a sequence is left; RefuseWhenFull is the
\* defect that throttles instead
SendEnabled(s) == /\ Recordable(s)
                  /\ ~(RefuseWhenFull /\ AllUnacked)

\* a run of k consecutive sequences all in S
HasRun(S, k) == \E a \in S : \A j \in 0 .. k - 1 : a + j \in S

\* every taken sequence is recorded: no fill phase is open
Between == sent = 1 .. Cardinality(sent)

TypeOK ==
    /\ sent \subseteq Seqs
    /\ ring \in [Slots -> [seq : 0 .. MaxSeq, acked : BOOLEAN]]
    /\ highest \in 0 .. MaxSeq
    /\ largestAcked \in 0 .. MaxSeq
    /\ inFlight \subseteq Seqs /\ rcvd \subseteq Seqs /\ pathLost \subseteq Seqs
    /\ acks \subseteq SUBSET Seqs
    /\ ackedSet \subseteq Seqs /\ evictedUnacked \subseteq Seqs
    /\ lostSet \subseteq Seqs /\ overflowSet \subseteq Seqs /\ staleSet \subseteq Seqs
    /\ healthy \in BOOLEAN

Init ==
    /\ sent = {}
    /\ ring = [i \in Slots |-> [seq |-> 0, acked |-> FALSE]]
    /\ highest = 0
    /\ largestAcked = 0
    /\ inFlight = {} /\ rcvd = {} /\ acks = {} /\ pathLost = {}
    /\ ackedSet = {} /\ evictedUnacked = {}
    /\ lostSet = {} /\ overflowSet = {} /\ staleSet = {}
    /\ healthy = TRUE

(***************************************************************************)
(* Send(s): a writer records sequence s (rocketnet_ack_endpoint_record_    *)
(* sent_classified). A stale write is refused and leaves the ring alone;   *)
(* any other write replaces the record in its slot and classifies what it  *)
(* evicts. The packet is always transmitted (it joins inFlight).           *)
(***************************************************************************)
Send(s) ==
    /\ SendEnabled(s)
    /\ LET old == ring[s % N]
           stale == ~WriteStale /\ (old.seq >= s \/ (highest >= N /\ s <= highest - N))
           unacked == old.seq # 0 /\ ~old.acked
           known == unacked /\ ~CountEveryUnacked /\ largestAcked >= old.seq + Threshold
       IN IF stale
          THEN /\ staleSet' = staleSet \cup {s}
               /\ UNCHANGED <<ring, highest, evictedUnacked, lostSet, overflowSet>>
          ELSE /\ ring' = [ring EXCEPT ![s % N] = [seq |-> s, acked |-> FALSE]]
               /\ highest' = IF s > highest THEN s ELSE highest
               /\ evictedUnacked' = IF unacked THEN evictedUnacked \cup {old.seq} ELSE evictedUnacked
               /\ lostSet' = IF known THEN lostSet \cup {old.seq} ELSE lostSet
               /\ overflowSet' = IF unacked /\ ~known THEN overflowSet \cup {old.seq} ELSE overflowSet
               /\ UNCHANGED staleSet
    /\ healthy' = (healthy /\ s - largestAcked <= Flight)
    /\ inFlight' = inFlight \cup {s}
    /\ sent' = sent \cup {s}
    /\ UNCHANGED <<largestAcked, rcvd, acks, pathLost, ackedSet>>

\* the path carries a packet to the receiver
Deliver ==
    /\ \E s \in inFlight :
          /\ inFlight' = inFlight \ {s}
          /\ rcvd' = rcvd \cup {s}
    /\ UNCHANGED <<sent, ring, highest, largestAcked, acks, pathLost, ackedSet,
                   evictedUnacked, lostSet, overflowSet, staleSet, healthy>>

\* the path loses a packet: at most MaxLoss in all, and never Threshold + 1
\* consecutive sequences
Lose ==
    /\ \E s \in inFlight :
          /\ Cardinality(pathLost) < MaxLoss
          /\ ~HasRun(pathLost \cup {s}, Threshold + 1)
          /\ inFlight' = inFlight \ {s}
          /\ pathLost' = pathLost \cup {s}
    /\ UNCHANGED <<sent, ring, highest, largestAcked, rcvd, acks, ackedSet,
                   evictedUnacked, lostSet, overflowSet, staleSet, healthy>>

\* the receiver sends an ack block restating its received set inside its window
ReceiverAck ==
    /\ rcvd # {}
    /\ Cardinality(acks) < AckSlots
    /\ LET top == Max(rcvd)
           block == {x \in rcvd : x + AckRange > top}
       IN acks' = acks \cup {block}
    /\ UNCHANGED <<sent, ring, highest, largestAcked, inFlight, rcvd, pathLost, ackedSet,
                   evictedUnacked, lostSet, overflowSet, staleSet, healthy>>

\* the sender reads one ack block (any order, any delay) between fill phases:
\* every named record still held and not yet acknowledged is marked,
\* largestAcked folds to the maximum of the newly marked; a sequence whose
\* record was evicted is ignored
ReceiveAck ==
    /\ Between
    /\ \E b \in acks :
          LET fresh == {x \in b : ring[x % N].seq = x /\ ~ring[x % N].acked}
          IN /\ acks' = acks \ {b}
             /\ ring' = [i \in Slots |->
                           IF ring[i].seq \in fresh THEN [ring[i] EXCEPT !.acked = TRUE] ELSE ring[i]]
             /\ ackedSet' = ackedSet \cup fresh
             /\ largestAcked' = IF fresh = {} THEN largestAcked
                                ELSE IF Max(fresh) > largestAcked THEN Max(fresh) ELSE largestAcked
    /\ UNCHANGED <<sent, highest, inFlight, rcvd, pathLost, evictedUnacked, lostSet, overflowSet,
                   staleSet, healthy>>

\* sending is over and the run may stop here (no deadlock to report)
Done ==
    /\ sent = Seqs
    /\ UNCHANGED vars

Next == (\E s \in Seqs : Send(s)) \/ Deliver \/ Lose \/ ReceiverAck \/ ReceiveAck \/ Done

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
\* the history refuses nothing: overflow (unknown evictions and stale
\* refusals) stays zero
NoOverflowUnderBoundedLoss ==
    healthy => Cardinality(overflowSet) + Cardinality(staleSet) = 0

\* the send is never refused: whenever a sequence is left, a send is enabled
SendNeverRefused ==
    sent # Seqs => \E s \in Seqs : ENABLED Send(s)

\* every slot holds the newest sequence written to it: a refused stale write
\* never replaces a newer record
RingHoldsNewest ==
    \A i \in Slots :
        LET written == {x \in sent \ staleSet : x % N = i}
        IN ring[i].seq = IF written = {} THEN 0 ELSE Max(written)

\* every send is transmitted, recorded or refused
EverySendTransmitted ==
    sent \subseteq inFlight \cup rcvd \cup pathLost

\* a reach predicate, not a rule: MCRNSentHistoryReachStale.cfg fails it to
\* show the concurrent instance reaches a stale refusal
NoStaleRefusal ==
    staleSet = {}

=============================================================================
