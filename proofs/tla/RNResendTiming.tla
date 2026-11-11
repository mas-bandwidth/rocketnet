--------------------------- MODULE RNResendTiming ---------------------------
(***************************************************************************)
(* rocketnet: the resend timing of reliable records (docs/CHANNELS.md 6.4  *)
(* "RTO", "Resend" and "Giving up"; the code is                            *)
(* rn_window_ack_resend_delay, rn_window_ack_chunk_resend_delay and       *)
(* rn_window_ack_check_timeout in sdk/rocketnet_window_ack.h and .cpp,     *)
(* rn_ordered_write and its walk rn_ordered_walk in                        *)
(* sdk/rocketnet_channel.cpp, and rn_endpoint_ordered_due in               *)
(* sdk/rocketnet_endpoint.cpp).                                            *)
(*                                                                         *)
(* RNWindow keeps the RTO abstract; this module is the timing alone.  Time *)
(* is in ticks of one unit each (each instance names its tick); every     *)
(* tick is an update.  There are two numbered, unacked records of the two  *)
(* delay classes: "s", a single-packet record, and "c", a chunk of a block *)
(* (a record whose send descriptor has more than one fragment).  The small *)
(* record is first sent at tick 0; the chunk at ChunkFirst (0 in the first *)
(* cut; late in its block's first timeout when the send budget paces a     *)
(* block out over longer than the timeout).  A record is due when          *)
(*     t >= lastSend[r] + Delay(r) where                                   *)
(*     Delay("s") = rto * backoff["s"]                                     *)
(*     Delay("c") = Min(rto, ChunkCap) * backoff["c"]                      *)
(* rto is the adaptive resend delay (rn_window_ack_resend_delay); a small  *)
(* record waits it alone.  ChunkCap = Timeout \div MinAttempts keeps       *)
(* MinAttempts chunk sends inside the timeout whatever the resend delay's  *)
(* maximum is; at the defaults (a 2.5 s cap against a 2 s maximum) a chunk *)
(* waits rto alone.  With Chunk = FALSE the chunk starts acked and the     *)
(* module is the one-record timing of the first cut.                       *)
(*                                                                         *)
(* An update is two steps.  In phase "walk" the acks that arrived are      *)
(* processed (Sample, Ack) and then Walk writes the resend FIFOs; in phase *)
(* "tick" time moves on one tick.  The code keeps one FIFO per delay class *)
(* and walks each from its head while due (OneFifo = FALSE): every due     *)
(* record is written in the update it falls due.  OneFifo = TRUE is one    *)
(* FIFO ordered by last send with both delays, walked from its head and    *)
(* stopped at the first record not due: once rto is above ChunkCap the     *)
(* chunk's delay is the shorter, and a small record at the head that is    *)
(* not due holds back the due chunk behind it (the reversed witnesses      *)
(* MCRNResendTimingBrokenOneFifo.cfg and                                   *)
(* MCRNResendTimingBrokenOneFifoGap.cfg).                                  *)
(*                                                                         *)
(* An RTT sample (the ack of any other packet) moves rto to any value in   *)
(* RtoMin..RtoMax, the abstraction of                                      *)
(* clamp(2 * srtt + 4 * rttvar + 30 ms, resend_delay_min,                  *)
(* resend_delay_max) (rn_window_ack_compute_resend_delay), and resets the  *)
(* back-off to 1.  Before any sample rto is RtoInitial.  A record's own    *)
(* ack may come at any update, or never: every send may be lost.  At tick  *)
(* Timeout after its own first send (MESSAGES m9.3) an unacked record      *)
(* ends the connection (RELIABLE_TIMEOUT) and nothing more is sent.  The   *)
(* code keeps each number's first send in send_first_sends                 *)
(* (rn_window_ack_record_first_send).  BlockClock = TRUE times the chunk   *)
(* from its block's first send at tick 0, the code before the fix: the     *)
(* reversed witness MCRNResendTimingBrokenBlockClock.cfg.                  *)
(*                                                                         *)
(* MaxBackoff is the back-off parameter: 1 is the rule (no back-off, the   *)
(* delay is the same for every resend); 8 is the first version's           *)
(* doubling per resend, 1, 2, 4, 8.                                        *)
(*                                                                         *)
(* The seed (MESSAGES m9.2): until the estimator has a sample of its own,  *)
(* the transport's measured round trip (RN-1 t15.5) is taken as its first  *)
(* sample in the first update the transport reports one, after that        *)
(* update's acks and before its resend walk.  SeedAt is the tick the       *)
(* transport first reports a round trip (a keep-alive's ack, 200 ms into   *)
(* the connection in the seeded instances) and SeedRto the resend delay    *)
(* it gives (2 * rtt + 4 * rtt + 30 ms, clamped).  SeedRule = FALSE is the *)
(* code before the seed: the transport's measurement ignored, the record   *)
(* waiting RtoInitial (2 s) until its own ack is sampled (the reversed     *)
(* witness MCRNResendTimingBrokenUnseeded.cfg).                            *)
(*                                                                         *)
(* AttemptsBeforeTimeout: a record still unacked at the timeout was sent   *)
(* at least MinAttempts times (ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS).     *)
(* attempts saturates at MinAttempts to keep the state space small.        *)
(*                                                                         *)
(* A refused first send (rn_window_ack_packet_failed after finish_packet   *)
(* refuses a granted packet): WalkRefused is an update's walk in which the *)
(* packet carrying a record never on the wire is refused.  The record      *)
(* keeps lastSend = NeverSent (RN_SEND_NUMBER_NEVER_SENT), so no first     *)
(* send is recorded and no timeout clock starts, and it goes back to the   *)
(* head of its FIFO, due at the next walk.  A record may be refused for    *)
(* RefuseFor ticks after it is first offered (0 by default: no refusals).  *)
(* StampRefused = TRUE is the code before the fix: the refusal records the *)
(* first send and stamps lastSend with the due-now sentinel DueNow, so the *)
(* clock runs from the refusal (the reversed witness                       *)
(* MCRNResendTimingBrokenRefusedClock.cfg).  A refused resend of a record  *)
(* already on the wire is not modelled.  In the code it goes back to the   *)
(* head of its FIFO with last_send at a second due-now sentinel,           *)
(* -(resend_delay_max + 2) (rn_packet_builder_packet_failed), and its      *)
(* send_first_sends entry, so its timeout clock, unchanged; it is due at   *)
(* the next walk.  Nothing is sent, so AttemptsBeforeTimeout and           *)
(* ResendGapBounded are claims for a transport that refuses no resend:    *)
(* resends refused for long enough leave fewer than MinAttempts sends     *)
(* before the timeout (by reading; not checked here).  NoDueRecordHeld's  *)
(* exception for a refused packet is the code's behaviour; see its        *)
(* comment.                                                                *)
(*                                                                         *)
(* The paced walk (Room > 0).  Walk writes every due record at once, the   *)
(* walk with room for all of them.  The product asks rn_ordered_write for  *)
(* one packet per call (rn_endpoint_write_packet), as many calls as the    *)
(* update and the send budget allow, and a chunk fills a packet alone.     *)
(* With Room > 0 an update is Room calls (WriteOne), then EndWrites.  Each *)
(* call plans as rn_ordered_walk does, in its order, until the plan holds  *)
(* a packet (a chunk fills it, the one small record does not), and the     *)
(* builder writes one item of the plan: a retransmit (a record on the      *)
(* wire) before ordered progress (a record never sent), either of two of   *)
(* one class.  Pending chunks of the chunk's block are numbered with it    *)
(* and never sent, behind it in number order; a pending chunk written is   *)
(* delivered and acked at once and leaves the model (the lost one is "c"). *)
(* Under the rule each class's numbers on the wire are walked first, the   *)
(* small class then the chunk class, then the never-sent numbers of each.  *)
(* UnsentFirst = TRUE is the walk before the fix: each class from its      *)
(* head, its never-sent numbers first, so a block's pending chunks fill    *)
(* every call and the due chunk behind them waits for the block's whole    *)
(* first pass (the reversed witnesses MCRNResendTimingBrokenUnsentFirst    *)
(* .cfg and MCRNResendTimingBrokenUnsentFirstHeld.cfg).  Acks and samples  *)
(* come before the update's first call, as the code reads them.            *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS
    RtoInitial,     \* message_resend_delay_initial, in ticks
    RtoMin,         \* message_resend_delay_min, in ticks (at least one)
    RtoMax,         \* message_resend_delay_max, in ticks
    Timeout,        \* reliable_timeout, in ticks
    MaxBackoff,     \* 1: no back-off (the rule); 8: doubling to 8
    MinAttempts     \* the sends that must fit before the timeout

ASSUME /\ RtoMin \in Nat \ {0} /\ RtoMax \in Nat /\ RtoMin <= RtoMax
       /\ RtoInitial \in RtoMin..RtoMax
       /\ Timeout \in Nat \ {0}
       /\ MaxBackoff \in {1, 2, 4, 8}
       /\ MinAttempts \in Nat \ {0}

\* The two switches are definitions with the one-record defaults, so a
\* configuration that names neither checks the first cut's single record;
\* a configuration turns one on by definition override
\* (Chunk <- ChunkInFlight, OneFifo <- OneFifoTwoDelays).
\*   Chunk:   a chunk of a block is in flight beside the small record.
\*   OneFifo: FALSE is one FIFO per delay class (the rule); TRUE is one
\*            FIFO with two delays (the reversed witness).
Chunk == FALSE
OneFifo == FALSE
ChunkInFlight == TRUE
OneFifoTwoDelays == TRUE

\*   ChunkFirst: the tick of the chunk's own first send.
\*   BlockClock: FALSE times every record from its own first send (the
\*            rule); TRUE times the chunk from its block's first send at
\*            tick 0 (the reversed witness).
\* (ChunkFirst <- ChunkFirstLate, BlockClock <- BlockClockOn)
ChunkFirst == 0
BlockClock == FALSE
ChunkFirstLate == Timeout - 2
BlockClockOn == TRUE

\*   RefuseFor: the ticks after a record is first offered in which its
\*            first send may be refused; 0 (never) by default.
\*   StampRefused: FALSE leaves a refused first send never sent (the rule);
\*            TRUE stamps it and starts its clock (the reversed witness).
\* (RefuseFor <- RefuseForLong, StampRefused <- StampRefusedOn)
RefuseFor == 0
StampRefused == FALSE
RefuseForLong == Timeout - 2
StampRefusedOn == TRUE

\*   SeedAt:  the tick the transport first reports a round trip; past the
\*            horizon (never) by default.
\*   SeedRto: the resend delay the seed gives, in ticks.
\*   SeedRule: TRUE takes the seed (the rule); FALSE ignores it (the
\*            reversed witness).
\* (SeedAt <- SeedAtEarly, SeedRto <- SeedRtoMeasured, SeedRule <- SeedRuleOff)
SeedAt == ChunkFirst + RefuseFor + Timeout + 1
SeedRto == RtoInitial
SeedRule == TRUE
SeedAtEarly == 2
SeedRtoMeasured == 3
SeedRuleOff == FALSE

\*   Room:    the packets an update writes, one rn_ordered_write call each;
\*            0 (the default) is Walk, every due record written at once.
\*   Pending: the never-sent chunks of the chunk's block (0 by default).
\*   Small:   the small record is in flight; FALSE starts it acked.
\*   UnsentFirst: FALSE walks each class's numbers on the wire before the
\*            never-sent ones (the rule); TRUE walks each class from its
\*            head, never-sent first (the reversed witness).
\* (Room <- RoomOne or RoomTwo, Pending <- PendingLong, Small <- SmallOff,
\*  UnsentFirst <- UnsentFirstOn)
Room == 0
Pending == 0
Small == TRUE
UnsentFirst == FALSE
RoomOne == 1
RoomTwo == 2
PendingLong == 2 * Timeout + 2
SmallOff == FALSE
UnsentFirstOn == TRUE

Min(a, b) == IF a < b THEN a ELSE b
Max(a, b) == IF a > b THEN a ELSE b

Records == {"s", "c"}

\* RN_SEND_NUMBER_NEVER_SENT and the due-now sentinel of
\* rn_window_ack_packet_failed, -(resend_delay_max + 1): below any tick
\* less every delay, so a record stamped DueNow is due at every tick.
DueNow == -(RtoMax * MaxBackoff + 1)
NeverSent == DueNow - 1
Other(r) == IF r = "s" THEN "c" ELSE "s"

\* The chunk delay's cap: MinAttempts sends fit before the timeout.
ChunkCap == Max(1, Timeout \div MinAttempts)

\* The validation rule of sdk/rocketnet_config.cpp: at least MinAttempts
\* sends of the initial resend delay fit before the timeout.
ConfigAccepted == Timeout >= MinAttempts * RtoInitial

VARIABLES
    t,          \* now, in ticks since the small record's first send
    lastSend,   \* [Records -> tick of the record's last send]
    rto,        \* the current clamped rto, in ticks
    backoff,    \* [Records -> the back-off factor, 1 under the rule]
    attempts,   \* [Records -> sends so far, saturating at MinAttempts]
    acked,      \* [Records -> the record's ack has arrived]
    head,       \* OneFifo: the record at the head of the one FIFO
    phase,      \* "walk": acks, then the resend walk; "tick": time moves
    sampled,    \* an ack of this layer's own has given an RTT sample
    seeded,     \* the transport's round trip was taken as the first sample
    firstSend,  \* [Records -> send_first_sends: the tick of the first send]
    refused,    \* [Records -> the packet carrying it was refused this update]
    pending,    \* the chunk's block's chunks numbered and never sent
    calls       \* Room > 0: the rn_ordered_write calls of this update so far

vars == <<t, lastSend, rto, backoff, attempts, acked, head, phase, sampled, seeded, firstSend, refused, pending, calls>>

\* A small record waits the resend delay alone; a chunk waits it capped.
Cap(r) == IF r = "s" THEN RtoMax ELSE ChunkCap

Delay(r) == Min(rto, Cap(r)) * backoff[r]

\* FirstSend(r): the tick the record is first offered to the walk.
FirstSend(r) == IF r = "c" THEN ChunkFirst ELSE 0
OnWire(r) == lastSend[r] # NeverSent
Clock(r) == IF r = "c" /\ BlockClock THEN 0 ELSE firstSend[r]
Deadline(r) == Clock(r) + Timeout
Horizon == ChunkFirst + RefuseFor + Timeout
Started(r) == t >= FirstSend(r)

\* rn_window_ack_check_timeout: the oldest unacked record that has a first
\* send has stood its timeout; a record never sent has no clock.
TimedOut == \E r \in Records : ~acked[r] /\ OnWire(r) /\ t >= Deadline(r)

Due(r) == ~acked[r] /\ ~TimedOut /\ Started(r) /\ (attempts[r] = 0 \/ t >= lastSend[r] + Delay(r))

Live == ~TimedOut /\ \E r \in Records : ~acked[r]

\* rn_window_ack_seed_rtt is owed this update: the transport has a round
\* trip and the estimator no sample (has_rtt = 0).
SeedOwed == SeedRule /\ ~sampled /\ ~seeded /\ t >= SeedAt

TypeOK ==
    /\ t \in 0..Horizon
    /\ lastSend \in [Records -> 0..Horizon \cup {DueNow, NeverSent}]
    /\ firstSend \in [Records -> 0..Horizon \cup {NeverSent}]
    /\ refused \in [Records -> BOOLEAN]
    /\ rto \in RtoMin..RtoMax
    /\ backoff \in [Records -> {1, 2, 4, 8}]
    /\ \A r \in Records : backoff[r] <= MaxBackoff
    /\ attempts \in [Records -> 0..MinAttempts]
    /\ acked \in [Records -> BOOLEAN]
    /\ head \in Records
    /\ phase \in {"walk", "tick"}
    /\ sampled \in BOOLEAN
    /\ seeded \in BOOLEAN
    /\ pending \in 0..Pending
    /\ calls \in 0..Room

Init ==
    /\ t = 0
    /\ lastSend = [r \in Records |-> IF FirstSend(r) = 0 /\ RefuseFor = 0 THEN 0 ELSE NeverSent]
    /\ firstSend = lastSend
    /\ rto = RtoInitial
    /\ backoff = [r \in Records |-> 1]
    /\ attempts = [r \in Records |-> IF FirstSend(r) = 0 /\ RefuseFor = 0 THEN 1 ELSE 0]
    /\ acked = [r \in Records |-> IF r = "c" THEN ~Chunk ELSE ~Small]
    /\ head \in (IF Chunk THEN Records ELSE {"s"})
    /\ phase = "walk"
    /\ sampled = FALSE
    /\ seeded = FALSE
    /\ refused = [r \in Records |-> FALSE]
    /\ pending = Pending
    /\ calls = 0

\* The one FIFO's walk: from the head while due, stopping at the first live
\* record that is not.
FirstLive == ~acked[head]
SendFirst == FirstLive /\ Due(head)
SendSecond == Due(Other(head)) /\ (~FirstLive \/ SendFirst)

Sent(r) ==
    IF OneFifo
    THEN IF r = head THEN SendFirst ELSE SendSecond
    ELSE Due(r)                     \* one FIFO per class, each walked while due

\* A record written moves to its FIFO's tail; the one FIFO's head is the
\* record with the older last send.
NextHead ==
    IF ~FirstLive THEN Other(head)
    ELSE IF SendFirst /\ ~SendSecond /\ ~acked[Other(head)] THEN Other(head)
    ELSE head

\* rn_ordered_walk in rn_ordered_write: every due record the walk reaches
\* is resent.
Walk ==
    /\ Room = 0
    /\ phase = "walk"
    /\ Live
    /\ ~SeedOwed
    /\ lastSend' = [r \in Records |-> IF Sent(r) THEN t ELSE lastSend[r]]
    /\ attempts' = [r \in Records |-> IF Sent(r) THEN Min(attempts[r] + 1, MinAttempts) ELSE attempts[r]]
    /\ backoff' = [r \in Records |-> IF Sent(r) THEN Min(backoff[r] * 2, MaxBackoff) ELSE backoff[r]]
    /\ firstSend' = [r \in Records |-> IF Sent(r) /\ ~OnWire(r) THEN t ELSE firstSend[r]]
    /\ head' = NextHead
    /\ refused' = [r \in Records |-> FALSE]
    /\ phase' = "tick"
    /\ UNCHANGED <<t, rto, acked, sampled, seeded, pending, calls>>

\* rn_window_ack_packet_failed: the walk of Walk, but finish_packet refuses
\* the packet carrying the records of F, each never on the wire and inside
\* its refusal window.  Under the rule a refused record stays NeverSent with
\* no first send and goes back to the head of its FIFO; the other due
\* records are written as in Walk.  StampRefused: the refusal records the
\* first send (rn_window_ack_record_first_send) and stamps DueNow, and a
\* record stamped so is refused again as a resend of the same sentinel.
Refusable(r) == Sent(r) /\ attempts[r] = 0 /\ t < FirstSend(r) + RefuseFor

WalkRefused(F) ==
    /\ Room = 0
    /\ phase = "walk"
    /\ Live
    /\ ~SeedOwed
    /\ F # {}
    /\ \A r \in F : Refusable(r)
    /\ lastSend' = [r \in Records |->
            IF r \in F THEN (IF StampRefused THEN DueNow ELSE lastSend[r])
            ELSE IF Sent(r) THEN t ELSE lastSend[r]]
    /\ firstSend' = [r \in Records |->
            IF r \in F THEN (IF StampRefused /\ ~OnWire(r) THEN t ELSE firstSend[r])
            ELSE IF Sent(r) /\ ~OnWire(r) THEN t ELSE firstSend[r]]
    /\ attempts' = [r \in Records |-> IF Sent(r) /\ r \notin F THEN Min(attempts[r] + 1, MinAttempts) ELSE attempts[r]]
    /\ backoff' = [r \in Records |-> IF Sent(r) /\ r \notin F THEN Min(backoff[r] * 2, MaxBackoff) ELSE backoff[r]]
    /\ head' = (IF head \in F THEN head ELSE CHOOSE r \in F : TRUE)
    /\ refused' = [r \in Records |-> r \in F]
    /\ phase' = "tick"
    /\ UNCHANGED <<t, rto, acked, sampled, seeded, pending, calls>>

\* The paced walk (Room > 0), one rn_ordered_write call for one packet.
\* The items a call can plan are "s", "c" and "u", the first pending chunk.
\* Under the rule rn_ordered_write walks the numbers on the wire of each
\* class from its sent mark (ack->resend_sent, ack->chunk_sent), small then
\* chunk, then the never-sent numbers of each class from its head: the
\* order s on the wire, c on the wire, s never sent, c never sent, u.
\* UnsentFirst walks each class from its head, never-sent numbers first:
\* s never sent, s on the wire, c never sent, u, c on the wire.  The chunk
\* is ahead of its block's pending chunks in number order.  The walk plans
\* while the plan holds less than the call's one packet: a chunk fills it,
\* the small record does not, so the plan is the order up to its first
\* chunk item, and Planned is that prefix as a set.
SentDue(r) == Due(r) /\ OnWire(r)
UnsentDue(r) == Due(r) /\ ~OnWire(r)
PendingDue == pending > 0 /\ ~TimedOut /\ Started("c")

Planned ==
    IF UnsentFirst
    THEN { x \in {"s"} : Due("s") } \cup
         { x \in {"c"} : UnsentDue("c") \/ (SentDue("c") /\ ~PendingDue) } \cup
         { x \in {"u"} : PendingDue /\ ~UnsentDue("c") }
    ELSE { x \in {"s"} : SentDue("s") \/ (UnsentDue("s") /\ ~SentDue("c")) } \cup
         { x \in {"c"} : Due("c") } \cup
         { x \in {"u"} : PendingDue /\ ~Due("c") }

\* The builder writes one packet of the plan, in the budget's class order:
\* a retransmit (a record on the wire) before ordered progress; within a
\* class the scheduler's levels and channels pick, so either may go.  A
\* chunk travels alone, and the small record beside a chunk takes its own
\* packet, so the call writes one item.
Retransmit(x) == x # "u" /\ OnWire(x)
Picks == IF \E x \in Planned : Retransmit(x)
         THEN {x \in Planned : Retransmit(x)}
         ELSE Planned

WriteOne(x) ==
    /\ Room > 0
    /\ phase = "walk"
    /\ Live
    /\ ~SeedOwed
    /\ calls < Room
    /\ x \in Picks
    /\ IF x = "u"
       THEN /\ pending' = pending - 1
            /\ UNCHANGED <<lastSend, attempts, backoff, firstSend>>
       ELSE /\ lastSend' = [lastSend EXCEPT ![x] = t]
            /\ attempts' = [attempts EXCEPT ![x] = Min(@ + 1, MinAttempts)]
            /\ backoff' = [backoff EXCEPT ![x] = Min(@ * 2, MaxBackoff)]
            /\ firstSend' = [firstSend EXCEPT ![x] = IF OnWire(x) THEN @ ELSE t]
            /\ UNCHANGED pending
    /\ calls' = calls + 1
    /\ UNCHANGED <<t, rto, acked, head, phase, sampled, seeded, refused>>

\* The update's writes end: its Room calls are spent, or a call finds
\* nothing to plan (rn_ordered_write returns 0 and the endpoint stops).
EndWrites ==
    /\ Room > 0
    /\ phase = "walk"
    /\ Live
    /\ ~SeedOwed
    /\ calls = Room \/ Planned = {}
    /\ calls' = 0
    /\ refused' = [r \in Records |-> FALSE]
    /\ phase' = "tick"
    /\ UNCHANGED <<t, lastSend, rto, backoff, attempts, acked, head, sampled, seeded, firstSend, pending>>

\* rn_window_ack_on_ack of another packet: one RTT sample.
Sample(x) ==
    /\ phase = "walk"
    /\ calls = 0
    /\ Live
    /\ rto' = x
    /\ backoff' = [r \in Records |-> 1]
    /\ sampled' = TRUE
    /\ UNCHANGED <<t, lastSend, attempts, acked, head, phase, seeded, firstSend, refused, pending, calls>>

\* rn_window_ack_seed_rtt from the message object's update: the transport's
\* round trip taken as the first sample, after the update's acks and before
\* its walk.
Seed ==
    /\ phase = "walk"
    /\ calls = 0
    /\ Live
    /\ SeedOwed
    /\ rto' = SeedRto
    /\ backoff' = [r \in Records |-> 1]
    /\ seeded' = TRUE
    /\ UNCHANGED <<t, lastSend, attempts, acked, head, phase, sampled, firstSend, refused, pending, calls>>

\* rn_window_ack_on_ack of a packet carrying record r.
Ack(r) ==
    /\ phase = "walk"
    /\ calls = 0
    /\ ~acked[r]
    /\ ~TimedOut
    /\ attempts[r] > 0
    /\ acked' = [acked EXCEPT ![r] = TRUE]
    /\ UNCHANGED <<t, lastSend, rto, backoff, attempts, head, phase, sampled, seeded, firstSend, refused, pending, calls>>

\* The update ends and time moves on one tick.
Tick ==
    /\ phase = "tick"
    /\ Live
    /\ t' = t + 1
    /\ phase' = "walk"
    /\ UNCHANGED <<lastSend, rto, backoff, attempts, acked, head, sampled, seeded, firstSend, refused, pending, calls>>

\* Every record acked, or the timeout fired: nothing more happens.
Done ==
    /\ ~Live
    /\ UNCHANGED vars

Next ==
    \/ Walk
    \/ \E F \in SUBSET Records : WalkRefused(F)
    \/ \E x \in {"s", "c", "u"} : WriteOne(x)
    \/ EndWrites
    \/ \E x \in RtoMin..RtoMax : Sample(x)
    \/ Seed
    \/ \E r \in Records : Ack(r)
    \/ Tick
    \/ Done

Spec == Init /\ [][Next]_vars

\* The property the rule exists for, for each class.
AttemptsBeforeTimeout ==
    \A r \in Records : (~acked[r] /\ OnWire(r) /\ t = Deadline(r)) => attempts[r] >= MinAttempts

\* Under the rule no unacked record waits more than its delay's bound for
\* its next send while the connection is up: RtoMax for a small record,
\* Min(RtoMax, ChunkCap) for a chunk.
ResendGapBounded ==
    \A r \in Records :
        (~acked[r] /\ ~TimedOut /\ attempts[r] > 0) => t - lastSend[r] <= Min(RtoMax, Cap(r)) * MaxBackoff

\* A due record is sent in the update it falls due: no update ends with a
\* record of either class still due, whatever the other class does, but
\* for one whose packet the transport refused.  The exception is the code's
\* behaviour, not a loosening: rn_window_ack_packet_failed pushes every
\* unacked number of the refused packet back to the head of its FIFO and
\* leaves it due at once (a number never on the wire keeps
\* RN_SEND_NUMBER_NEVER_SENT, so attempts = 0; a resend gets a negative
\* due-now sentinel, -(resend_delay_max + 2) after
\* rn_packet_builder_packet_failed), so after the walk in which its packet
\* was refused the record is due by construction and waits for the next
\* walk.  refused[r] is set only by WalkRefused and only for a record that
\* walk offered (Refusable implies Sent), and every walk clears it, so a
\* record that falls due and is not offered still fails this invariant.
\* No sentinel value can remove the exception: the sentinel exists to make
\* the record due.
NoDueRecordHeld ==
    phase = "tick" => \A r \in Records : ~Due(r) \/ refused[r]

\* From the update the transport first reports a round trip until this
\* layer's own first sample, no unacked record waits longer than the
\* seeded delay for its next send: a record last sent before SeedAt is
\* resent by SeedAt + its delay, not by its last send + RtoInitial.
SeededGapBounded ==
    \A r \in Records :
        (~acked[r] /\ ~TimedOut /\ attempts[r] > 0 /\ ~sampled /\ t >= SeedAt) =>
            t - Max(lastSend[r], SeedAt) <= Min(SeedRto, Cap(r)) * MaxBackoff

=============================================================================
