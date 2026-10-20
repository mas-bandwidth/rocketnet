--------------------------- MODULE RNResendTiming ---------------------------
(***************************************************************************)
(* rocketnet: resend timing of reliable records (standard/MESSAGES.md m9.3  *)
(* "RTO", "Resend" and "Giving up"; the code is                            *)
(* rn_window_ack_resend_delay, rn_window_ack_chunk_resend_delay,           *)
(* rn_window_ack_resend and rn_window_ack_check_timeout in                 *)
(* sdk/rocketnet_window_ack.h and .cpp, rn_ordered_write in                *)
(* sdk/rocketnet_channel.cpp and rn_message_ordered_due in                 *)
(* sdk/rocketnet_message_server.cpp).                                      *)
(*                                                                         *)
(* RNWindow keeps the RTO abstract; this module is the timing alone.  Time *)
(* is in ticks of one unit each (the committed instances use 100 ms); every *)
(* tick is an update.  There are two numbered, unacked records of the two  *)
(* delay classes: "s", a single-packet record, and "c", a chunk of a block *)
(* (a record whose send descriptor has more than one fragment).  Both are  *)
(* first sent at tick 0, in either order.  A record is due when            *)
(*     t >= lastSend[r] + Delay(r) where                                   *)
(*     Delay("s") = Min(rto, Interval)  * backoff["s"]                     *)
(*     Delay("c") = Min(rto, ChunkCap) * backoff["c"]                      *)
(* ChunkCap = Timeout \div MinAttempts keeps MinAttempts chunk sends       *)
(* inside the timeout whatever rto_max is; at the defaults (2.5 s against  *)
(* rto_max 250 ms) a chunk waits rto alone.  With Chunk = FALSE the chunk  *)
(* starts acked and the module is the one-record timing of the first cut.  *)
(*                                                                         *)
(* An update is two steps.  In phase "walk" the acks that arrived are      *)
(* processed (Sample, Ack) and then Walk writes the resend FIFOs; in phase *)
(* "tick" time moves on one tick.  The code keeps one FIFO per delay class *)
(* and walks each from its head while due (OneFifo = FALSE): every due     *)
(* record is written in the update it falls due.  OneFifo = TRUE is one    *)
(* FIFO ordered by last send with both delays, walked from its head and    *)
(* stopped at the first record not due: a chunk at the head that is not    *)
(* due holds back the small record behind it (the reversed witness         *)
(* MCRNResendTimingBrokenOneFifo.cfg).                                     *)
(*                                                                         *)
(* An RTT sample (the ack of any other packet) moves rto to any value in   *)
(* RtoMin..RtoMax, the abstraction of                                      *)
(* clamp(srtt * scale + 2 * rttvar, rto_min, rto_max), and resets the      *)
(* back-off to 1.  Before any sample rto is RtoInitial.  A record's own    *)
(* ack may come at any update, or never: every send may be lost.  At tick  *)
(* Timeout an unacked record ends the connection (RELIABLE_TIMEOUT) and    *)
(* nothing more is sent.                                                   *)
(*                                                                         *)
(* MaxBackoff is the back-off parameter: 1 is the rule (no back-off, the   *)
(* delay is the same for every resend); 8 is the first version's           *)
(* doubling per resend, 1, 2, 4, 8.  Interval is resend_interval; a value  *)
(* of at least RtoMax means no cap.                                        *)
(*                                                                         *)
(* AttemptsBeforeTimeout: a record still unacked at the timeout was sent   *)
(* at least MinAttempts times (ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS).     *)
(* attempts saturates at MinAttempts to keep the state space small.        *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    Interval,       \* resend_interval, in ticks
    RtoInitial,     \* rto before the first sample, in ticks
    RtoMin,         \* rto_min, in ticks
    RtoMax,         \* rto_max, in ticks
    Timeout,        \* reliable_timeout, in ticks
    MaxBackoff,     \* 1: no back-off (the rule); 8: doubling to 8
    MinAttempts     \* the sends that must fit before the timeout

ASSUME /\ Interval \in Nat \ {0}
       /\ RtoMin \in Nat \ {0} /\ RtoMax \in Nat /\ RtoMin <= RtoMax
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

Min(a, b) == IF a < b THEN a ELSE b
Max(a, b) == IF a > b THEN a ELSE b

Records == {"s", "c"}
Other(r) == IF r = "s" THEN "c" ELSE "s"

\* The chunk delay's cap: MinAttempts sends fit before the timeout.
ChunkCap == Max(1, Timeout \div MinAttempts)

\* The validation rule of sdk/rocketnet_config.cpp: at least MinAttempts
\* sends of the longest first delay fit before the timeout.
ConfigAccepted == Timeout >= MinAttempts * Max(Interval, RtoInitial)

VARIABLES
    t,          \* now, in ticks since the first send
    lastSend,   \* [Records -> tick of the record's last send]
    rto,        \* the current clamped rto, in ticks
    backoff,    \* [Records -> the back-off factor, 1 under the rule]
    attempts,   \* [Records -> sends so far, saturating at MinAttempts]
    acked,      \* [Records -> the record's ack has arrived]
    head,       \* OneFifo: the record at the head of the one FIFO
    phase       \* "walk": acks, then the resend walk; "tick": time moves

vars == <<t, lastSend, rto, backoff, attempts, acked, head, phase>>

Cap(r) == IF r = "s" THEN Interval ELSE ChunkCap

Delay(r) == Min(rto, Cap(r)) * backoff[r]

Due(r) == ~acked[r] /\ t < Timeout /\ t >= lastSend[r] + Delay(r)

Live == t < Timeout /\ \E r \in Records : ~acked[r]

TypeOK ==
    /\ t \in 0..Timeout
    /\ lastSend \in [Records -> 0..Timeout]
    /\ rto \in RtoMin..RtoMax
    /\ backoff \in [Records -> {1, 2, 4, 8}]
    /\ \A r \in Records : backoff[r] <= MaxBackoff
    /\ attempts \in [Records -> 1..MinAttempts]
    /\ acked \in [Records -> BOOLEAN]
    /\ head \in Records
    /\ phase \in {"walk", "tick"}

Init ==
    /\ t = 0
    /\ lastSend = [r \in Records |-> 0]
    /\ rto = RtoInitial
    /\ backoff = [r \in Records |-> 1]
    /\ attempts = [r \in Records |-> 1]
    /\ acked = [r \in Records |-> IF r = "c" THEN ~Chunk ELSE FALSE]
    /\ head \in (IF Chunk THEN Records ELSE {"s"})
    /\ phase = "walk"

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

\* rn_window_ack_resend / rn_ordered_write: every due record the walk
\* reaches is resent.
Walk ==
    /\ phase = "walk"
    /\ Live
    /\ lastSend' = [r \in Records |-> IF Sent(r) THEN t ELSE lastSend[r]]
    /\ attempts' = [r \in Records |-> IF Sent(r) THEN Min(attempts[r] + 1, MinAttempts) ELSE attempts[r]]
    /\ backoff' = [r \in Records |-> IF Sent(r) THEN Min(backoff[r] * 2, MaxBackoff) ELSE backoff[r]]
    /\ head' = NextHead
    /\ phase' = "tick"
    /\ UNCHANGED <<t, rto, acked>>

\* rn_window_ack_on_ack of another packet: one RTT sample.
Sample(x) ==
    /\ phase = "walk"
    /\ Live
    /\ rto' = x
    /\ backoff' = [r \in Records |-> 1]
    /\ UNCHANGED <<t, lastSend, attempts, acked, head, phase>>

\* rn_window_ack_on_ack of a packet carrying record r.
Ack(r) ==
    /\ phase = "walk"
    /\ ~acked[r]
    /\ t < Timeout
    /\ acked' = [acked EXCEPT ![r] = TRUE]
    /\ UNCHANGED <<t, lastSend, rto, backoff, attempts, head, phase>>

\* The update ends and time moves on one tick.
Tick ==
    /\ phase = "tick"
    /\ Live
    /\ t' = t + 1
    /\ phase' = "walk"
    /\ UNCHANGED <<lastSend, rto, backoff, attempts, acked, head>>

\* Every record acked, or the timeout fired: nothing more happens.
Done ==
    /\ ~Live
    /\ UNCHANGED vars

Next ==
    \/ Walk
    \/ \E x \in RtoMin..RtoMax : Sample(x)
    \/ \E r \in Records : Ack(r)
    \/ Tick
    \/ Done

Spec == Init /\ [][Next]_vars

\* The property the rule exists for, for each class.
AttemptsBeforeTimeout ==
    \A r \in Records : (~acked[r] /\ t = Timeout) => attempts[r] >= MinAttempts

\* Under the rule no unacked record waits more than its cap for its next
\* send while the connection is up: Interval for a small record, the RTO
\* bound for a chunk.
ResendGapBounded ==
    \A r \in Records :
        (~acked[r] /\ t < Timeout) => t - lastSend[r] <= Min(RtoMax, Cap(r)) * MaxBackoff

\* A due small record is sent in the update it falls due: no update ends
\* with it still due, whatever the chunk beside it does.
SmallNeverHeldByChunk ==
    phase = "tick" => ~Due("s")

=============================================================================
