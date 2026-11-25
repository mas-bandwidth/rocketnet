----------------------------- MODULE RNPathProbe -----------------------------
(***************************************************************************)
(* The path packet size probe of the handshake: the client's walk down its *)
(* candidate packet sizes after the challenge, the server's answer to each *)
(* probe, the size the connection response carries and the slot the server *)
(* assigns with it (sdk/rocketnet_client_protocol.cpp: the probing branch  *)
(* of rocketnet_client_protocol_update, rocketnet_client_process_probe;    *)
(* sdk/rocketnet_server_protocol.cpp: rocketnet_server_process_probe, the  *)
(* response's size field, rocketnet_server_process_connection_response and *)
(* rocketnet_server_handshake_deadline_seconds; standard/TRANSPORT.md      *)
(* t8.13, t11.4 and t12 rule 14).                                          *)
(*                                                                         *)
(* The model starts after the challenge: the client holds the session     *)
(* keys and the server's mapping for the client's address is half-open    *)
(* (proven).  Probes and replies are sealed under the session keys on the *)
(* session sequences.  Sources: "C", the client; "F", a forger at the      *)
(* client's address, whose probes find C's mapping and fail to open; "X",  *)
(* an address with no mapping.                                             *)
(*                                                                         *)
(* Sizes are wire sizes up to Ceiling (1452 in the code); a control packet *)
(* (response, keep-alive) is wire size 0 here and fits any path.  ClientSet *)
(* is the client's candidate set for the committed family (P of each       *)
(* link_mtus entry, equal sizes collapsed); the walk takes it largest      *)
(* first, Cand(1) > Cand(2) > ....  ServerList is the server's own        *)
(* candidate set for that family, which may differ.  A one-entry set does  *)
(* no probing: the response goes at once with that entry's size.           *)
(*                                                                         *)
(* The path has a capacity in each direction, chosen at Init from CapsUp   *)
(* and CapsDown and fixed for the behaviour: a packet larger than its      *)
(* direction's capacity is dropped when sent, always; a packet that fits   *)
(* may be lost (LoseUp, LoseDown: at most MaxLoss in a behaviour, an      *)
(* honest probe the server's allowance withholds counted as one) and      *)
(* packets in flight are a set, delivered in any order.  An on-path       *)
(* attacker may replay a client probe in flight (a second copy, n = 1) or *)
(* send a probe of any size from X or F, at most AttackBudget times in     *)
(* all.  With Forger, a forger at the client's address sends probes from F *)
(* without limit (ForgedNeverStarves).                                     *)
(*                                                                         *)
(* Time.  Tick is one client send interval: the client may send one packet *)
(* (maySend: the send is due, or the early send an advance armed), and the *)
(* server runs an update, which resets the mapping's probe reply allowance *)
(* (answered); ServerUpdate is a further server update inside it.  A       *)
(* probing client sends at each interval, so a Tick while it probes        *)
(* follows its send.  A probe window is W sends of the candidate's probe   *)
(* (max(ROCKETNET_CLIENT_PROBE_WINDOW_SENDS, 3 round trips), at most        *)
(* ROCKETNET_CLIENT_PROBE_WINDOW_MAX_SENDS); at the send after the W-th,    *)
(* the client moves to the next candidate and sends its probe              *)
(* (ClientExpireNext) or, past the last, fails -14 (ClientExpireFail).     *)
(*                                                                         *)
(* The handshake deadline.  With Deadline > 0, age counts the Ticks the    *)
(* mapping has been half-open, and at Deadline Ticks (Deadline + Budget    *)
(* when the server's own list gives more than one candidate) the mapping   *)
(* is removed; Budget is the probe's time bound in Ticks (ROCKETNET_LINK_   *)
(* MTU_PROBE_BUDGET_SECONDS), at least the longest walk, N * W.  Deadline  *)
(* = 0 leaves the deadline out (age stays 0).                              *)
(*                                                                         *)
(* Timed.  FALSE is the code: the client sends at every send opportunity  *)
(* whatever is in flight, a window can end with its probe or reply still   *)
(* in flight (so a reply may arrive in a later window, and a reply at a    *)
(* size at or below the one being probed is taken), the response is      *)
(* resent at every opportunity, and the silence timeout (-4, the client    *)
(* state -(3 + 1)) can end sending connection response at any time; it    *)
(* waits while the client probes.  TRUE adds the assumption the liveness  *)
(* properties need: the window covers a round trip, so the client sends    *)
(* again only when nothing of its last exchange is in flight and a window  *)
(* ends only then, and the response phase is W responses, ended -4 only    *)
(* while the server holds no slot and nothing is in flight.                *)
(*                                                                         *)
(* After connected, both ends send payload at the connection's packet     *)
(* size (the client's size, the server's slot size); upGot and downGot are *)
(* ghosts set when one arrives.  No connection timeout is modelled: small  *)
(* keep-alives keep a connection whose full-size packets are all dropped   *)
(* alive, which is the hang NoHang names.                                  *)
(*                                                                         *)
(*   ClientSendProbe        a probe of the current candidate's size        *)
(*   ClientExpireNext       the window ends: the next candidate's probe    *)
(*   ClientExpireFail       the window of the last candidate ends: -14,    *)
(*                          the attempt's state zeroed (ClientReset)       *)
(*   ClientRecvReply(r)     a matching reply (a size at an index <= idx)   *)
(*                          fixes the packet size, builds the response and *)
(*                          arms the early send                            *)
(*   ClientSendResponse     the response, carrying the packet size         *)
(*   ClientTimeout          silence after the walk: -4, the attempt zeroed *)
(*   ClientRecvKeepAlive(r) connected                                      *)
(*   ServerRecvProbe(p)     a probe from C or X, in the code's order: a    *)
(*                          size on the server's own list, a mapping for   *)
(*                          the source, no slot, the update's allowance    *)
(*                          unspent (else withheld); then the open, and a  *)
(*                          probe that opens spends the allowance and is   *)
(*                          answered with a reply of its size              *)
(*   ServerRecvForged(p)    a probe from F: the same checks, and an open   *)
(*                          that fails, which spends nothing               *)
(*   ServerRecvResponse(p)  half-open and the size on its list: the slot,  *)
(*                          with that size, and the first keep-alive;      *)
(*                          otherwise ignored                              *)
(*   ServerSendKeepAlive    the connected slot's keep-alive on its clock   *)
(*   ClientSendData, ServerSendData, ClientRecvData, ServerRecvData        *)
(*   Tick, ServerUpdate     the clocks; Tick removes a half-open mapping   *)
(*                          at its deadline                                *)
(*   LoseUp, LoseDown, Replay, AttackProbe   the environment               *)
(*   Done                   the terminal stutter                           *)
(*                                                                         *)
(* Broken selects a reversed witness:                                      *)
(*   "today"       no probe: the client sends its response at once with   *)
(*                 the ceiling, the code before the probe; on a path that *)
(*                 cannot carry it the client connects on the small        *)
(*                 keep-alive and full-size payload never arrives          *)
(*                 (CarriedBothWays, and NoHang and FailsWhenNoneFits as   *)
(*                 temporal properties);                                   *)
(*   "unproven"    the server answers a probe with no mapping, or one     *)
(*                 that did not open (ReplyOnlyProven);                    *)
(*   "larger"      the server answers with its largest size, not the      *)
(*                 probe's (ReplyNoLarger);                                *)
(*   "offlist"     the server answers a size not on its own list          *)
(*                 (ReplyOwnList);                                         *)
(*   "noallowance" the server answers every probe of a mapping in one     *)
(*                 update (OneReplyPerUpdate);                             *)
(*   "slotceiling" the server's slot takes the ceiling, not the           *)
(*                 response's size (SlotAgrees);                           *)
(*   "noexpire"    the probe window never ends, so a client whose first   *)
(*                 candidate the path drops probes forever (Ends);         *)
(*   "skiplast"    the walk ends one candidate early, so a client whose   *)
(*                 path carries only its last candidate fails -14          *)
(*                 (ConnectsWhenFits);                                     *)
(*   "forgedspends" the allowance is spent before the open, so a forged   *)
(*                 probe spends it, and a forger at the client's address   *)
(*                 withholds every genuine probe (ForgedNeverStarves);     *)
(*   "nobudget"    the deadline without the probe's budget: the mapping   *)
(*                 is removed while the client walks (WalkInsideDeadline); *)
(*   "timeoutprobing" the silence timeout runs while probing, so the      *)
(*                 attempt ends -4 before the walk's own end               *)
(*                 (NoTimeoutWhileProbing).                                *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Ceiling,        \* the packet ceiling, ROCKETNET_MAX_PACKET_BYTES
    ClientSet,      \* the client's candidate packet sizes
    ServerList,     \* the server's own candidate set
    CapsUp,         \* the path capacities client -> server to explore
    CapsDown,       \* the path capacities server -> client to explore
    W,              \* sends in one probe window, and responses when Timed
    MaxLoss,        \* the most packets that fit and are lost, in a behaviour
    AttackBudget,   \* the most attacker sends (replays and probes from X or F)
    Forger,         \* TRUE: probes from F without limit
    Deadline,       \* the handshake deadline in Ticks; 0 leaves it out
    Budget,         \* the probe's time bound in Ticks
    Timed,          \* TRUE: the window covers a round trip (the liveness cases)
    Broken          \* "none", or the reversed witness

N == Cardinality(ClientSet)

ASSUME /\ Ceiling \in Nat \ {0}
       /\ ClientSet \subseteq 1..Ceiling /\ ClientSet # {}
       /\ ServerList \subseteq 1..Ceiling /\ ServerList # {}
       /\ CapsUp \subseteq 0..Ceiling /\ CapsUp # {}
       /\ CapsDown \subseteq 0..Ceiling /\ CapsDown # {}
       /\ W \in Nat \ {0} /\ MaxLoss \in Nat /\ AttackBudget \in Nat
       /\ Forger \in BOOLEAN
       /\ Deadline \in Nat /\ Budget \in Nat
       \* the code's static_assert: the longest walk fits the probe's bound
       /\ Deadline > 0 => Budget >= N * W
       /\ Timed \in BOOLEAN
       /\ Broken \in {"none", "today", "unproven", "larger", "offlist",
                      "noallowance", "slotceiling", "noexpire", "skiplast",
                      "forgedspends", "nobudget", "timeoutprobing"}

Probing == N > 1
\* the i-th largest candidate: the walk's order
Cand(i) == CHOOSE c \in ClientSet : Cardinality({d \in ClientSet : d > c}) = i - 1
Max(S) == CHOOSE m \in S : \A x \in S : x <= m

\* every size a packet can carry here
Sizes == ClientSet \cup ServerList \cup {Ceiling}

\* the server's list probes the source's family: the deadline takes the budget
ServerProbes == Cardinality(ServerList) > 1
Limit == Deadline + (IF ServerProbes /\ Broken # "nobudget" THEN Budget ELSE 0)

\* packets: one record shape for both directions
Pkt(k, src, s, p, n) == [k |-> k, src |-> src, s |-> s, p |-> p, n |-> n]
UpPackets ==
    {Pkt("probe", a, s, 0, n) : a \in {"C", "F", "X"}, s \in Sizes, n \in {0, 1}}
    \cup {Pkt("resp", "C", s, 0, 0) : s \in Sizes}
    \cup {Pkt("data", "C", s, 0, 0) : s \in Sizes}
DownPackets ==
    {Pkt("reply", "S", s, p, 0) : s \in Sizes, p \in Sizes}
    \cup {Pkt("ka", "S", 0, 0, 0)}
    \cup {Pkt("data", "S", s, 0, 0) : s \in Sizes}

VARIABLES
    capUp,          \* the path's capacity client -> server (environment)
    capDown,        \* the path's capacity server -> client (environment)
    phase,          \* "probe", "response", "connected", "failed" (-14), "timedout" (-4)
    idx,            \* probe_index + 1: the candidate being probed
    size,           \* the client's packet_bytes, 0 until chosen
    sends,          \* probe_sends (and, when Timed, the responses sent)
    maySend,        \* the client's send is due, or its early send armed
    mapping,        \* the server's mapping of C: "halfopen", "connected" or "removed"
    slot,           \* the mapping's packet_bytes, 0 until the slot
    answered,       \* the mapping's probe_update is this update
    age,            \* Ticks the mapping has been half-open (0 when Deadline = 0)
    up,             \* packets in flight client -> server (environment)
    down,           \* packets in flight server -> client (environment)
    losses,         \* packets lost so far (environment)
    attacks,        \* attacker sends so far (environment)
    proof,          \* ghost: the reply the client took: [up, down] sizes that crossed
    upGot,          \* ghost: a payload reached the server
    downGot,        \* ghost: a payload reached the client
    badUnproven,    \* ghost: a reply to a probe with no mapping or that did not open
    badLarger,      \* ghost: a reply larger than the probe it answers
    badOffList,     \* ghost: a reply of a size not on the server's list
    badTwice        \* ghost: a second reply to one mapping in one update

vars == << capUp, capDown, phase, idx, size, sends, maySend, mapping, slot,
           answered, age, up, down, losses, attacks, proof, upGot, downGot,
           badUnproven, badLarger, badOffList, badTwice >>

\* the wire size: a response and a keep-alive are control packets, size 0
\* (the response carries the packet size s in its padding)
Wire(p) == IF p.k \in {"resp", "ka"} THEN 0 ELSE p.s

\* send: a packet larger than the direction's capacity never arrives
SendUp(p) == IF Wire(p) <= capUp THEN up \cup {p} ELSE up
SendDown(p) == IF Wire(p) <= capDown THEN down \cup {p} ELSE down

ProbeInFlight == \E p \in up : p.k = "probe" /\ p.src = "C" /\ p.s = Cand(idx)
ReplyInFlight == \E r \in down : r.k = "reply" /\ r.s = Cand(idx)
ResponseInFlight == \E p \in up : p.k = "resp"
KeepAliveInFlight == \E r \in down : r.k = "ka"
ExchangeIdle == ~ProbeInFlight /\ ~ReplyInFlight

\* the last candidate the walk reaches
LastIdx == IF Broken = "skiplast" THEN N - 1 ELSE N

Init ==
    /\ capUp \in CapsUp
    /\ capDown \in CapsDown
    /\ IF Broken = "today"
       THEN /\ phase = "response" /\ size = Ceiling
       ELSE IF Probing
            THEN /\ phase = "probe" /\ size = 0
            ELSE /\ phase = "response" /\ size = Cand(1)
    /\ idx = 1
    /\ sends = 0
    /\ maySend = TRUE                \* the challenge armed the early send
    /\ mapping = "halfopen"
    /\ slot = 0
    /\ answered = FALSE
    /\ age = 0
    /\ up = {}
    /\ down = {}
    /\ losses = 0
    /\ attacks = 0
    /\ proof = [up |-> 0, down |-> 0]
    /\ upGot = FALSE
    /\ downGot = FALSE
    /\ badUnproven = FALSE
    /\ badLarger = FALSE
    /\ badOffList = FALSE
    /\ badTwice = FALSE

-----------------------------------------------------------------------------
(* The client *)

\* A terminal state ends the attempt and zeroes it (rocketnet_client_set_state,
\* rocketnet_client_connection_reset): the probe index and count, the packet
\* size and the send clock, so the next send is due at once.
ClientReset ==
    /\ idx' = 1
    /\ size' = 0
    /\ sends' = 0
    /\ maySend' = TRUE

ClientSendProbe ==
    /\ phase = "probe"
    /\ maySend
    /\ sends < W \/ Broken = "noexpire"
    /\ Timed => ExchangeIdle
    /\ up' = SendUp(Pkt("probe", "C", Cand(idx), 0, 0))
    /\ sends' = IF sends < W THEN sends + 1 ELSE sends
    /\ maySend' = FALSE
    /\ UNCHANGED << capUp, capDown, phase, idx, size, mapping, slot, answered,
                    age, down, losses, attacks >>
    /\ UNCHANGED << proof, upGot, downGot, badUnproven, badLarger, badOffList, badTwice >>

ClientExpireNext ==
    /\ phase = "probe"
    /\ maySend
    /\ sends >= W
    /\ Broken # "noexpire"
    /\ idx < LastIdx
    /\ Timed => ExchangeIdle
    /\ idx' = idx + 1
    /\ sends' = 1
    /\ up' = SendUp(Pkt("probe", "C", Cand(idx + 1), 0, 0))
    /\ maySend' = FALSE
    /\ UNCHANGED << capUp, capDown, phase, size, mapping, slot, answered,
                    age, down, losses, attacks >>
    /\ UNCHANGED << proof, upGot, downGot, badUnproven, badLarger, badOffList, badTwice >>

ClientExpireFail ==
    /\ phase = "probe"
    /\ maySend
    /\ sends >= W
    /\ Broken # "noexpire"
    /\ idx >= LastIdx
    /\ Timed => ExchangeIdle
    /\ phase' = "failed"
    /\ ClientReset
    /\ UNCHANGED << capUp, capDown, mapping, slot, answered, age, up, down,
                    losses, attacks >>
    /\ UNCHANGED << proof, upGot, downGot, badUnproven, badLarger, badOffList, badTwice >>

\* A reply opens under the session server-to-client key (every reply here
\* is sealed under C's mapping); it matches when its size is a candidate at
\* an index at or below the one being probed. Anything else is ignored.
ClientRecvReply(r) ==
    /\ r \in down
    /\ r.k = "reply"
    /\ down' = down \ {r}
    /\ IF phase = "probe" /\ \E j \in 1..idx : Cand(j) = r.s
       THEN /\ phase' = "response"
            /\ size' = r.s
            /\ sends' = IF Timed THEN 0 ELSE sends
            /\ maySend' = TRUE        \* the early send for this advance
            /\ proof' = [up |-> r.p, down |-> r.s]
       ELSE /\ UNCHANGED << phase, size, sends, maySend >>
            /\ UNCHANGED proof
    /\ UNCHANGED << capUp, capDown, idx, mapping, slot, answered, age, up,
                    losses, attacks, upGot, downGot, badUnproven, badLarger,
                    badOffList, badTwice >>

ClientSendResponse ==
    /\ phase = "response"
    /\ maySend
    /\ Timed => sends < W /\ ~ResponseInFlight /\ ~KeepAliveInFlight
    /\ up' = SendUp(Pkt("resp", "C", size, 0, 0))
    /\ sends' = IF Timed THEN sends + 1 ELSE sends
    /\ maySend' = FALSE
    /\ UNCHANGED << capUp, capDown, phase, idx, size, mapping, slot, answered,
                    age, down, losses, attacks >>
    /\ UNCHANGED << proof, upGot, downGot, badUnproven, badLarger, badOffList, badTwice >>

\* the silence timeout waits while the client probes: the walk ends in its
\* own error, -14 (rocketnet_client_check_deadlines, !client->probing)
ClientTimeout ==
    /\ IF Timed
       THEN /\ phase = "response"
            /\ sends = W
            /\ ~ResponseInFlight /\ ~KeepAliveInFlight
            /\ mapping \in {"halfopen", "removed"}
       ELSE IF Broken = "timeoutprobing"
            THEN phase \in {"probe", "response"}
            ELSE phase = "response"
    /\ phase' = "timedout"
    /\ ClientReset
    /\ UNCHANGED << capUp, capDown, mapping, slot, answered, age, up, down,
                    losses, attacks >>
    /\ UNCHANGED << proof, upGot, downGot, badUnproven, badLarger, badOffList, badTwice >>

ClientRecvKeepAlive(r) ==
    /\ r \in down
    /\ r.k = "ka"
    /\ down' = down \ {r}
    /\ phase' = IF phase = "response" THEN "connected" ELSE phase
    /\ UNCHANGED << capUp, capDown, idx, size, sends, maySend, mapping, slot,
                    answered, age, up, losses, attacks >>
    /\ UNCHANGED << proof, upGot, downGot, badUnproven, badLarger, badOffList, badTwice >>

ClientSendData ==
    /\ phase = "connected"
    /\ ~\E p \in up : p.k = "data"
    /\ up' = SendUp(Pkt("data", "C", size, 0, 0))
    /\ up' # up
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, answered, age, down, losses, attacks >>
    /\ UNCHANGED << proof, upGot, downGot, badUnproven, badLarger, badOffList, badTwice >>

ClientRecvData(r) ==
    /\ r \in down
    /\ r.k = "data"
    /\ down' = down \ {r}
    /\ IF phase = "connected" THEN downGot' = TRUE ELSE UNCHANGED downGot
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, answered, age, up, losses, attacks >>
    /\ UNCHANGED << proof, upGot, badUnproven, badLarger, badOffList, badTwice >>

-----------------------------------------------------------------------------
(* The server *)

\* the code's checks, in its order; each witness drops one
OnList(p) == Broken = "offlist" \/ p.s \in ServerList
Mapped(p) == p.src \in {"C", "F"} /\ mapping # "removed"   \* C's mapping
HalfOpen(p) == Mapped(p) /\ mapping = "halfopen"
Allowed == Broken = "noallowance" \/ ~answered
Opens(p) == p.src = "C"                      \* only C holds the session keys
ReplySize(p) == IF Broken = "larger" THEN Max(ServerList) ELSE p.s

\* the checks before the open pass
Reaches(p) == OnList(p) /\ HalfOpen(p) /\ Allowed
\* only a probe that opened spends the update's allowance; "forgedspends"
\* spends it before the open, as the code did
Spend(p) == Reaches(p) /\ (Opens(p) \/ Broken = "forgedspends")
Answer(p) ==
    IF Broken = "unproven"
    THEN OnList(p) /\ (~Mapped(p) \/ Reaches(p))
    ELSE Reaches(p) /\ Opens(p)

\* An honest probe the allowance withholds: the client's probe arrived in the
\* same server update as an earlier one that opened (delay bunched them, or
\* a replayed copy was first; under "forgedspends", a forged probe). To the
\* client that is a lost attempt, so it spends the loss budget.
Withheld(p) == OnList(p) /\ HalfOpen(p) /\ ~Allowed /\ Opens(p) /\ p.n = 0

\* one receive of a probe: the same code for every source
RecvProbe(p) ==
    /\ p \in up
    /\ p.k = "probe"
    /\ up' = up \ {p}
    /\ Withheld(p) => losses < MaxLoss
    /\ losses' = IF Withheld(p) THEN losses + 1 ELSE losses
    /\ answered' = IF Spend(p) THEN TRUE ELSE answered
    /\ down' = IF Answer(p) /\ Mapped(p)
               THEN SendDown(Pkt("reply", "S", ReplySize(p), p.s, 0))
               ELSE down     \* no reply, or one to X: never reaches C
    /\ IF Answer(p) /\ ~(Mapped(p) /\ Opens(p)) THEN badUnproven' = TRUE ELSE UNCHANGED badUnproven
    /\ IF Answer(p) /\ ReplySize(p) > p.s THEN badLarger' = TRUE ELSE UNCHANGED badLarger
    /\ IF Answer(p) /\ ReplySize(p) \notin ServerList THEN badOffList' = TRUE ELSE UNCHANGED badOffList
    /\ IF Answer(p) /\ Mapped(p) /\ answered THEN badTwice' = TRUE ELSE UNCHANGED badTwice
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, age, attacks, proof, upGot, downGot >>

\* a probe from the client, a replayed copy of one, or one from X
ServerRecvProbe(p) == p.src # "F" /\ RecvProbe(p)

\* a probe forged at the client's address: it reaches the open and fails,
\* and spends nothing, so the genuine probe of the update is still answered
ServerRecvForged(p) == p.src = "F" /\ RecvProbe(p)

\* the response opens; the size it carries must be on the server's own list;
\* the slot is assigned with it and the first keep-alive goes back at once.
\* A response to a connected or removed mapping is ignored (a connected
\* slot's keep-alives are on their own clock, ServerSendKeepAlive).
ServerRecvResponse(p) ==
    /\ p \in up
    /\ p.k = "resp"
    /\ up' = up \ {p}
    /\ IF mapping = "halfopen" /\ p.s \in ServerList
       THEN /\ mapping' = "connected"
            /\ slot' = IF Broken = "slotceiling" THEN Ceiling ELSE p.s
            /\ down' = SendDown(Pkt("ka", "S", 0, 0, 0))
       ELSE UNCHANGED << mapping, slot, down >>
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, answered,
                    age, losses, attacks >>
    /\ UNCHANGED << proof, upGot, downGot, badUnproven, badLarger, badOffList, badTwice >>

ServerSendKeepAlive ==
    /\ mapping = "connected"
    /\ Timed => ~KeepAliveInFlight
    /\ down' = SendDown(Pkt("ka", "S", 0, 0, 0))
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, answered, age, up, losses, attacks >>
    /\ UNCHANGED << proof, upGot, downGot, badUnproven, badLarger, badOffList, badTwice >>

ServerSendData ==
    /\ mapping = "connected"
    /\ ~\E r \in down : r.k = "data"
    /\ down' = SendDown(Pkt("data", "S", slot, 0, 0))
    /\ down' # down
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, answered, age, up, losses, attacks >>
    /\ UNCHANGED << proof, upGot, downGot, badUnproven, badLarger, badOffList, badTwice >>

ServerRecvData(p) ==
    /\ p \in up
    /\ p.k = "data"
    /\ up' = up \ {p}
    /\ IF mapping = "connected" THEN upGot' = TRUE ELSE UNCHANGED upGot
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, answered, age, down, losses, attacks >>
    /\ UNCHANGED << proof, downGot, badUnproven, badLarger, badOffList, badTwice >>

-----------------------------------------------------------------------------
(* Time and the environment *)

\* One client send interval and the server's update in it: the allowance is
\* renewed, and a half-open mapping one Tick older is removed at its
\* deadline (rocketnet_server_protocol_update). A probing client sends at
\* every interval, so while it probes a Tick follows its send.
Tick ==
    /\ ~maySend \/ answered
    /\ phase = "probe" => ~maySend
    /\ maySend' = TRUE
    /\ answered' = FALSE
    /\ IF Deadline > 0 /\ mapping = "halfopen"
       THEN /\ age' = age + 1
            /\ mapping' = IF age + 1 >= Limit THEN "removed" ELSE mapping
       ELSE UNCHANGED << age, mapping >>
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, slot, up,
                    down, losses, attacks >>
    /\ UNCHANGED << proof, upGot, downGot, badUnproven, badLarger, badOffList, badTwice >>

ServerUpdate ==
    /\ answered
    /\ answered' = FALSE
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, age, up, down, losses, attacks >>
    /\ UNCHANGED << proof, upGot, downGot, badUnproven, badLarger, badOffList, badTwice >>

\* a packet that fits is lost on the way (an attacker's needs no loss)
LoseUp(p) ==
    /\ losses < MaxLoss
    /\ p \in up
    /\ p.src = "C" /\ p.n = 0
    /\ up' = up \ {p}
    /\ losses' = losses + 1
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, answered, age, down, attacks >>
    /\ UNCHANGED << proof, upGot, downGot, badUnproven, badLarger, badOffList, badTwice >>

LoseDown(r) ==
    /\ losses < MaxLoss
    /\ r \in down
    /\ down' = down \ {r}
    /\ losses' = losses + 1
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, answered, age, up, attacks >>
    /\ UNCHANGED << proof, upGot, downGot, badUnproven, badLarger, badOffList, badTwice >>

\* an on-path copy of a client probe in flight, the client's address its source
Replay(p) ==
    /\ attacks < AttackBudget
    /\ p \in up
    /\ p.k = "probe" /\ p.src = "C" /\ p.n = 0
    /\ up' = up \cup {[p EXCEPT !.n = 1]}
    /\ up' # up
    /\ attacks' = attacks + 1
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, answered, age, down, losses >>
    /\ UNCHANGED << proof, upGot, downGot, badUnproven, badLarger, badOffList, badTwice >>

\* a probe of any size that does not open: from X, an address with no
\* mapping, or from F, a forger at the client's own address; with Forger,
\* F's probes are not counted against the budget
AttackProbe(a, s) ==
    /\ attacks < AttackBudget \/ (Forger /\ a = "F")
    /\ up' = up \cup {Pkt("probe", a, s, 0, 0)}
    /\ up' # up
    /\ attacks' = IF Forger /\ a = "F" THEN attacks ELSE attacks + 1
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, answered, age, down, losses >>
    /\ UNCHANGED << proof, upGot, downGot, badUnproven, badLarger, badOffList, badTwice >>

\* the attempt is over: failed and timed out end it, and a connected client
\* whose packets are all delivered or dropped has nothing left to do
Done ==
    /\ phase \in {"connected", "failed", "timedout"}
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping, slot,
                    answered, age, up, down, losses, attacks, proof, upGot, downGot,
                    badUnproven, badLarger, badOffList, badTwice >>

Next ==
    \/ ClientSendProbe
    \/ ClientExpireNext
    \/ ClientExpireFail
    \/ \E r \in down : ClientRecvReply(r)
    \/ ClientSendResponse
    \/ ClientTimeout
    \/ \E r \in down : ClientRecvKeepAlive(r)
    \/ ClientSendData
    \/ \E r \in down : ClientRecvData(r)
    \/ \E p \in up : ServerRecvProbe(p)
    \/ \E p \in up : ServerRecvForged(p)
    \/ \E p \in up : ServerRecvResponse(p)
    \/ ServerSendKeepAlive
    \/ ServerSendData
    \/ \E p \in up : ServerRecvData(p)
    \/ Tick
    \/ ServerUpdate
    \/ \E p \in up : LoseUp(p)
    \/ \E r \in down : LoseDown(r)
    \/ \E p \in up : Replay(p)
    \/ \E a \in {"X", "F"}, s \in Sizes : AttackProbe(a, s)
    \/ Done

\* Every step of the two ends and every delivery is weakly fair; the clocks
\* too: the server keeps updating, so a spent allowance is renewed even while
\* a probing client waits for its exchange. Loss and the attacker are not.
Fairness ==
    /\ WF_vars(ClientSendProbe)
    /\ WF_vars(ClientExpireNext)
    /\ WF_vars(ClientExpireFail)
    /\ WF_vars(ClientSendResponse)
    /\ WF_vars(ClientTimeout)
    /\ WF_vars(ClientSendData)
    /\ WF_vars(ServerSendKeepAlive)
    /\ WF_vars(ServerSendData)
    /\ WF_vars(Tick)
    /\ WF_vars(ServerUpdate)
    /\ \A r \in DownPackets : WF_vars(ClientRecvReply(r))
    /\ \A r \in DownPackets : WF_vars(ClientRecvKeepAlive(r))
    /\ \A r \in DownPackets : WF_vars(ClientRecvData(r))
    /\ \A p \in UpPackets : WF_vars(ServerRecvProbe(p))
    /\ \A p \in UpPackets : WF_vars(ServerRecvForged(p))
    /\ \A p \in UpPackets : WF_vars(ServerRecvResponse(p))
    /\ \A p \in UpPackets : WF_vars(ServerRecvData(p))

Spec == Init /\ [][Next]_vars
FairSpec == Spec /\ Fairness

-----------------------------------------------------------------------------
(* Safety *)

TypeOK ==
    /\ capUp \in 0..Ceiling /\ capDown \in 0..Ceiling
    /\ phase \in {"probe", "response", "connected", "failed", "timedout"}
    /\ idx \in 1..N
    /\ size \in {0} \cup Sizes
    /\ sends \in 0..W
    /\ maySend \in BOOLEAN
    /\ mapping \in {"halfopen", "connected", "removed"}
    /\ slot \in {0} \cup Sizes
    /\ answered \in BOOLEAN
    /\ age \in 0..Limit
    /\ up \subseteq UpPackets
    /\ down \subseteq DownPackets
    /\ losses \in 0..MaxLoss
    /\ attacks \in 0..AttackBudget
    /\ proof \in [up : {0} \cup Sizes, down : {0} \cup Sizes]
    /\ {upGot, downGot, badUnproven, badLarger, badOffList, badTwice} \subseteq BOOLEAN

\* (1) both ends hold the same packet size: when the client is connected the
\* server's slot is assigned, with the client's size
SlotAgrees == phase = "connected" => mapping = "connected" /\ slot = size

\* the chosen size is one of the client's own candidates
ChosenIsCandidate == phase \in {"response", "connected"} => size \in ClientSet

\* (2) a probing client is connected only at a size the path carries both
\* ways, and a packet of that size has crossed each way: the probe the reply
\* answers up, the reply down. A one-entry set does no probing.
CarriedBothWays ==
    (phase = "connected" /\ Probing) => size <= capUp /\ size <= capDown
CrossedBothWays ==
    (phase = "connected" /\ Probing) => proof.up >= size /\ proof.down >= size

\* (3) the server never answers a probe that has no mapping or did not open
\* under the session keys, never with a packet larger than the one it
\* answers, and only with sizes on its own list
ReplyOnlyProven == ~badUnproven
ReplyNoLarger == ~badLarger
ReplyOwnList == ~badOffList

\* (4) at most one probe reply per mapping per update
OneReplyPerUpdate == ~badTwice

\* (5) the walk ends inside the server's handshake deadline: while the client
\* probes, its mapping is still there (rocketnet_server_handshake_deadline_seconds)
WalkInsideDeadline == phase = "probe" => mapping # "removed"

\* (6) a probing client's attempt ends -4 only after the walk has chosen a
\* size: the walk itself ends in -14, never in the silence timeout
NoTimeoutWhileProbing ==
    (phase = "timedout" /\ Probing /\ Broken # "today") => proof.down > 0

\* Reachability, not a rule: a probing client never takes a reply from an
\* earlier window than the one it is in. MCRNPathProbeReachLateReply.cfg
\* must violate it, so the late-reply path of the code is explored.
NoLateReplyTaken ==
    (phase \in {"response", "connected"} /\ Probing) => size = Cand(idx)

-----------------------------------------------------------------------------
(* Liveness, under FairSpec with Timed *)

Fits(c) == c \in ServerList /\ c <= capUp /\ c <= capDown
SomeFits == IF Probing THEN \E i \in 1..N : Fits(Cand(i))
                       ELSE Cand(1) \in ServerList
LossNotTotal == MaxLoss < W

\* the attempt ends: connected, -14 or -4; never probing or responding forever
Ends == <>(phase \in {"connected", "failed", "timedout"})

\* a probing client that connects gets full-size payload through both ways:
\* no hang where small keep-alives keep a connection alive that carries nothing
NoHang == Probing => [](phase = "connected" => <>(upGot /\ downGot))

\* if a candidate on both lists fits both ways and loss is not total, it connects
ConnectsWhenFits == (SomeFits /\ LossNotTotal) => <>(phase = "connected")

\* if none fits, the probing client ends in the named failure, -14
FailsWhenNoneFits == (Probing /\ ~SomeFits) => <>(phase = "failed")

\* a forger at the client's address, sending without limit, cannot starve a
\* client whose path fits: its probes spend nothing, so the genuine probe of
\* every update is still answered (rocketnet_server_process_probe)
ForgedNeverStarves ==
    (Forger /\ SomeFits /\ LossNotTotal) => <>(phase = "connected")

=============================================================================
