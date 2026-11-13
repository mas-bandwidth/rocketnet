----------------------------- MODULE RNEndpoint -----------------------------
(***************************************************************************)
(* rocketnet: two message endpoints end to end over the transport, with    *)
(* the CONFIG announcement (docs/CHANNELS.md layer 7: sections 10.1, 10.3, *)
(* 13.4 and the RNEndpoint row of section 15).  Written before the code,   *)
(* which now exists: sdk/rocketnet_endpoint.cpp cites its actions          *)
(* (rn_endpoint_reset, rn_endpoint_on_payload, rn_endpoint_tick, ...),     *)
(* bound by citation, not by a trace walk.                                 *)
(*                                                                         *)
(* Two endpoints, "a" and "b", one per end of a connection, each sending   *)
(* to the other over its own direction of the path: two instances of Link  *)
(* (tla/Link.tla), AB carrying a's packets to b and BA carrying b's to a.  *)
(* Each endpoint's application sends messages 1..N in index order; a       *)
(* message is "ordered" (RELIABLE_ORDERED), "reliable" (RELIABLE) or       *)
(* "unreliable" (UNRELIABLE), on channel MsgChan[k].                       *)
(*                                                                         *)
(* The transport underneath is abstracted to its ack semantics             *)
(* (CHANNELS.md section 2):                                                *)
(*   P2  acked implies opened: a packet's reliable records are put in the  *)
(*       opening end's pending acks when the packet is opened, whatever    *)
(*       the message layer then does with them (an endpoint that refuses   *)
(*       the packet as a violation still acked it);                        *)
(*   P5  a missing ack proves nothing: burned ids (PacketFailed: written,  *)
(*       never sent) and ack history overflow (AckSilence: pending acks    *)
(*       dropped unsent) both leave silence, never a false delivery fact;  *)
(*   P4  acked ids reach the sender as a set per opened packet (OnAcked).  *)
(* An ack names records, not packet ids: an acked packet id maps to the    *)
(* records it carried, which is RNWindow's sent map, checked there.        *)
(* Duplicate packets are the transport's replay window's to drop           *)
(* (ReplayWindow), so the instances run Link with Duplication = FALSE.     *)
(*                                                                         *)
(* Numbering, acks and delivery are taken at the level RNWindow and        *)
(* RNChannelTypes check: a reliable record takes its number in queue       *)
(* order, is first written only while its number is below U + W (U the    *)
(* oldest unacked number), and may be resent at any time until acked (the  *)
(* RTO is abstract, as in RNWindow; its timing is RNResendTiming's).  The  *)
(* receiver keeps the received set (RNWindow's R and bitset), drops a      *)
(* duplicate, and holds an ordered message until its channel predecessors  *)
(* are received (RNChannelTypes).                                          *)
(*                                                                         *)
(* CONFIG (10.3, 13.4): record 0.  At Reset it is queued at the head of    *)
(* IMMEDIATE and takes the first reliable number, so the first packet an   *)
(* endpoint writes carries it and it is resent until acked.  On receipt    *)
(* it is compared field by field with the receiver's own configuration     *)
(* (ConfigCompare); a configuration here is its num_channels, the field    *)
(* the instances vary.  A mismatch is the error CONFIG_MISMATCH.  Data     *)
(* records that arrive before CONFIG are processed against the receiver's  *)
(* own bounds: a channel outside them is a VIOLATION, never an overrun.  A *)
(* remote endpoint whose CONFIG or acks never come is RELIABLE_TIMEOUT     *)
(* (Tick) when the reliable timeout is configured, and waits otherwise.  A *)
(* remote endpoint heard from by nothing for T updates is TIMED_OUT, the  *)
(* transport's silence end (Elapse).  An endpoint in error does nothing   *)
(* more: the object disconnects it.                                        *)
(*                                                                         *)
(* The reversed witnesses are constants, each the rule it names:           *)
(*   ConfigReliable = FALSE  CONFIG is sent once, unreliable: a lost first *)
(*                           packet hangs verification                     *)
(*                           (ConfigVerifiedDespiteFirstLoss fails);       *)
(*   ConfigCompared = FALSE  CONFIG is taken without the comparison: a     *)
(*                           mismatched remote endpoint is accepted        *)
(*                           (MismatchErrorsBothEnds fails);               *)
(*   OrderChecked = FALSE    an ordered message is released with no check  *)
(*                           of its channel predecessors                   *)
(*                           (OrderedInOrder fails).                       *)
(*                                                                         *)
(* Time (revision 1.1).  With Clocked = TRUE time passes in whole updates  *)
(* (Elapse), and three clocks run on it: each end's silence clock (quiet:  *)
(* updates since that end last heard any packet from the other side; the   *)
(* transport ends a connection silent for SilenceT updates, TIMED_OUT on   *)
(* the server and CONNECTION_TIMED_OUT on the client), and, only when the  *)
(* reliable timeout is configured, the CONFIG wait from its start (cwait)  *)
(* and each reliable record's age since its first send (fs), the oldest    *)
(* unacked record's age being the reliable clock (ReliableT updates).      *)
(* With Clocked = FALSE the model is untimed as before revision 1.1: no    *)
(* Elapse, the clocks stay at their initial values, and Tick fires         *)
(* whenever its guard holds (a model-size knob for the cases that check    *)
(* no timing property; an end in error is inert, so the silence end adds  *)
(* no state those cases' invariants read).                                *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    N,              \* messages each application sends, 1..N
    MsgRel,         \* [1..N -> {"ordered", "reliable", "unreliable"}]
    MsgChan,        \* [1..N -> Nat], the channel of each message
    W,              \* the reliable window, numbers in flight
    CfgPairs,       \* the configurations: a set of [a |-> num_channels, b |-> num_channels]
    ConfigReliable, \* BOOLEAN: CONFIG takes the first reliable number (10.3)
    ConfigCompared, \* BOOLEAN: the receiver compares CONFIG with its own (13.4)
    OrderChecked,   \* BOOLEAN: an ordered message waits for its channel predecessors (7.2)
    TimeoutAnyTime, \* BOOLEAN: Tick may time out at any time, not only on a silent remote endpoint
    TimeoutConfigured, \* BOOLEAN: reliable_message_timeout_ms is nonzero (m9.3); zero, the
                    \* default, gives neither half of Tick a clock
    ConfigWaitOnTimeout, \* BOOLEAN, a reversed witness: the CONFIG wait keeps its clock
                    \* with the timeout off, where a zero timeout fires at once
    Clocked,        \* BOOLEAN: time passes (Elapse) and the clocks run
    SilenceT,       \* the connection timeout T, in updates (>= 1)
    ReliableT,      \* the reliable timeout, in updates, when it is configured (>= 1)
    SilenceEndEarly, \* BOOLEAN, a reversed witness: the silence end fires one update
                    \* before T (the comparison one short)
    Silence,        \* BOOLEAN: ack history overflow may drop pending acks (P5)
    Failures,       \* BOOLEAN: a written packet may fail to send (PacketFailed, P5)
    WriteAll,       \* BOOLEAN: a packet carries every due record (a model-size knob;
                    \* TRUE on an in-order link leaves ordering unexercised)
    RtoAboveRtt,    \* BOOLEAN: a record is resent only once its copy and its ack are gone
    ABPattern,      \* Link loss pattern, a to b
    BAPattern,      \* Link loss pattern, b to a
    Capacity, MaxDelay, Duplication, Reordering, BurstStart, BurstLen

E      == {"a", "b"}
Remote(e) == IF e = "a" THEN "b" ELSE "a"
Msgs   == 1..N
CONFIG == 0
Recs   == 0..N          \* record ids: CONFIG and the messages
NoCfg  == 0             \* remoteCfg before a CONFIG is taken
Errors == {"NONE", "CONFIG_MISMATCH", "VIOLATION", "RELIABLE_TIMEOUT", "TIMED_OUT"}
NotSent == -1           \* fs before a record's first send

ASSUME /\ N \in Nat \ {0}
       /\ MsgRel \in [Msgs -> {"ordered", "reliable", "unreliable"}]
       /\ MsgChan \in [Msgs -> Nat]
       /\ W \in Nat \ {0}
       /\ CfgPairs \subseteq [E -> Nat \ {0}] /\ CfgPairs # {}
       /\ ConfigReliable \in BOOLEAN /\ ConfigCompared \in BOOLEAN
       /\ OrderChecked \in BOOLEAN
       /\ TimeoutAnyTime \in BOOLEAN /\ Silence \in BOOLEAN /\ Failures \in BOOLEAN
       /\ TimeoutConfigured \in BOOLEAN /\ ConfigWaitOnTimeout \in BOOLEAN
       /\ WriteAll \in BOOLEAN /\ RtoAboveRtt \in BOOLEAN
       /\ Clocked \in BOOLEAN /\ SilenceEndEarly \in BOOLEAN
       /\ SilenceT \in Nat \ {0} /\ ReliableT \in Nat \ {0}

\* a packet: reliable records, unreliable records, and the transport acks
\* it carries back (the records of the remote endpoint's packets this end opened)
Pkts == [rel : SUBSET Recs, unrel : SUBSET Recs, acks : SUBSET Recs]

VARIABLES
    cfg,          \* e -> num_channels, fixed at Init
    \* the sender, per endpoint
    nextMsg,      \* the next message index the application sends
    relQ,         \* the reliable records in number order (CONFIG first)
    acked,        \* the reliable records acked
    unrelQ,       \* unreliable records waiting for their one write
    firstWritten, \* the endpoint has written its first packet
    \* the transport under each endpoint
    pendAck,      \* records of opened remote packets, to ack
    \* the receiver, per endpoint
    remoteCfg,      \* the remote endpoint's configuration once its CONFIG is taken
    rcvd,         \* the reliable messages received
    held,         \* ordered messages waiting for a predecessor
    deliv,        \* the messages delivered, in delivery order
    err,          \* the error state (rn_endpoint_error)
    \* ghost
    opened,       \* every reliable record in every packet this end opened
    \* the clocks (Clocked)
    quiet,        \* e -> updates since e last heard a packet from the other side
    cwait,        \* e -> updates of the CONFIG wait (reliable timeout configured)
    fs,           \* e -> record -> updates since its first send, NotSent before it
    \* the links
    abWire, abSent, abPulse,
    baWire, baSent, baPulse

abVars == <<abWire, abSent, abPulse>>
baVars == <<baWire, baSent, baPulse>>
epVars == <<cfg, nextMsg, relQ, acked, unrelQ, firstWritten, pendAck,
            remoteCfg, rcvd, held, deliv, err, opened>>
clockVars == <<quiet, cwait, fs>>
vars   == <<epVars, clockVars, abVars, baVars>>

\* the reliable clocks run: time passes and the timeout is configured
RClock == Clocked /\ TimeoutConfigured

AB == INSTANCE Link WITH wire <- abWire, sent <- abSent, pulse <- abPulse,
                         Packets <- Pkts, Pattern <- ABPattern
BA == INSTANCE Link WITH wire <- baWire, sent <- baSent, pulse <- baPulse,
                         Packets <- Pkts, Pattern <- BAPattern

\* e hands p to its link; the packet into e is delivered
LinkSend(e, p) ==
    IF e = "a" THEN AB!Send(p) /\ UNCHANGED baVars ELSE BA!Send(p) /\ UNCHANGED abVars
LinkDeliver(e, p) ==
    IF e = "b" THEN AB!Deliver(p) /\ UNCHANGED baVars ELSE BA!Deliver(p) /\ UNCHANGED abVars
WireInto(e) == IF e = "b" THEN abWire ELSE baWire

(***************************************************************************)
(* Derived state.                                                          *)
(***************************************************************************)
Min(S) == CHOOSE m \in S : \A x \in S : m <= x

RECURSIVE SortSeq(_)
SortSeq(S) == IF S = {} THEN <<>> ELSE <<Min(S)>> \o SortSeq(S \ {Min(S)})

Range(s) == {s[i] : i \in DOMAIN s}

Live(e) == err[e] = "NONE"

\* the position of the oldest unacked reliable record (U), or past the end
UPos(e) ==
    LET un == {i \in DOMAIN relQ[e] : relQ[e][i] \notin acked[e]}
    IN  IF un = {} THEN Len(relQ[e]) + 1 ELSE Min(un)

Unacked(e)  == Range(relQ[e]) \ acked[e]
Oldest(e)   == relQ[e][UPos(e)]
\* unacked records whose number is inside the window U .. U + W - 1 (6.2)
Writable(e) == {relQ[e][i] : i \in {j \in DOMAIN relQ[e] :
                    j < UPos(e) + W /\ relQ[e][j] \notin acked[e]}}

\* RtoAboveRtt: the abstract RTO, bounded below by the RTT.  A record is
\* due again only once no copy of it is in flight out of e and no ack of
\* it is pending at the remote endpoint or in flight back: the resend answers a real
\* loss.  An ASSUMPTION of the liveness cases, proved by no model: the code
\* resends on the resend delay alone (RNResendTiming lets a sample set it
\* anywhere in its range and relates it to no round trip), so it can resend
\* while a copy or its ack is in flight.  A composed bounded-network proof
\* is a roadmap entry (README-RNEndpoint.md).
WireOutOf(e)  == IF e = "a" THEN abWire ELSE baWire
InFlight(e)   == UNION {WireOutOf(e)[i].pkt.rel : i \in DOMAIN WireOutOf(e)}
AckInFlight(e) ==
    pendAck[Remote(e)] \cup UNION {WireOutOf(Remote(e))[i].pkt.acks : i \in DOMAIN WireOutOf(Remote(e))}
Due(e) == IF RtoAboveRtt THEN Writable(e) \ (InFlight(e) \cup AckInFlight(e))
                         ELSE Writable(e)

\* the ordered messages that must be delivered before k (7.2, per channel)
Preds(k) ==
    IF MsgRel[k] = "ordered"
      THEN {j \in Msgs : j < k /\ MsgRel[j] = "ordered" /\ MsgChan[j] = MsgChan[k]}
      ELSE {}

\* the messages e's application handed to Send and Send accepted
Accepted(e) == {k \in Msgs : k < nextMsg[e] /\ MsgChan[k] < cfg[e]}

(***************************************************************************)
(* Reset (rn_endpoint_reset): a new connection on both endpoints.  CONFIG  *)
(* is queued at the head of IMMEDIATE with the first reliable number.      *)
(***************************************************************************)
Reset ==
    /\ cfg \in CfgPairs
    /\ nextMsg = [e \in E |-> 1]
    /\ relQ = [e \in E |-> IF ConfigReliable THEN <<CONFIG>> ELSE <<>>]
    /\ acked = [e \in E |-> {}]
    /\ unrelQ = [e \in E |-> IF ConfigReliable THEN {} ELSE {CONFIG}]
    /\ firstWritten = [e \in E |-> FALSE]
    /\ pendAck = [e \in E |-> {}]
    /\ remoteCfg = [e \in E |-> NoCfg]
    /\ rcvd = [e \in E |-> {}]
    /\ held = [e \in E |-> {}]
    /\ deliv = [e \in E |-> <<>>]
    /\ err = [e \in E |-> "NONE"]
    /\ opened = [e \in E |-> {}]
    /\ quiet = [e \in E |-> 0]
    /\ cwait = [e \in E |-> 0]
    /\ fs = [e \in E |-> [r \in Recs |-> NotSent]]
    /\ AB!Init /\ BA!Init

Init == Reset

(***************************************************************************)
(* Send (rn_endpoint_send): the application sends its next message.  A     *)
(* channel outside the sender's own num_channels is refused (returns 0).   *)
(* A reliable message takes the next reliable number; an unreliable one    *)
(* waits for its single write.                                             *)
(***************************************************************************)
Send(e) ==
    /\ Live(e)
    /\ nextMsg[e] <= N
    /\ LET k == nextMsg[e] IN
       /\ nextMsg' = [nextMsg EXCEPT ![e] = k + 1]
       /\ IF MsgChan[k] >= cfg[e]
            THEN UNCHANGED <<relQ, unrelQ>>
            ELSE IF MsgRel[k] = "unreliable"
                   THEN /\ unrelQ' = [unrelQ EXCEPT ![e] = @ \cup {k}]
                        /\ UNCHANGED relQ
                   ELSE /\ relQ' = [relQ EXCEPT ![e] = Append(@, k)]
                        /\ UNCHANGED unrelQ
    /\ UNCHANGED <<cfg, acked, firstWritten, pendAck, remoteCfg, rcvd, held, deliv, clockVars,
                   err, opened, abVars, baVars>>

(***************************************************************************)
(* WritePacket (rn_endpoint_write_packet): a packet carrying reliable      *)
(* records inside the window (first sends and resends alike) and some      *)
(* waiting unreliable records, plus the transport's pending acks.  While   *)
(* the endpoint has written nothing, CONFIG is at the head of IMMEDIATE    *)
(* and the packet carries it.  An unreliable record is written once.       *)
(***************************************************************************)
WriteOk(e, S, Us) ==
    /\ Live(e)
    /\ S \subseteq Due(e) /\ Us \subseteq unrelQ[e]
    /\ S \cup Us # {}
    /\ ~firstWritten[e] => CONFIG \in S \cup Us
    /\ WriteAll => (S = Due(e) /\ Us = unrelQ[e])

WritePacket(e, S, Us) ==
    /\ WriteOk(e, S, Us)
    /\ LinkSend(e, [rel |-> S, unrel |-> Us, acks |-> pendAck[e]])
    /\ unrelQ' = [unrelQ EXCEPT ![e] = @ \ Us]
    /\ firstWritten' = [firstWritten EXCEPT ![e] = TRUE]
    /\ pendAck' = [pendAck EXCEPT ![e] = {}]
    \* a record's reliable clock starts at its first send (rn_window_ack_commit
    \* records first_send only for a number never sent before)
    /\ fs' = IF RClock
               THEN [fs EXCEPT ![e] = [r \in Recs |->
                        IF r \in S /\ @[r] = NotSent THEN 0 ELSE @[r]]]
               ELSE fs
    /\ UNCHANGED <<cfg, nextMsg, relQ, acked, remoteCfg, rcvd, held, deliv, err, opened,
                   quiet, cwait>>

\* PacketFailed (rn_endpoint_packet_failed): the packet was written and the
\* transport never sent it (a burned id).  Its reliable records stay unacked
\* and due; its unreliable records are gone.  The transport keeps its acks.
\* A record never on the wire stays never sent: no reliable clock starts
\* (rn_window_ack_packet_failed).
PacketFailed(e, S, Us) ==
    /\ Failures
    /\ WriteOk(e, S, Us)
    /\ unrelQ' = [unrelQ EXCEPT ![e] = @ \ Us]
    /\ firstWritten' = [firstWritten EXCEPT ![e] = TRUE]
    /\ UNCHANGED <<cfg, nextMsg, relQ, acked, pendAck, remoteCfg, rcvd, held, deliv, clockVars,
                   err, opened, abVars, baVars>>

\* The transport sends its acks with no message records (a keepalive).
WriteAck(e) ==
    /\ Live(e)
    /\ pendAck[e] # {}
    /\ LinkSend(e, [rel |-> {}, unrel |-> {}, acks |-> pendAck[e]])
    /\ pendAck' = [pendAck EXCEPT ![e] = {}]
    /\ UNCHANGED <<cfg, nextMsg, relQ, acked, unrelQ, firstWritten, remoteCfg, rcvd, clockVars,
                   held, deliv, err, opened>>

\* Ack history overflow: pending acks are lost unsent (P5, silence).
AckSilence(e) ==
    /\ Silence
    /\ pendAck[e] # {}
    /\ pendAck' = [pendAck EXCEPT ![e] = {}]
    /\ UNCHANGED <<cfg, nextMsg, relQ, acked, unrelQ, firstWritten, remoteCfg, rcvd, clockVars,
                   held, deliv, err, opened, abVars, baVars>>

(***************************************************************************)
(* OnAcked (rn_endpoint_on_acked): the records of e's own packets the      *)
(* remote endpoint opened are acked.                                       *)
(*                                                                         *)
(* ConfigCompare: TRUE when the remote CONFIG differs from the receiver's  *)
(* own configuration (13.4: every field compared).  ConfigCompared = FALSE *)
(* is the reversed witness that takes CONFIG without looking.              *)
(***************************************************************************)
OnAcked(e, as) == acked[e] \cup (as \cap Range(relQ[e]))

ConfigCompare(e) == ConfigCompared /\ cfg[Remote(e)] # cfg[e]

(***************************************************************************)
(* OnPayload (rn_endpoint_on_payload): e opens a packet from the remote    *)
(* endpoint.                                                               *)
(* The transport acks its reliable records whatever follows (P2), and its  *)
(* acks reach e (OnAcked).  The records are first checked against e's own *)
(* num_channels as a whole: one outside refuses the whole payload, a       *)
(* VIOLATION, and nothing of it is taken (R6; the reader's check).         *)
(* CONFIG, first in the packet, is then compared; a mismatch is the error  *)
(* and parsing stops.  Otherwise a fresh reliable message is received once, an *)
(* ordered one is held until its predecessors are received and then       *)
(* delivered with every hold it releases, and an unreliable one is         *)
(* delivered now.  An endpoint in error opens and discards.                *)
(*                                                                         *)
(* Any packet the transport accepts restarts e's silence clock: the        *)
(* client's and the server's last_receive_time = current_time on every     *)
(* accepted packet (sdk/rocketnet_client_protocol.cpp,                     *)
(* sdk/rocketnet_server_protocol.cpp).  An end in error keeps its clocks   *)
(* as they were at its end, so an end's quiet is the silence it ended at.  *)
(***************************************************************************)
OnPayload(e, p) ==
    /\ LinkDeliver(e, p)
    /\ IF ~Live(e)
         THEN UNCHANGED <<epVars, clockVars>>
         ELSE
         LET hasCfg  == CONFIG \in (p.rel \cup p.unrel)
             cfgNew  == hasCfg /\ remoteCfg[e] = NoCfg
             data    == (p.rel \cup p.unrel) \ {CONFIG}
             bad     == {k \in data : MsgChan[k] >= cfg[e]}
             fresh   == (p.rel \ {CONFIG}) \ rcvd[e]
             rcvd1   == rcvd[e] \cup fresh
             cand    == held[e] \cup fresh
             release == IF OrderChecked
                          THEN {k \in cand : Preds(k) \subseteq rcvd1}
                          ELSE cand
             unrelNow == p.unrel \ {CONFIG}
         IN
         /\ pendAck' = [pendAck EXCEPT ![e] = @ \cup p.rel]
         /\ opened' = [opened EXCEPT ![e] = @ \cup p.rel]
         /\ acked' = [acked EXCEPT ![e] = OnAcked(e, p.acks)]
         /\ quiet' = IF Clocked THEN [quiet EXCEPT ![e] = 0] ELSE quiet
         \* an acked record's clock is read no more (it is not the oldest
         \* unacked): back to NotSent, so it adds no state
         /\ fs' = IF RClock
                    THEN [fs EXCEPT ![e] = [r \in Recs |->
                             IF r \in OnAcked(e, p.acks) THEN NotSent ELSE @[r]]]
                    ELSE fs
         /\ UNCHANGED cwait
         \* the channel check is the reader's whole-payload check
         \* (rocketnet_message_check_payload, check 5, in
         \* rocketnet_message_reader_begin): a record outside e's own
         \* num_channels refuses the whole payload before any record is handed
         \* out, so a refused packet's CONFIG is not taken or compared and
         \* none of its data is received (revision 1.2: before it the model
         \* compared CONFIG first and took it from a refused packet)
         /\ IF bad # {}
              THEN /\ err' = [err EXCEPT ![e] = "VIOLATION"]
                   /\ UNCHANGED <<remoteCfg, rcvd, held, deliv>>
              ELSE IF cfgNew /\ ConfigCompare(e)
              THEN /\ err' = [err EXCEPT ![e] = "CONFIG_MISMATCH"]
                   /\ UNCHANGED <<remoteCfg, rcvd, held, deliv>>
              ELSE /\ remoteCfg' = IF cfgNew THEN [remoteCfg EXCEPT ![e] = cfg[Remote(e)]]
                                           ELSE remoteCfg
                   /\ rcvd' = [rcvd EXCEPT ![e] = rcvd1]
                   /\ held' = [held EXCEPT ![e] = cand \ release]
                   /\ deliv' = [deliv EXCEPT ![e] =
                                  @ \o SortSeq(release) \o SortSeq(unrelNow)]
                   /\ UNCHANGED err
         /\ UNCHANGED <<cfg, nextMsg, relQ, unrelQ, firstWritten>>

(***************************************************************************)
(* Tick (rn_endpoint_tick): the reliable timeout.  An endpoint still       *)
(* waiting for the remote endpoint's CONFIG, or with a reliable record     *)
(* unacked, past reliable_timeout enters RELIABLE_TIMEOUT.  The RTO due    *)
(* marks are abstract (any unacked record may be resent at any time), so   *)
(* Tick here is the timeout alone.  With TimeoutAnyTime the timeout may    *)
(* fire at any moment (safety holds whenever it fires); without it, only   *)
(* once the remote end is in error and silent.  That a live remote end's   *)
(* resends land before the timeout is an ASSUMPTION of the liveness cases, *)
(* proved by no model: RNResendTiming's AttemptsBeforeTimeout counts sends *)
(* before the deadline, not deliveries, and lets every send be lost.       *)
(*                                                                         *)
(* The CONFIG wait starts at Reset here, for both ends.  The code starts   *)
(* each end's at the first update its connection is connected at          *)
(* (rn_endpoint_config_tick: the server's Observe reset, the client's      *)
(* first update its transport reports CONNECTED), which can be later than *)
(* the other end's.  A later start only delays that end's CONFIG timeout,  *)
(* and Tick in Spec may fire at any update once the wait is due, so the    *)
(* code's later end is among the model's behaviours; the model also has   *)
(* the earlier one.                                                        *)
(*                                                                         *)
(* Both halves run only when the timeout is configured (MESSAGES.md m9.3   *)
(* and m11 rule 4): with it off (zero, the default) the CONFIG wait and an *)
(* unacked record have no clock, and only the transport's silence timeout  *)
(* ends a silent connection.  ConfigWaitOnTimeout is the code before       *)
(* v1.1.0: the CONFIG half compared the wait with a timeout of zero and    *)
(* fired at the first update.                                              *)
(*                                                                         *)
(* Clocked, the two halves are the code's two comparisons with            *)
(* reliable_timeout: now - config_wait_start (cwait, from Reset) for the   *)
(* CONFIG half (rn_endpoint_config_tick), and now - oldest_first_send for  *)
(* the record at U, the oldest unacked number, once it has been sent       *)
(* (rn_window_ack_check_timeout, rn_window_ack_refresh_oldest: no clock    *)
(* before the first send).  The broken CONFIG half compares with zero, and *)
(* every wait is at least zero, so it needs no clock value.                *)
(***************************************************************************)
ReliableDue(e) ==
    \/ remoteCfg[e] = NoCfg /\ (~Clocked \/ cwait[e] >= ReliableT)
    \/ Unacked(e) # {} /\ (~Clocked \/ fs[e][Oldest(e)] >= ReliableT)

Tick(e) ==
    /\ Live(e)
    /\ \/ TimeoutConfigured /\ ReliableDue(e)
       \/ ConfigWaitOnTimeout /\ ~TimeoutConfigured /\ remoteCfg[e] = NoCfg
    /\ TimeoutAnyTime \/ ~Live(Remote(e))
    /\ err' = [err EXCEPT ![e] = "RELIABLE_TIMEOUT"]
    /\ UNCHANGED <<cfg, nextMsg, relQ, acked, unrelQ, firstWritten, pendAck, remoteCfg, clockVars,
                   rcvd, held, deliv, opened, abVars, baVars>>

(***************************************************************************)
(* Elapse: one update passes (Clocked only).  Every live end's silence     *)
(* clock counts it, and the transport ends each live end that has now     *)
(* heard nothing for SilenceT updates, at this update: the server's        *)
(* current_time >= last_receive_time + timeout_seconds                     *)
(* (rocketnet_server_protocol_update, TIMED_OUT) and the client's          *)
(* current_time - last_receive_time >= timeout_seconds                     *)
(* (CONNECTION_TIMED_OUT), both checked every update.  With the reliable   *)
(* timeout configured the CONFIG wait and every sent, unacked record's     *)
(* age count it too, up to ReliableT (past which only >= ReliableT is      *)
(* read).  SilenceEndEarly is the reversed witness: the comparison one     *)
(* update short.  An end in error keeps its clocks.  The links run with   *)
(* MaxDelay = 0 here, so no copy's due counts down.                        *)
(***************************************************************************)
SilenceEnd == IF SilenceEndEarly THEN SilenceT - 1 ELSE SilenceT

Elapse ==
    /\ Clocked
    /\ \E e \in E : Live(e)
    /\ quiet' = [e \in E |-> IF Live(e) THEN quiet[e] + 1 ELSE quiet[e]]
    /\ err' = [e \in E |-> IF Live(e) /\ quiet[e] + 1 >= SilenceEnd
                             THEN "TIMED_OUT" ELSE err[e]]
    /\ cwait' = IF RClock
                  THEN [e \in E |-> IF Live(e) /\ remoteCfg[e] = NoCfg /\ cwait[e] < ReliableT
                                      THEN cwait[e] + 1 ELSE cwait[e]]
                  ELSE cwait
    /\ fs' = IF RClock
               THEN [e \in E |-> [r \in Recs |->
                       IF Live(e) /\ fs[e][r] # NotSent /\ fs[e][r] < ReliableT
                         THEN fs[e][r] + 1 ELSE fs[e][r]]]
               ELSE fs
    /\ UNCHANGED <<cfg, nextMsg, relQ, acked, unrelQ, firstWritten, pendAck, remoteCfg,
                   rcvd, held, deliv, opened, abVars, baVars>>

(***************************************************************************)
(* The outside events: the two paths.                                      *)
(***************************************************************************)
ABEnvironment == AB!Environment /\ UNCHANGED <<epVars, clockVars, baVars>>
BAEnvironment == BA!Environment /\ UNCHANGED <<epVars, clockVars, abVars>>

WriteAny(e)  == \E S \in SUBSET Due(e), Us \in SUBSET unrelQ[e] :
                    WritePacket(e, S, Us) \/ PacketFailed(e, S, Us)
\* Link delivers only the head of its wire, so the packet opened is that
\* head: the same steps as \E p \in Pkts, without enumerating Pkts.
OpenAny(e)   == WireInto(e) # <<>> /\ OnPayload(e, Head(WireInto(e)).pkt)

Next ==
    \/ \E e \in E :
         \/ Send(e)
         \/ WriteAny(e)
         \/ WriteAck(e)
         \/ AckSilence(e)
         \/ OpenAny(e)
         \/ Tick(e)
    \/ Elapse
    \/ ABEnvironment
    \/ BAEnvironment

(***************************************************************************)
(* Fairness, for liveness only.  The application sends; the oldest unacked *)
(* reliable record is written again while it can be (the resend FIFO's    *)
(* guarantee, as in RNWindow); a waiting unreliable record is written;     *)
(* acks go out; the head of each wire is opened; a due timeout fires.      *)
(* Strong where a lost send or a full link disables the step for a while.  *)
(***************************************************************************)
WriteOldest(e) ==
    /\ Unacked(e) # {}
    /\ \E S \in SUBSET Due(e), Us \in SUBSET unrelQ[e] :
         Oldest(e) \in S /\ WritePacket(e, S, Us)
WriteUnrel(e) ==
    \E S \in SUBSET Due(e), Us \in SUBSET unrelQ[e] :
         Us # {} /\ WritePacket(e, S, Us)
OpenHead(e) == WireInto(e) # <<>> /\ OnPayload(e, Head(WireInto(e)).pkt)

Fairness ==
    \A e \in E :
        /\ WF_vars(Send(e))
        /\ SF_vars(WriteOldest(e))
        /\ SF_vars(WriteUnrel(e))
        /\ SF_vars(WriteAck(e))
        /\ SF_vars(OpenHead(e))
        /\ WF_vars(Tick(e))

Spec     == Init /\ [][Next]_vars
LiveSpec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* Safety.                                                                 *)
(***************************************************************************)
Count(s, k) == Cardinality({i \in DOMAIN s : s[i] = k})

TypeOK ==
    /\ cfg \in CfgPairs
    /\ nextMsg \in [E -> 1..N+1]
    /\ \A e \in E : Range(relQ[e]) \subseteq Recs /\ Len(relQ[e]) <= N + 1
    /\ acked \in [E -> SUBSET Recs]
    /\ unrelQ \in [E -> SUBSET Recs]
    /\ firstWritten \in [E -> BOOLEAN]
    /\ pendAck \in [E -> SUBSET Recs]
    /\ remoteCfg \in [E -> Nat]
    /\ rcvd \in [E -> SUBSET Msgs] /\ held \in [E -> SUBSET Msgs]
    /\ \A e \in E : Range(deliv[e]) \subseteq Msgs
    /\ err \in [E -> Errors]
    /\ opened \in [E -> SUBSET Recs]
    /\ quiet \in [E -> 0..SilenceT]
    /\ cwait \in [E -> 0..ReliableT]
    /\ fs \in [E -> [Recs -> {NotSent} \cup 0..ReliableT]]
    /\ AB!InFlightBounded /\ BA!InFlightBounded

\* CONFIG takes the first reliable number (10.3)
ConfigFirstReliable == ConfigReliable => \A e \in E : relQ[e][1] = CONFIG

\* the window: no reliable record past U + W - 1 is ever written (6.2)
WindowBounded == \A e \in E : Cardinality(Writable(e)) <= W

\* acked implies opened: a record e holds acked was in a packet the remote endpoint
\* opened (P2 carried end to end)
AckedImpliesOpened == \A e \in E : acked[e] \subseteq opened[Remote(e)]

\* a reliable message is delivered at most once, and only if the remote endpoint's
\* application sent it; with OnPayload's dedupe and the liveness below this
\* is exactly once
ReliableExactlyOnce ==
    \A e \in E : \A k \in Msgs :
        MsgRel[k] # "unreliable" =>
            /\ Count(deliv[e], k) <= 1
            /\ Count(deliv[e], k) = 1 => k \in Accepted(Remote(e))

\* an unreliable message is delivered at most once, and only if sent
UnreliableAtMostOnce ==
    \A e \in E : \A k \in Msgs :
        MsgRel[k] = "unreliable" =>
            /\ Count(deliv[e], k) <= 1
            /\ Count(deliv[e], k) = 1 => k \in Accepted(Remote(e))

\* an ordered message is delivered only after every ordered message before
\* it on its channel
OrderedInOrder ==
    \A e \in E : \A i \in DOMAIN deliv[e] :
        \A j \in Preds(deliv[e][i]) : \E h \in 1..i-1 : deliv[e][h] = j

\* An end that took the remote endpoint's CONFIG took an equal one, and an end in
\* CONFIG_MISMATCH saw a real difference: every end that opens a differing
\* CONFIG errors, so with differing configurations both ends that hear the
\* other error, and the liveness below says both do.
MismatchErrorsBothEnds ==
    \A e \in E :
        /\ remoteCfg[e] # NoCfg => remoteCfg[e] = cfg[e]
        /\ err[e] = "CONFIG_MISMATCH" => cfg[e] # cfg[Remote(e)]

\* data records before CONFIG are safe: with equal configurations a correct
\* remote endpoint never trips the receiver's bounds
NoViolationWhenEqual ==
    cfg["a"] = cfg["b"] => \A e \in E : err[e] # "VIOLATION"

\* The timing ends: the reliable timeout and the transport's silence end.
\* The other two ends are the other side breaking the protocol (CONFIG_MISMATCH,
\* VIOLATION; with equal configurations neither is reachable, as
\* MismatchErrorsBothEnds and NoViolationWhenEqual say).
TimingEnd(e) == err[e] \in {"RELIABLE_TIMEOUT", "TIMED_OUT"}

\* (S) No end shorter than T.  With the reliable timeout off (zero, the
\* default; MESSAGES.md m9.3, m11 rule 4, m1.9 rule 1) every timing end, of
\* either reason, comes only after the ending end has heard nothing from the
\* other side for T updates: an outage shorter than T ends nothing.  Checked
\* Clocked, where the silence end is reachable (replace quiet[e] >= SilenceT
\* by FALSE and MCRNEndpointTimeoutOff.cfg fails).  Reversed witnesses: the
\* CONFIG wait on a zero timeout (the code before v1.1.0) ends at quiet 0,
\* and a silence end one update early ends at T - 1.
EndsNeedSilenceT == \A e \in E : TimingEnd(e) => quiet[e] >= SilenceT
NoEndBeforeSilenceT == ~TimeoutConfigured => EndsNeedSilenceT

\* (K) The end at T: no live end has heard nothing for T updates; the update
\* that brings an end's silence to T ends it.  Checked Clocked; a silence end
\* one update late, or none, fails it.
SilenceEndsAtT == \A e \in E : Live(e) => quiet[e] < SilenceT

(***************************************************************************)
(* Liveness.                                                               *)
(***************************************************************************)
Equal == cfg["a"] = cfg["b"]

\* CONFIG is verified by both ends even with each link losing its first
\* packet ("first"): CONFIG is resent until acked
ConfigVerifiedDespiteFirstLoss ==
    Equal => <>(\A e \in E : remoteCfg[e] # NoCfg)

\* every reliable message the remote endpoint sent is delivered (no hang)
EveryReliableDelivered ==
    Equal => <>(\A e \in E :
                  {k \in Accepted(Remote(e)) : MsgRel[k] # "unreliable"}
                      \subseteq Range(deliv[e])
                  /\ nextMsg[Remote(e)] = N + 1)

\* with differing configurations both ends end in error (no hang)
MismatchBothEndsError ==
    ~Equal => <>(\A e \in E : ~Live(e))

=============================================================================
