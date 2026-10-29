----------------------------- MODULE RNEndpoint -----------------------------
(***************************************************************************)
(* rocketnet: two message endpoints end to end over the transport, with    *)
(* the CONFIG announcement (docs/CHANNELS.md layer 7: sections 10.1, 10.3, *)
(* 13.4 and the RNEndpoint row of section 15).  Written before the code:  *)
(* the model is the spec the CONFIG card and the endpoint card implement,  *)
(* and its action names are the planned function names of 10.1.           *)
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
(* (Tick).  An                                                             *)
(* endpoint in error does nothing more: the object disconnects it.         *)
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
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

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
Errors == {"NONE", "CONFIG_MISMATCH", "VIOLATION", "RELIABLE_TIMEOUT"}

ASSUME /\ N \in Nat \ {0}
       /\ MsgRel \in [Msgs -> {"ordered", "reliable", "unreliable"}]
       /\ MsgChan \in [Msgs -> Nat]
       /\ W \in Nat \ {0}
       /\ CfgPairs \subseteq [E -> Nat \ {0}] /\ CfgPairs # {}
       /\ ConfigReliable \in BOOLEAN /\ ConfigCompared \in BOOLEAN
       /\ OrderChecked \in BOOLEAN
       /\ TimeoutAnyTime \in BOOLEAN /\ Silence \in BOOLEAN /\ Failures \in BOOLEAN
       /\ WriteAll \in BOOLEAN /\ RtoAboveRtt \in BOOLEAN

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
    \* the links
    abWire, abSent, abPulse,
    baWire, baSent, baPulse

abVars == <<abWire, abSent, abPulse>>
baVars == <<baWire, baSent, baPulse>>
epVars == <<cfg, nextMsg, relQ, acked, unrelQ, firstWritten, pendAck,
            remoteCfg, rcvd, held, deliv, err, opened>>
vars   == <<epVars, abVars, baVars>>

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
\* loss.  RNResendTiming checks the timing that makes this so.
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
    /\ UNCHANGED <<cfg, acked, firstWritten, pendAck, remoteCfg, rcvd, held, deliv,
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
    /\ UNCHANGED <<cfg, nextMsg, relQ, acked, remoteCfg, rcvd, held, deliv, err, opened>>

\* PacketFailed (rn_endpoint_packet_failed): the packet was written and the
\* transport never sent it (a burned id).  Its reliable records stay unacked
\* and due; its unreliable records are gone.  The transport keeps its acks.
PacketFailed(e, S, Us) ==
    /\ Failures
    /\ WriteOk(e, S, Us)
    /\ unrelQ' = [unrelQ EXCEPT ![e] = @ \ Us]
    /\ firstWritten' = [firstWritten EXCEPT ![e] = TRUE]
    /\ UNCHANGED <<cfg, nextMsg, relQ, acked, pendAck, remoteCfg, rcvd, held, deliv,
                   err, opened, abVars, baVars>>

\* The transport sends its acks with no message records (a keepalive).
WriteAck(e) ==
    /\ Live(e)
    /\ pendAck[e] # {}
    /\ LinkSend(e, [rel |-> {}, unrel |-> {}, acks |-> pendAck[e]])
    /\ pendAck' = [pendAck EXCEPT ![e] = {}]
    /\ UNCHANGED <<cfg, nextMsg, relQ, acked, unrelQ, firstWritten, remoteCfg, rcvd,
                   held, deliv, err, opened>>

\* Ack history overflow: pending acks are lost unsent (P5, silence).
AckSilence(e) ==
    /\ Silence
    /\ pendAck[e] # {}
    /\ pendAck' = [pendAck EXCEPT ![e] = {}]
    /\ UNCHANGED <<cfg, nextMsg, relQ, acked, unrelQ, firstWritten, remoteCfg, rcvd,
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
(* acks reach e (OnAcked).  CONFIG, first in the packet, is compared; a    *)
(* mismatch is the error and parsing stops.  The data records are checked *)
(* against e's own num_channels: one outside is a VIOLATION and parsing    *)
(* stops (R6).  Otherwise a fresh reliable message is received once, an    *)
(* ordered one is held until its predecessors are received and then       *)
(* delivered with every hold it releases, and an unreliable one is         *)
(* delivered now.  An endpoint in error opens and discards.                *)
(***************************************************************************)
OnPayload(e, p) ==
    /\ LinkDeliver(e, p)
    /\ IF ~Live(e)
         THEN UNCHANGED epVars
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
         /\ IF cfgNew /\ ConfigCompare(e)
              THEN /\ err' = [err EXCEPT ![e] = "CONFIG_MISMATCH"]
                   /\ UNCHANGED <<remoteCfg, rcvd, held, deliv>>
              ELSE /\ remoteCfg' = IF cfgNew THEN [remoteCfg EXCEPT ![e] = cfg[Remote(e)]]
                                           ELSE remoteCfg
                   /\ IF bad # {}
                        THEN /\ err' = [err EXCEPT ![e] = "VIOLATION"]
                             /\ UNCHANGED <<rcvd, held, deliv>>
                        ELSE /\ rcvd' = [rcvd EXCEPT ![e] = rcvd1]
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
(* once the remote end is in error and silent: that time passes enough for *)
(* a live remote end's resends to land first is RNResendTiming's           *)
(* AttemptsBeforeTimeout.                                                  *)
(***************************************************************************)
Tick(e) ==
    /\ Live(e)
    /\ remoteCfg[e] = NoCfg \/ Unacked(e) # {}
    /\ TimeoutAnyTime \/ ~Live(Remote(e))
    /\ err' = [err EXCEPT ![e] = "RELIABLE_TIMEOUT"]
    /\ UNCHANGED <<cfg, nextMsg, relQ, acked, unrelQ, firstWritten, pendAck, remoteCfg,
                   rcvd, held, deliv, opened, abVars, baVars>>

(***************************************************************************)
(* The outside events: the two paths.                                      *)
(***************************************************************************)
ABEnvironment == AB!Environment /\ UNCHANGED <<epVars, baVars>>
BAEnvironment == BA!Environment /\ UNCHANGED <<epVars, abVars>>

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
