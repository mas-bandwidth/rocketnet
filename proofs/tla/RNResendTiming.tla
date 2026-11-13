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
(* (OneFifo = FALSE) and walks each class's numbers on the wire from its  *)
(* sent mark while due, then its never-sent numbers from its head; Walk   *)
(* has room for every due record and writes each in the update it falls   *)
(* due (the paced walk below has less room).  OneFifo = TRUE is one        *)
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
(* ends the connection (RELIABLE_TIMEOUT), when the timeout is configured, *)
(* and nothing more is sent.  The                                          *)
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
(* already on the wire is WriteRefused in the paced walk: in the code it   *)
(* keeps its send_first_sends entry, so its timeout clock, takes a         *)
(* due-now sentinel (-(resend_delay_max + 2) after                         *)
(* rn_packet_builder_packet_failed; DueNow here) and goes back on the wire *)
(* as its class's sent mark (rn_resend_fifo_push_mark), so the next call   *)
(* plans it as a retransmit (RetryPlanned).  RefusedOffWire = TRUE is the  *)
(* code before the fix: the refused resend went back to the head of its   *)
(* FIFO, among the never-sent numbers, and was planned as ordered progress *)
(* behind every due number on the wire (the reversed witness               *)
(* MCRNResendTimingBrokenRefusedOffWire.cfg).                              *)
(*                                                                         *)
(* What the invariants claim.  In each write call the due numbers on the   *)
(* wire are planned first, the small class before the chunk class, each   *)
(* class oldest last send first, ahead of every number not on the wire.  A *)
(* due number is held only while the update's packets (the send budget,   *)
(* packets_per_update, the transport) go to numbers ahead of it in that    *)
(* order, or its packet is refused.  NoDueRecordHeld, ResendGapBounded and *)
(* AttemptsBeforeTimeout are checked for one lost chunk and one small      *)
(* record with a packet for each in every update; they are not claims      *)
(* about two due numbers and one packet (MCRNResendTimingPacedSmallRoomOne *)
(* .cfg is the recorded expected failure), about a budget that admits no   *)
(* packet in an update, or about refused resends.  For a refused resend    *)
(* the claim is RetryPlanned.                                              *)
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
(* come before the update's first call, as the code reads them.  With      *)
(* RefuseResends a call may instead have its packet refused when it        *)
(* carries a record on the wire (WriteRefused); the call is spent.         *)
(***************************************************************************)
EXTENDS Integers

\* Revision 1.1 (standard/MESSAGES.md m9.2, m9.3, m1.9).  Everything above
\* holds with the reliable timeout configured (TimeoutOn, every case named
\* above).  With it off, the default of v1.1.0, the chunk waits rto
\* uncapped, and probe pacing acts on a stalled path
\* (rn_window_ack_probing, the skip in rn_ordered_walk): once Probe ticks
\* pass with no reliable number newly acknowledged, counted from the later
\* of that progress and U's first send, the walk resends only U among the
\* numbers on the wire; numbers never sent are still sent; only progress
\* releases it (ProbeOnlyU, NoDueRecordHeld with the hold as its
\* exception, EveryReliableAcked).  Probe pacing does not act with the
\* timeout configured.
\*
\* The ends (revision 1.2, design (S) and (K)).  With TrackQuiet the
\* transport's silence timeout is in the model: quiet counts the ticks
\* since anything was heard from the other side (an ack of any packet, or
\* any other packet: KeepAlive), and an update that begins with quiet at
\* SilenceT ends the connection (SilenceEnd: TIMED_OUT, the transport's
\* own clock).  So an end is reachable with the timeout off, and
\* NoEndBeforeT states that every end there is, of either kind, comes
\* after SilenceT ticks with nothing heard; SilenceEndsAtT that a live
\* connection never passes SilenceT ticks of silence.  The reliable
\* timeout (TimedOut) still fires when configured; with it off it fires
\* only under the reversed witness TimeoutWhenOff (the code before
\* v1.1.0), which ends a connection whose other side was heard inside T.
\*
\* The endpoint's due test (rn_endpoint_ordered_due) decides whether a
\* frame is started for the walk at all; it reads each class's FIFO head
\* and asks the walk's own hold (rn_window_ack_held), so a held head stands
\* for U, the one number the walk may resend (FifoDue), and a never-sent
\* number at the head (a block's pending chunk) is due at once.  It
\* reports work exactly when the walk writes some number (DueMeansWritten,
\* for Walk and for each paced call): a frame started for a held
\* connection with nothing to write would be aborted and cost the
\* transport's per-update send capacity (MESSAGES.md m1.8; the capacity
\* itself is RNServerSendFair).
\*
\* The RTT filter of revision 1.1 (rn_window_ack_on_acks,
\* rn_window_ack_on_ack) is RNRttFilter, with send records and a moving
\* estimate; here an ack of another packet moves rto freely (Sample).
\* The state of revision 1.1 is gated so the cases above carry none of
\* it: sinceProgress moves only with the timeout off, and quiet only in
\* the cases that track it.

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
\* (Room <- RoomOne or RoomTwo, Pending <- PendingLong or PendingShort, Small <- SmallOff,
\*  UnsentFirst <- UnsentFirstOn)
Room == 0
Pending == 0
Small == TRUE
UnsentFirst == FALSE
RoomOne == 1
RoomTwo == 2
PendingLong == 2 * Timeout + 2
PendingShort == 2
SmallOff == FALSE
UnsentFirstOn == TRUE

\*   RefuseResends: a paced call carrying a record on the wire may have its
\*            packet refused (WriteRefused); FALSE (never) by default.
\*   RefusedOffWire: FALSE walks a refused resend with the numbers on the
\*            wire, as its class's mark (the rule); TRUE with the never-sent
\*            numbers, as ordered progress (the reversed witness).
\* (RefuseResends <- RefuseResendsOn, RefusedOffWire <- RefusedOffWireOn)
RefuseResends == FALSE
RefusedOffWire == FALSE
RefuseResendsOn == TRUE
RefusedOffWireOn == TRUE

\* Revision 1.1 (MESSAGES.md m9.2, m9.3, m1.9): the reliable timeout is
\* optional, and with it off a stalled path gets probe pacing.
\*   TimeoutOn: reliable_message_timeout_ms is nonzero (the cases before
\*            revision 1.1 all run TRUE).  FALSE is the default of v1.1.0,
\*            zero: no end for an unacknowledged number, no chunk cap, and
\*            Timeout is then only the horizon of the instance.  Past the
\*            horizon time stands still (Tick moves no clock), the
\*            abstraction of "long enough" the liveness case reads.
\*   TimeoutWhenOff: the reversed witness of the code before v1.1.0, the
\*            timeout timed from the first send whatever the configuration.
\* (TimeoutOn <- TimeoutOff, TimeoutWhenOff <- TimeoutWhenOffOn)
TimeoutOn == TRUE
TimeoutWhenOff == FALSE
TimeoutOff == FALSE
TimeoutWhenOffOn == TRUE

\*   Probe:   RN_PROBE_SECONDS in ticks (1 s is 2 ticks of 500 ms).
\*   ProbeMode: "rule" holds every number on the wire but U once Probe
\*            ticks pass with no reliable number newly acknowledged, from
\*            the later of the last progress and U's first send, and only
\*            progress releases it; the reversed witnesses: "off" resends
\*            every due number while stalled (ResendAllWhileStalled), "norelease"
\*            times the hold from U's first send alone, so progress that
\*            leaves U in place does not release it, and "holdsu" holds U
\*            too, a probe that never sends.
\*   Third:   a second small record "s2", numbered after the chunk, on the
\*            wire beside the other two (Walk only): a hold with U in place
\*            and progress is seen only with three numbers.
\* (Probe <- ..., ProbeMode <- ProbeOff / ProbeNoRelease / ProbeHoldsU,
\*  Third <- ThirdOn)
Probe == 2
ProbeMode == "rule"
Third == FALSE
ProbeOff == "off"
ProbeNoRelease == "norelease"
ProbeHoldsU == "holdsu"
ThirdOn == TRUE

\*   DueRule: TRUE is the due test asking the walk's hold, a held head
\*            standing for U (the rule); FALSE is the due test blind to the
\*            hold, the code of v1.1.0: a held head past its delay reports
\*            work the walk never writes (the reversed witness
\*            MCRNResendTimingBrokenDueIgnoresHold.cfg).
\* (DueRule <- DueRuleOff)
DueRule == TRUE
DueRuleOff == FALSE

\*   TrackQuiet: the transport's silence timeout is in the model: quiet
\*            counts the ticks since anything was last heard from the other
\*            side (an ack of any packet, or KeepAlive), and an update that
\*            begins with quiet at SilenceT ends the connection
\*            (SilenceEnd).  Off (FALSE) by default.
\*   SilenceT: the connection timeout T in ticks (3, 4 or 5 in the cases).
\*   SilenceAt: the quiet at which the transport ends the connection;
\*            SilenceT is the rule; SilenceEarly (T - 1) and SilenceLate
\*            (T + 1) are the reversed witnesses.
\*   GoodFrom: the tick the last bad interval ends: a copy sent from then
\*            on is acknowledged (the liveness fairness, GoodAck); past the
\*            horizon (never) by default.
\* (TrackQuiet <- TrackQuietOn, SilenceT <- ..., SilenceAt <- SilenceEarly or
\*  SilenceLate, GoodFrom <- GoodFromMid)
TrackQuiet == FALSE
SilenceT == 3
SilenceAt == SilenceT
GoodFrom == ChunkFirst + RefuseFor + Timeout + 1
QuietMax == SilenceAt
TrackQuietOn == TRUE
SilenceEarly == SilenceT - 1
SilenceLate == SilenceT + 1
SilenceFive == 5
GoodFromMid == 4

Min(a, b) == IF a < b THEN a ELSE b
Max(a, b) == IF a > b THEN a ELSE b

Records == {"s", "c", "s2"}

\* RN_SEND_NUMBER_NEVER_SENT and the due-now sentinel of
\* rn_window_ack_packet_failed, -(resend_delay_max + 1): below any tick
\* less every delay, so a record stamped DueNow is due at every tick.
DueNow == -(RtoMax * MaxBackoff + 1)
NeverSent == DueNow - 1
Other(r) == IF r = "s" THEN "c" ELSE "s"

\* The chunk delay's cap: MinAttempts sends fit before the timeout.
ChunkCap == Max(1, Timeout \div MinAttempts)

\* The validation rule of sdk/rocketnet_config.cpp
\* (rocketnet_message_validate_config_internal): a configured timeout is
\* 1000 ms or more and at least MinAttempts sends of the initial resend delay
\* fit before it; zero (off) is accepted.  TimeoutFloor is 1000 ms in ticks:
\* 2 at the 500 ms tick of most instances, TimeoutFloorFine (10) at the
\* 100 ms tick (TimeoutFloor <- TimeoutFloorFine).  At 500 ms the floor
\* never binds (MinAttempts sends of at least one tick are 4 ticks, 2 s); at
\* 100 ms it does, and MCRNResendTimingBelowTimeoutFloor.cfg is a config the
\* rule without the floor accepts and the code refuses.
TimeoutFloor == 2
TimeoutFloorFine == 10
ConfigAccepted == ~TimeoutOn \/ (Timeout >= TimeoutFloor /\ Timeout >= MinAttempts * RtoInitial)

\* The timeout's clock runs: configured, or the reversed witness.
Timed == TimeoutOn \/ TimeoutWhenOff

ASSUME Third => (Room = 0 /\ ~OneFifo)

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
    calls,      \* Room > 0: the rn_ordered_write calls of this update so far
    sinceProgress, \* ticks since a reliable number was newly acknowledged
                \* (ack->last_progress), saturating at Probe; Probe at the
                \* start, so U's first send alone times the first hold
    quiet,      \* TrackQuiet: ticks since the other side was last heard, saturating
    silenced    \* TrackQuiet: the transport ended the connection (TIMED_OUT)

vars == <<t, lastSend, rto, backoff, attempts, acked, head, phase, sampled, seeded, firstSend, refused, pending, calls,
          sinceProgress, quiet, silenced>>
probeVars == <<sinceProgress, quiet, silenced>>

\* A small record waits the resend delay alone; a chunk waits it capped,
\* when the timeout is configured (m9.3; rn_window_ack_chunk_resend_delay).
Cap(r) == IF r = "c" /\ Timed THEN ChunkCap ELSE RtoMax

Delay(r) == Min(rto, Cap(r)) * backoff[r]

\* FirstSend(r): the tick the record is first offered to the walk.
FirstSend(r) == IF r = "c" THEN ChunkFirst ELSE 0
OnWire(r) == lastSend[r] # NeverSent
Clock(r) == IF r = "c" /\ BlockClock THEN 0 ELSE firstSend[r]
Deadline(r) == Clock(r) + Timeout
Horizon == ChunkFirst + RefuseFor + Timeout
Started(r) == t >= FirstSend(r)

\* rn_window_ack_check_timeout: the oldest unacked record that has a first
\* send has stood its timeout; a record never sent has no clock.  Only
\* when the timeout is configured (or under the reversed witness).
TimedOut == Timed /\ \E r \in Records : ~acked[r] /\ OnWire(r) /\ t >= Deadline(r)

Due(r) == ~acked[r] /\ ~TimedOut /\ Started(r) /\ (attempts[r] = 0 \/ t >= lastSend[r] + Delay(r))

Live == ~TimedOut /\ ~silenced /\ \E r \in Records : ~acked[r]

\* The connection has ended, by either clock.
Ended == TimedOut \/ silenced

\* The transport's silence timeout is due: the update begins with SilenceAt
\* ticks in which nothing was heard.  Nothing but a packet heard in this
\* update (Ack, Sample, KeepAlive) or the end (SilenceEnd) may happen.
SilenceDue == TrackQuiet /\ quiet >= SilenceAt

\* U, the oldest unacknowledged number: the records are numbered s, c, s2.
U == IF ~acked["s"] THEN "s" ELSE IF ~acked["c"] THEN "c" ELSE "s2"

\* rn_window_ack_probing: the timeout off, U on the wire (has_oldest), and
\* Probe ticks since the later of the last progress and U's first send.
Probing == ~TimeoutOn /\ Live /\ OnWire(U)
           /\ sinceProgress >= Probe /\ t - firstSend[U] >= Probe
\* The hold the walk applies (rn_ordered_walk skips a number on the wire
\* that is not U while probing), and its reversed witnesses.
WalkProbing ==
    CASE ProbeMode = "off" -> FALSE
      [] ProbeMode = "norelease" -> ~TimeoutOn /\ Live /\ OnWire(U) /\ t - firstSend[U] >= Probe
      [] OTHER -> Probing
Held(r) == Probing /\ OnWire(r) /\ r # U
WalkHeld(r) == WalkProbing /\ OnWire(r) /\ (r # U \/ ProbeMode = "holdsu")

\* rn_endpoint_fifo_due for one class's FIFO (s and s2 small, c a chunk).
\* The head is a number never sent, else the oldest last send (a refused
\* resend's due-now stamp first), s before s2 at the same tick, their
\* numbers' order.  A head never sent is due; a head on the wire that the
\* walk holds stands for U when U is in this class (rn_window_ack_probe_slot)
\* and for nothing otherwise; the number it stands for is due past its
\* delay.  DueRule = FALSE reads the head alone.
ClassOf(r) == IF r = "c" THEN "c" ELSE "s"
ClassLive(k) == {r \in Records : ~acked[r] /\ ClassOf(r) = k /\ Started(r)}
Before(a, b) ==
    \/ ~OnWire(a) /\ OnWire(b)
    \/ /\ OnWire(a) = OnWire(b)
       /\ \/ lastSend[a] < lastSend[b]
          \/ lastSend[a] = lastSend[b] /\ a = "s"
FifoHead(k) == CHOOSE r \in ClassLive(k) : \A o \in ClassLive(k) \ {r} : Before(r, o)
FifoDue(k) ==
    /\ ClassLive(k) # {}
    /\ LET h == FifoHead(k)
           x == IF DueRule /\ OnWire(h) /\ WalkHeld(h) THEN U ELSE h
       IN ClassOf(x) = k /\ Due(x)
\* rn_endpoint_ordered_due: either class's FIFO.  A block's pending chunks
\* are numbered and never sent, so they lead the chunk FIFO (its never-sent
\* part, ahead of the sent mark) and its head is due at once
\* (rn_send_number_unsent_due).
ReportedDue ==
    \/ FifoDue("s")
    \/ FifoDue("c")
    \/ pending > 0 /\ ~TimedOut /\ Started("c")

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
    /\ sinceProgress \in 0..Probe
    /\ quiet \in 0..QuietMax
    /\ silenced \in BOOLEAN

Init ==
    /\ t = 0
    /\ lastSend = [r \in Records |-> IF FirstSend(r) = 0 /\ RefuseFor = 0 THEN 0 ELSE NeverSent]
    /\ firstSend = lastSend
    /\ rto = RtoInitial
    /\ backoff = [r \in Records |-> 1]
    /\ attempts = [r \in Records |-> IF FirstSend(r) = 0 /\ RefuseFor = 0 THEN 1 ELSE 0]
    /\ acked = [r \in Records |-> CASE r = "c" -> ~Chunk [] r = "s" -> ~Small [] OTHER -> ~Third]
    /\ head \in (IF Chunk THEN {"s", "c"} ELSE {"s"})
    /\ sinceProgress = Probe
    /\ quiet = 0
    /\ silenced = FALSE
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
    ELSE Due(r) /\ ~WalkHeld(r)     \* one FIFO per class, each walked while due,
                                    \* past the numbers the probe holds

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
    /\ ~SilenceDue
    /\ ~SeedOwed
    /\ lastSend' = [r \in Records |-> IF Sent(r) THEN t ELSE lastSend[r]]
    /\ attempts' = [r \in Records |-> IF Sent(r) THEN Min(attempts[r] + 1, MinAttempts) ELSE attempts[r]]
    /\ backoff' = [r \in Records |-> IF Sent(r) THEN Min(backoff[r] * 2, MaxBackoff) ELSE backoff[r]]
    /\ firstSend' = [r \in Records |-> IF Sent(r) /\ ~OnWire(r) THEN t ELSE firstSend[r]]
    /\ head' = NextHead
    /\ refused' = [r \in Records |-> FALSE]
    /\ phase' = "tick"
    /\ UNCHANGED <<t, rto, acked, sampled, seeded, pending, calls, probeVars>>

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
    /\ ~SilenceDue
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
    /\ UNCHANGED <<t, rto, acked, sampled, seeded, pending, calls, probeVars>>

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
\* RefusedOffWire: a refused resend (DueNow) walked with the never-sent.
OffWire(r) == ~OnWire(r) \/ (RefusedOffWire /\ lastSend[r] = DueNow)
\* A number the probe holds is skipped by the walk (WalkHeld): it is not
\* planned, and the walk goes on past it.
SentDue(r) == Due(r) /\ ~OffWire(r) /\ ~WalkHeld(r)
UnsentDue(r) == Due(r) /\ OffWire(r)
Walkable(r) == SentDue(r) \/ UnsentDue(r)
PendingDue == pending > 0 /\ ~TimedOut /\ Started("c")

Planned ==
    IF UnsentFirst
    THEN { x \in {"s"} : Walkable("s") } \cup
         { x \in {"c"} : UnsentDue("c") \/ (SentDue("c") /\ ~PendingDue) } \cup
         { x \in {"u"} : PendingDue /\ ~UnsentDue("c") }
    ELSE { x \in {"s"} : SentDue("s") \/ (UnsentDue("s") /\ ~SentDue("c")) } \cup
         { x \in {"c"} : Walkable("c") } \cup
         { x \in {"u"} : PendingDue /\ ~Walkable("c") }

\* The builder writes one packet of the plan, in the budget's class order:
\* a retransmit (a record on the wire) before ordered progress; within a
\* class the scheduler's levels and channels pick, so either may go.  A
\* chunk travels alone, and the small record beside a chunk takes its own
\* packet, so the call writes one item.
Retransmit(x) == x # "u" /\ ~OffWire(x)
Picks == IF \E x \in Planned : Retransmit(x)
         THEN {x \in Planned : Retransmit(x)}
         ELSE Planned

WriteOne(x) ==
    /\ Room > 0
    /\ phase = "walk"
    /\ Live
    /\ ~SilenceDue
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
    /\ UNCHANGED <<t, rto, acked, head, phase, sampled, seeded, refused, probeVars>>

\* rn_window_ack_packet_failed for a call whose packet carries x, a record
\* on the wire: the transport refuses it.  x keeps its first send (its
\* clock) and its attempts, takes the due-now sentinel, and goes back on
\* the wire as its class's mark (rn_resend_fifo_push_mark); the call is
\* spent.
WriteRefused(x) ==
    /\ Room > 0
    /\ RefuseResends
    /\ phase = "walk"
    /\ Live
    /\ ~SilenceDue
    /\ ~SeedOwed
    /\ calls < Room
    /\ x \in Picks
    /\ Retransmit(x)
    /\ lastSend' = [lastSend EXCEPT ![x] = DueNow]
    /\ refused' = [refused EXCEPT ![x] = TRUE]
    /\ calls' = calls + 1
    /\ UNCHANGED <<t, rto, backoff, attempts, acked, head, phase, sampled, seeded, firstSend, pending, probeVars>>

\* The update's writes end: its Room calls are spent, or a call finds
\* nothing to plan (rn_ordered_write returns 0 and the endpoint stops).
\* The update's refusals stand until Tick.
EndWrites ==
    /\ Room > 0
    /\ phase = "walk"
    /\ Live
    /\ ~SilenceDue
    /\ ~SeedOwed
    /\ calls = Room \/ Planned = {}
    /\ calls' = 0
    /\ phase' = "tick"
    /\ UNCHANGED <<t, lastSend, rto, backoff, attempts, acked, head, sampled, seeded, firstSend, refused, pending, probeVars>>

\* Anything acknowledged, or any other packet that arrives, is the other
\* side heard: the transport's last receive time moves (quiet).
Heard == quiet' = 0

\* Any packet from the other side that acknowledges nothing here (a
\* keep-alive, a packet of another layer): heard, nothing else.
KeepAlive ==
    /\ TrackQuiet
    /\ phase = "walk"
    /\ calls = 0
    /\ Live
    /\ Heard
    /\ UNCHANGED <<t, lastSend, rto, backoff, attempts, acked, head, phase, sampled, seeded, firstSend, refused, pending, calls,
                   sinceProgress, silenced>>

\* The transport's silence timeout (TIMED_OUT on the server,
\* CONNECTION_TIMED_OUT on the client): an update that begins with nothing
\* heard for SilenceAt ticks ends the connection.
SilenceEnd ==
    /\ SilenceDue
    /\ phase = "walk"
    /\ calls = 0
    /\ Live
    /\ silenced' = TRUE
    /\ UNCHANGED <<t, lastSend, rto, backoff, attempts, acked, head, phase, sampled, seeded, firstSend, refused, pending, calls,
                   sinceProgress, quiet>>

\* The update's acknowledgements come before its timeout check: the message
\* update applies the received ids (rn_endpoint_on_acked_ids) before
\* rn_endpoint_tick runs rn_window_ack_check_timeout at the same time
\* (rocketnet_message_server.cpp), so an ack processed in the update the
\* deadline falls in keeps the connection.  TimedOut becomes true only on
\* the Tick into that update (Tick needs Live), so at phase "walk" with no
\* call yet, t = Deadline(r) is the moment before the check: Ack and Sample
\* are taken there whatever TimedOut says, and a state that stays TimedOut
\* is the end (Done).  CheckBeforeAcks is the reversed witness: the acks
\* gated by TimedOut, the check before them
\* (CheckBeforeAcks <- CheckBeforeAcksOn).
CheckBeforeAcks == FALSE
CheckBeforeAcksOn == TRUE
AcksOpen == ~silenced /\ (\E r \in Records : ~acked[r]) /\ (CheckBeforeAcks => ~TimedOut)

\* rn_window_ack_on_ack of another packet: one RTT sample moves rto to any
\* value of the range (the filter that may refuse it is RNRttFilter).
Sample(x) ==
    /\ phase = "walk"
    /\ calls = 0
    /\ AcksOpen
    /\ Heard
    /\ rto' = x
    /\ backoff' = [r \in Records |-> 1]
    /\ sampled' = TRUE
    /\ UNCHANGED <<t, lastSend, attempts, acked, head, phase, seeded, firstSend, refused, pending, calls,
                   sinceProgress, silenced>>

\* rn_window_ack_seed_rtt from the message object's update: the transport's
\* round trip taken as the first sample, after the update's acks and before
\* its walk.
Seed ==
    /\ phase = "walk"
    /\ calls = 0
    /\ Live
    /\ ~SilenceDue
    /\ SeedOwed
    /\ rto' = SeedRto
    /\ backoff' = [r \in Records |-> 1]
    /\ seeded' = TRUE
    /\ UNCHANGED <<t, lastSend, attempts, acked, head, phase, sampled, firstSend, refused, pending, calls, probeVars>>

\* rn_window_ack_on_ack of a packet carrying record r: a reliable number
\* newly acknowledged, the progress that resets the probe's clock
\* (ack->last_progress).  Its RTT sample is Sample's.
Ack(r) ==
    /\ phase = "walk"
    /\ calls = 0
    /\ ~acked[r]
    /\ AcksOpen
    /\ attempts[r] > 0
    /\ acked' = [acked EXCEPT ![r] = TRUE]
    /\ sinceProgress' = IF TimeoutOn THEN sinceProgress ELSE 0
    /\ Heard
    /\ UNCHANGED <<t, lastSend, rto, backoff, attempts, head, phase, sampled, seeded, firstSend, refused, pending, calls,
                   silenced>>

\* The update ends and time moves on one tick; its refusals are over.  Past
\* the horizon (only reached with the timeout off) time stands still.
Tick ==
    /\ phase = "tick"
    /\ Live
    /\ phase' = "walk"
    /\ refused' = [r \in Records |-> FALSE]
    /\ IF t < Horizon
       THEN /\ t' = t + 1
            /\ sinceProgress' = IF TimeoutOn THEN sinceProgress ELSE Min(sinceProgress + 1, Probe)
            /\ quiet' = IF TrackQuiet THEN Min(quiet + 1, QuietMax) ELSE quiet
       ELSE UNCHANGED <<t, sinceProgress, quiet>>
    /\ UNCHANGED <<lastSend, rto, backoff, attempts, acked, head, sampled, seeded, firstSend, pending, calls, silenced>>

\* Every record acked, or the connection ended: nothing more happens.
Done ==
    /\ ~Live
    /\ UNCHANGED vars

Next ==
    \/ Walk
    \/ \E F \in SUBSET Records : WalkRefused(F)
    \/ \E x \in {"s", "c", "u"} : WriteOne(x)
    \/ \E x \in Records : WriteRefused(x)
    \/ EndWrites
    \/ \E x \in RtoMin..RtoMax : Sample(x)
    \/ Seed
    \/ \E r \in Records : Ack(r)
    \/ KeepAlive
    \/ SilenceEnd
    \/ Tick
    \/ Done

Spec == Init /\ [][Next]_vars

\* The liveness instance: after the last bad interval (GoodFrom) a copy
\* sent is acknowledged.  Strong fairness on that acknowledgement only: a
\* copy sent inside the bad interval may be lost for ever.  Updates go on.
GoodAck(r) == Ack(r) /\ lastSend[r] >= GoodFrom
Fairness ==
    /\ WF_vars(Walk) /\ WF_vars(EndWrites) /\ WF_vars(Tick) /\ WF_vars(Seed)
    /\ \A x \in {"s", "c", "u"} : WF_vars(WriteOne(x))
    /\ \A r \in Records : SF_vars(GoodAck(r))
LiveSpec == Spec /\ Fairness

\* The property the rule exists for, for each class: checked with a packet
\* for each due record in every update (Room 0 or 2) and no refused resend,
\* with the timeout configured.
AttemptsBeforeTimeout ==
    \A r \in Records : (TimeoutOn /\ ~acked[r] /\ OnWire(r) /\ t = Deadline(r)) => attempts[r] >= MinAttempts

\* An ack that arrives in the update the deadline falls in is taken before
\* the timeout check (the order of the message update): at the start of that
\* update the record can still be acknowledged.  Reversed witness
\* MCRNResendTimingBrokenCheckBeforeAcks.cfg (the check first).
DeadlineUpdateTakesAcks ==
    \A r \in Records :
        (Timed /\ phase = "walk" /\ calls = 0 /\ ~silenced /\ ~acked[r] /\ attempts[r] > 0 /\ t = Deadline(r))
            => ENABLED Ack(r)

\* Under the rule no unacked record waits more than its delay's bound for
\* its next send while the connection is up: RtoMax for a small record,
\* Min(RtoMax, ChunkCap) for a chunk.  Checked as AttemptsBeforeTimeout is:
\* a packet for each due record in every update, no refused resend, the
\* timeout configured (with it off the probe may hold a number longer).
ResendGapBounded ==
    \A r \in Records :
        (TimeoutOn /\ ~acked[r] /\ ~TimedOut /\ attempts[r] > 0) => t - lastSend[r] <= Min(RtoMax, Cap(r)) * MaxBackoff

\* With the timeout off (m9.3: zero turns the reliable timeout off) every
\* end is the transport's silence end.  Where the silence end is in the
\* model (TrackQuiet) an end is reachable and this is checked over it;
\* without it, this is the gate of rn_window_ack_check_timeout
\* (reliable_timeout > 0.0) and the reversed witness TimeoutWhenOff.
OffMeansNoStallEnd == ~TimeoutOn => (Ended => silenced)

\* (S) No end before T, the default: every end the connection has, by
\* either clock, comes after SilenceT ticks with nothing heard from the
\* other side, so an outage shorter than T never ends it.  An end is
\* reachable (SilenceEnd), so the bound is checked over real ends: with
\* the bound replaced by FALSE the passing case fails.  Reversed
\* witnesses: TimeoutWhenOff (the reliable clock from the first send, the
\* code before v1.1.0) ends a connection heard inside T; SilenceEarly ends
\* it at T - 1.
NoEndBeforeT == (TrackQuiet /\ ~TimeoutOn) => (Ended => quiet >= SilenceT)

\* (K) The end at T: a live connection never has more than SilenceT ticks
\* with nothing heard, so an outage of T ends it at its first update at or
\* after T.  Reversed witness SilenceLate.
SilenceEndsAtT == TrackQuiet => (Live => quiet <= SilenceT)

\* Probe pacing (m9.3): while the path has acknowledged no reliable number
\* for Probe ticks, no number on the wire but U is resent.
ProbeOnlyU ==
    [][Probing => \A r \in Records : (OnWire(r) /\ r # U) => lastSend'[r] = lastSend[r]]_vars

\* A frame is started only for work, and never withheld from it: at each
\* point the endpoint asks rn_endpoint_next_packet_bytes, the due test
\* reports work exactly when the walk that follows writes a number.  For
\* Walk (Room = 0, one FIFO per class), the due test against the numbers
\* the walk sends; for each paced call (Room > 0), against the call's plan
\* (an empty plan is a frame started and aborted, EndWrites with calls
\* left).  Under probe pacing a head the walk holds is the case this
\* exists for: the due test blind to the hold (DueRule = FALSE) reports a
\* held chunk or s2 past its delay due in every update while U is not,
\* and the walk writes nothing.
DueMeansWritten ==
    /\ (Room = 0 /\ ~OneFifo /\ phase = "walk" /\ Live /\ ~SilenceDue /\ ~SeedOwed) =>
          (ReportedDue <=> \E r \in Records : Sent(r))
    /\ (Room > 0 /\ phase = "walk" /\ Live /\ ~SilenceDue /\ ~SeedOwed /\ calls < Room) =>
          (ReportedDue <=> Planned # {})

\* After the last bad interval every reliable number is acknowledged:
\* probe pacing never keeps a delivering path from progress.
EveryReliableAcked == <>(\A r \in Records : acked[r])

\* A due record is sent in the update it falls due: no update ends with a
\* record of either class still due, whatever the other class does, but
\* for one whose packet the transport refused.  Checked with a packet for
\* each due record in every update (Room 0 or 2); with one packet and two
\* due records it fails (MCRNResendTimingPacedSmallRoomOne.cfg), as the
\* code holds the second for the next update.  The exception is the code's
\* behaviour, not a loosening: rn_window_ack_packet_failed puts every
\* unacked number of the refused packet back and leaves it due at once (a
\* number never on the wire keeps RN_SEND_NUMBER_NEVER_SENT, so attempts =
\* 0; a resend gets a negative due-now sentinel, -(resend_delay_max + 2)
\* after rn_packet_builder_packet_failed), so after the walk or call in
\* which its packet was refused the record is due by construction and
\* waits for the next.  refused[r] is set only by WalkRefused and
\* WriteRefused and only for a record offered (Refusable implies Sent;
\* WriteRefused needs x in Picks), and every Tick clears it, so a record
\* that falls due and is not offered still fails this invariant.  No
\* sentinel value can remove the exception: the sentinel exists to make the
\* record due.  With the timeout off the second exception is a number the
\* probe holds by the rule (Held, m9.3: the due-resend rule excepts it);
\* the walk's own hold is not the exception, so a hold the rule has
\* released (ProbeMode "norelease") or one on U ("holdsu") fails here.
NoDueRecordHeld ==
    phase = "tick" => \A r \in Records : ~Due(r) \/ refused[r] \/ Held(r)

\* A resend the transport refused is planned as a retransmit by the next
\* call of the update, ahead of every number not on the wire: it is among
\* the items the builder may write first (Picks).  The next update's walk
\* plans it the same way, as a due number on the wire.
RetryPlanned ==
    (Room > 0 /\ phase = "walk" /\ Live /\ ~SeedOwed /\ calls < Room) =>
        \A r \in Records : (refused[r] /\ Due(r) /\ ~Held(r)) => r \in Picks

\* From the update the transport first reports a round trip until this
\* layer's own first sample, no unacked record waits longer than the
\* seeded delay for its next send: a record last sent before SeedAt is
\* resent by SeedAt + its delay, not by its last send + RtoInitial.
SeededGapBounded ==
    \A r \in Records :
        (~acked[r] /\ ~TimedOut /\ attempts[r] > 0 /\ ~sampled /\ t >= SeedAt) =>
            t - Max(lastSend[r], SeedAt) <= Min(SeedRto, Cap(r)) * MaxBackoff

=============================================================================
