--------------------------- MODULE RNResendTiming ---------------------------
(***************************************************************************)
(* rocketnet: the resend timing of one reliable record (docs/CHANNELS.md   *)
(* 6.4 "RTO", "Resend" and "Giving up"; the code is                        *)
(* rn_window_ack_resend_delay, rn_window_ack_resend and                    *)
(* rn_window_ack_check_timeout in sdk/rocketnet_window_ack.h and .cpp,     *)
(* rn_ordered_write in sdk/rocketnet_channel.cpp and                       *)
(* rn_message_ordered_due in sdk/rocketnet_message_server.cpp).            *)
(*                                                                         *)
(* RNWindow keeps the RTO abstract; this module is the timing alone.  Time *)
(* is in ticks of one unit each (the committed instances use 100 ms); every *)
(* tick is an update.  There is one numbered, unacked record.  It is first *)
(* sent at tick 0.  An update resends it when                              *)
(*     t >= lastSend + Delay,  Delay = Min(rto, Interval) * backoff        *)
(* and time does not pass an update with a resend due (the update writes   *)
(* every due record before it returns).  An RTT sample (the ack of any     *)
(* other packet) moves rto to any value in RtoMin..RtoMax, the abstraction *)
(* of clamp(srtt * scale + 2 * rttvar, rto_min, rto_max), and resets the   *)
(* back-off to 1.  Before any sample rto is RtoInitial.  The record's own  *)
(* ack may come at any tick, or never: every send may be lost.  At tick    *)
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

Min(a, b) == IF a < b THEN a ELSE b
Max(a, b) == IF a > b THEN a ELSE b

\* The validation rule of sdk/rocketnet_config.cpp: at least MinAttempts
\* sends of the longest first delay fit before the timeout.
ConfigAccepted == Timeout >= MinAttempts * Max(Interval, RtoInitial)

VARIABLES
    t,          \* now, in ticks since the first send
    lastSend,   \* tick of the record's last send
    rto,        \* the current clamped rto, in ticks
    backoff,    \* the back-off factor, 1 under the rule
    attempts,   \* sends of the record so far, saturating at MinAttempts
    acked       \* the record's ack has arrived

vars == <<t, lastSend, rto, backoff, attempts, acked>>

Delay == Min(rto, Interval) * backoff

Due == ~acked /\ t < Timeout /\ t >= lastSend + Delay

TypeOK ==
    /\ t \in 0..Timeout
    /\ lastSend \in 0..Timeout
    /\ rto \in RtoMin..RtoMax
    /\ backoff \in {1, 2, 4, 8}
    /\ backoff <= MaxBackoff
    /\ attempts \in 1..MinAttempts
    /\ acked \in BOOLEAN

Init ==
    /\ t = 0
    /\ lastSend = 0
    /\ rto = RtoInitial
    /\ backoff = 1
    /\ attempts = 1
    /\ acked = FALSE

\* rn_window_ack_resend / rn_ordered_write: the record is due and resent.
Resend ==
    /\ Due
    /\ lastSend' = t
    /\ attempts' = Min(attempts + 1, MinAttempts)
    /\ backoff' = Min(backoff * 2, MaxBackoff)
    /\ UNCHANGED <<t, rto, acked>>

\* rn_window_ack_on_ack of another packet: one RTT sample.
Sample(r) ==
    /\ ~acked
    /\ t < Timeout
    /\ rto' = r
    /\ backoff' = 1
    /\ UNCHANGED <<t, lastSend, attempts, acked>>

\* rn_window_ack_on_ack of a packet carrying the record.
Ack ==
    /\ ~acked
    /\ t < Timeout
    /\ acked' = TRUE
    /\ UNCHANGED <<t, lastSend, rto, backoff, attempts>>

\* The update ends with nothing due and time moves on one tick.
Tick ==
    /\ ~acked
    /\ ~Due
    /\ t < Timeout
    /\ t' = t + 1
    /\ UNCHANGED <<lastSend, rto, backoff, attempts, acked>>

\* Acked, or the timeout fired: nothing more happens.
Done ==
    /\ acked \/ t = Timeout
    /\ UNCHANGED vars

Next ==
    \/ Resend
    \/ \E r \in RtoMin..RtoMax : Sample(r)
    \/ Ack
    \/ Tick
    \/ Done

Spec == Init /\ [][Next]_vars

\* The property the rule exists for.
AttemptsBeforeTimeout ==
    (~acked /\ t = Timeout) => attempts >= MinAttempts

\* Under the rule no unacked record waits more than Interval ticks for its
\* next send while the connection is up.
ResendGapBounded ==
    (~acked /\ t < Timeout) => t - lastSend <= Interval * MaxBackoff

=============================================================================
