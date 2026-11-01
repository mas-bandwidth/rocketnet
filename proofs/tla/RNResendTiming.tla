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
(***************************************************************************)
EXTENDS Naturals

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

\*   SeedAt:  the tick the transport first reports a round trip; past the
\*            horizon (never) by default.
\*   SeedRto: the resend delay the seed gives, in ticks.
\*   SeedRule: TRUE takes the seed (the rule); FALSE ignores it (the
\*            reversed witness).
\* (SeedAt <- SeedAtEarly, SeedRto <- SeedRtoMeasured, SeedRule <- SeedRuleOff)
SeedAt == ChunkFirst + Timeout + 1
SeedRto == RtoInitial
SeedRule == TRUE
SeedAtEarly == 2
SeedRtoMeasured == 3
SeedRuleOff == FALSE

Min(a, b) == IF a < b THEN a ELSE b
Max(a, b) == IF a > b THEN a ELSE b

Records == {"s", "c"}
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
    seeded      \* the transport's round trip was taken as the first sample

vars == <<t, lastSend, rto, backoff, attempts, acked, head, phase, sampled, seeded>>

\* A small record waits the resend delay alone; a chunk waits it capped.
Cap(r) == IF r = "s" THEN RtoMax ELSE ChunkCap

Delay(r) == Min(rto, Cap(r)) * backoff[r]

FirstSend(r) == IF r = "c" THEN ChunkFirst ELSE 0
Clock(r) == IF r = "c" /\ BlockClock THEN 0 ELSE FirstSend(r)
Deadline(r) == Clock(r) + Timeout
Horizon == ChunkFirst + Timeout
Started(r) == t >= FirstSend(r)

\* rn_window_ack_check_timeout: the oldest unacked record that has been
\* sent has stood its timeout.
TimedOut == \E r \in Records : ~acked[r] /\ Started(r) /\ t >= Deadline(r)

Due(r) == ~acked[r] /\ ~TimedOut /\ Started(r) /\ (attempts[r] = 0 \/ t >= lastSend[r] + Delay(r))

Live == ~TimedOut /\ \E r \in Records : ~acked[r]

\* rn_window_ack_seed_rtt is owed this update: the transport has a round
\* trip and the estimator no sample (has_rtt = 0).
SeedOwed == SeedRule /\ ~sampled /\ ~seeded /\ t >= SeedAt

TypeOK ==
    /\ t \in 0..Horizon
    /\ lastSend \in [Records -> 0..Horizon]
    /\ rto \in RtoMin..RtoMax
    /\ backoff \in [Records -> {1, 2, 4, 8}]
    /\ \A r \in Records : backoff[r] <= MaxBackoff
    /\ attempts \in [Records -> 0..MinAttempts]
    /\ acked \in [Records -> BOOLEAN]
    /\ head \in Records
    /\ phase \in {"walk", "tick"}
    /\ sampled \in BOOLEAN
    /\ seeded \in BOOLEAN

Init ==
    /\ t = 0
    /\ lastSend = [r \in Records |-> FirstSend(r)]
    /\ rto = RtoInitial
    /\ backoff = [r \in Records |-> 1]
    /\ attempts = [r \in Records |-> IF FirstSend(r) = 0 THEN 1 ELSE 0]
    /\ acked = [r \in Records |-> IF r = "c" THEN ~Chunk ELSE FALSE]
    /\ head \in (IF Chunk THEN Records ELSE {"s"})
    /\ phase = "walk"
    /\ sampled = FALSE
    /\ seeded = FALSE

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
    /\ phase = "walk"
    /\ Live
    /\ ~SeedOwed
    /\ lastSend' = [r \in Records |-> IF Sent(r) THEN t ELSE lastSend[r]]
    /\ attempts' = [r \in Records |-> IF Sent(r) THEN Min(attempts[r] + 1, MinAttempts) ELSE attempts[r]]
    /\ backoff' = [r \in Records |-> IF Sent(r) THEN Min(backoff[r] * 2, MaxBackoff) ELSE backoff[r]]
    /\ head' = NextHead
    /\ phase' = "tick"
    /\ UNCHANGED <<t, rto, acked, sampled, seeded>>

\* rn_window_ack_on_ack of another packet: one RTT sample.
Sample(x) ==
    /\ phase = "walk"
    /\ Live
    /\ rto' = x
    /\ backoff' = [r \in Records |-> 1]
    /\ sampled' = TRUE
    /\ UNCHANGED <<t, lastSend, attempts, acked, head, phase, seeded>>

\* rn_window_ack_seed_rtt from the message object's update: the transport's
\* round trip taken as the first sample, after the update's acks and before
\* its walk.
Seed ==
    /\ phase = "walk"
    /\ Live
    /\ SeedOwed
    /\ rto' = SeedRto
    /\ backoff' = [r \in Records |-> 1]
    /\ seeded' = TRUE
    /\ UNCHANGED <<t, lastSend, attempts, acked, head, phase, sampled>>

\* rn_window_ack_on_ack of a packet carrying record r.
Ack(r) ==
    /\ phase = "walk"
    /\ ~acked[r]
    /\ ~TimedOut
    /\ attempts[r] > 0
    /\ acked' = [acked EXCEPT ![r] = TRUE]
    /\ UNCHANGED <<t, lastSend, rto, backoff, attempts, head, phase, sampled, seeded>>

\* The update ends and time moves on one tick.
Tick ==
    /\ phase = "tick"
    /\ Live
    /\ t' = t + 1
    /\ phase' = "walk"
    /\ UNCHANGED <<lastSend, rto, backoff, attempts, acked, head, sampled, seeded>>

\* Every record acked, or the timeout fired: nothing more happens.
Done ==
    /\ ~Live
    /\ UNCHANGED vars

Next ==
    \/ Walk
    \/ \E x \in RtoMin..RtoMax : Sample(x)
    \/ Seed
    \/ \E r \in Records : Ack(r)
    \/ Tick
    \/ Done

Spec == Init /\ [][Next]_vars

\* The property the rule exists for, for each class.
AttemptsBeforeTimeout ==
    \A r \in Records : (~acked[r] /\ Started(r) /\ t = Deadline(r)) => attempts[r] >= MinAttempts

\* Under the rule no unacked record waits more than its delay's bound for
\* its next send while the connection is up: RtoMax for a small record,
\* Min(RtoMax, ChunkCap) for a chunk.
ResendGapBounded ==
    \A r \in Records :
        (~acked[r] /\ ~TimedOut /\ attempts[r] > 0) => t - lastSend[r] <= Min(RtoMax, Cap(r)) * MaxBackoff

\* A due record is sent in the update it falls due: no update ends with a
\* record of either class still due, whatever the other class does.
NoDueRecordHeld ==
    phase = "tick" => \A r \in Records : ~Due(r)

\* From the update the transport first reports a round trip until this
\* layer's own first sample, no unacked record waits longer than the
\* seeded delay for its next send: a record last sent before SeedAt is
\* resent by SeedAt + its delay, not by its last send + RtoInitial.
SeededGapBounded ==
    \A r \in Records :
        (~acked[r] /\ ~TimedOut /\ attempts[r] > 0 /\ ~sampled /\ t >= SeedAt) =>
            t - Max(lastSend[r], SeedAt) <= Min(SeedRto, Cap(r)) * MaxBackoff

=============================================================================
