-------------------------- MODULE ClientConnection --------------------------
(***************************************************************************)
(* rocketnet: the client's connection state machine.                       *)
(*                                                                         *)
(* The client of sdk/rocketnet_client_protocol.cpp, driven over two        *)
(* instances of the shared lossy link (tla/Link.tla): C2S carries what the *)
(* client sends, S2C what the server sends.  The server is the environment *)
(* the client talks to, modelled only as far as it drives the client: it   *)
(* answers a request with a cookie, a challenge or a denied, a response    *)
(* with a keep-alive or a denied, sends keep-alives on its timer while the  *)
(* slot is held, may send payloads, may disconnect or crash, and forgets a  *)
(* half-open mapping at its handshake deadline and an idle one at the      *)
(* timeout (sdk/rocketnet_server_protocol.cpp).                            *)
(*                                                                         *)
(* States are the standard's numbers (STANDARD.md, Client State Machine;   *)
(* sdk/rocketnet.h:398-409): -7 token expired, -6 invalid token, -5        *)
(* connection timed out, -4 response timed out, -3 request timed out, -2   *)
(* cookie request timed out, -1 denied, 0 disconnected, 1 requesting       *)
(* cookie, 2 sending connection request, 3 sending connection response,    *)
(* 4 connected.  -6 is decided before anything is sent and is not          *)
(* modelled: a token that fails the pre-send checks never reaches the      *)
(* wire.                                                                   *)
(*                                                                         *)
(* Keys.  An attempt's salt and a mapping's session nonce are the two      *)
(* fresh values every key is derived from, so a key pair here is the       *)
(* record [att, nonce]: a sealed packet opens iff its [att, nonce] is the  *)
(* pair the receiver holds.  The client's ephemeral keypair is the boolean *)
(* cEph (live or erased).  A cookie is a flag: the cookie gate itself is   *)
(* the security module's business.                                         *)
(*                                                                         *)
(* Sequence numbers and the replay window.  Server session packets carry   *)
(* an id from a small pool instead of a 64-bit sequence.  A fresh send     *)
(* takes an id no copy on the wire carries and forgets it from the         *)
(* client's seen-set, so a fresh packet is never refused as a replay and a *)
(* copy of an older packet with that id cannot exist.  A copy whose id is  *)
(* in the seen-set is the replay the window refuses                        *)
(* (rocketnet_replay_already_received, sdk/rocketnet_client_protocol.cpp). *)
(* Handshake packets need no id: the cookie is idempotent, the challenge   *)
(* and the denied are gated by state.                                      *)
(*                                                                         *)
(* Time.  Tick is one send interval's worth of the clock passing on both   *)
(* ends; every timer is a saturating counter of ticks, so the state space  *)
(* is finite without a clock.  A deadline or timeout of 0 is off, as the   *)
(* code's timeout_seconds = 0 is off; the total deadline cannot be off in  *)
(* the code, and Deadline = 0 is the model's way of asking "does the       *)
(* protocol alone get there".                                              *)
(*                                                                         *)
(* Loss.  Both links take a Link pattern.  KindLoss adds the pattern the   *)
(* request named that Link cannot express without reading packets, "the   *)
(* first packet of every kind is dropped": the sender's first send of each *)
(* type is lost before it reaches the link.                                *)
(*                                                                         *)
(* Witnesses.  Two constants reproduce the hang class the request named,   *)
(* a server that moves to a new state, sends the packet announcing it      *)
(* once, and never again: RepeatKeepAlive = FALSE sends the establishing   *)
(* keep-alive once at the response; KeepAliveClock = "anysend" paces the   *)
(* keep-alive off the last send of anything, which a payload stream        *)
(* suppresses forever (the #90 regression the server code's comment        *)
(* records, sdk/rocketnet_server_protocol.cpp, write_keep_alive_if_due).   *)
(* The code's behaviour is RepeatKeepAlive = TRUE, KeepAliveClock =        *)
(* "keepalive".                                                            *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    SendInterval,     \* ticks between scheduled sends, both ends; >= 1
    Timeout,          \* idle timeout in ticks, both ends; 0 = off
    Deadline,         \* the client's total connect deadline in ticks; 0 = off
    ServerDeadline,   \* the server's handshake deadline for a mapping; 0 = off
    MaxAttempts,      \* how many times the application may connect
    MaxNonces,        \* the pool of session nonces (one per server mapping)
    MaxIds,           \* the pool of ids for server session packets
    MaxSlots,         \* the server's client slots
    Others,           \* other clients holding slots at the start
    OthersMayJoin,    \* BOOLEAN: may another client take a free slot
    Disconnects,      \* BOOLEAN: may either application disconnect, may the server crash
    ServerMute,       \* BOOLEAN: the server answers nothing (the silent server)
    ServerPayloads,   \* BOOLEAN: may the server application send payloads
    DisconnectCopies, \* the redundant disconnect burst (10 in the code)
    KindLoss,         \* "none" or "firstkind"
    RepeatKeepAlive,  \* witness knob; the code is TRUE
    KeepAliveClock,   \* witness knob: "keepalive" (the code) or "anysend"
    C2SPattern, S2CPattern,                         \* Link patterns
    Capacity, MaxDelay, Duplication, Reordering,    \* Link parameters
    BurstStart, BurstLen

ASSUME /\ SendInterval \in Nat \ {0}
       /\ Timeout \in Nat /\ Deadline \in Nat /\ ServerDeadline \in Nat
       /\ MaxAttempts \in Nat \ {0} /\ MaxNonces \in Nat \ {0} /\ MaxIds \in Nat \ {0}
       /\ MaxSlots \in Nat \ {0} /\ Others \in 0..MaxSlots
       /\ OthersMayJoin \in BOOLEAN /\ Disconnects \in BOOLEAN
       /\ ServerMute \in BOOLEAN /\ ServerPayloads \in BOOLEAN
       /\ DisconnectCopies \in Nat \ {0}
       /\ KindLoss \in {"none", "firstkind"}
       /\ RepeatKeepAlive \in BOOLEAN
       /\ KeepAliveClock \in {"keepalive", "anysend"}

Att   == 1..MaxAttempts
Nonce == 1..MaxNonces
Id    == 1..MaxIds

(***************************************************************************)
(* The family race (TRANSPORT.md t11 rule 3; RocketnetClientRace in        *)
(* sdk/rocketnet_client_transport.cpp).  A token naming both families on a *)
(* host with both puts both families in flight (RaceFamilies): the         *)
(* requesting-cookie request goes to every family in flight, the server    *)
(* answers a cookie only on a family that reaches it (ReachableFamilies:   *)
(* a broken IPv6 route reaches nothing), and the first valid cookie        *)
(* commits the attempt to the family it arrived on (cFam).  From then on   *)
(* the request, with its cookie, goes to that family only, and a cookie    *)
(* from the other family is dropped.  The default races one family, the   *)
(* one-address client every other run checks.  CommitBeforeAnswer is the  *)
(* reversed witness: the client commits while requesting a cookie, before *)
(* any answer (ClientCommitEarly).  Definitions, replaced with <- in the   *)
(* race cfgs, so every other run keeps its cfg.                            *)
(***************************************************************************)
Families           == {"ipv4", "ipv6"}
RaceFamilies       == {"ipv4"}
ReachableFamilies  == {"ipv4"}
CommitBeforeAnswer == FALSE

NoKeys == [att |-> 0, nonce |-> 0]
Keys   == [att : Att, nonce : Nonce]

(***************************************************************************)
(* The packets.  Types are the standard's names.  The client's standalone  *)
(* ack (debt threshold 64) and its payloads are not here: neither moves    *)
(* the connection machine.                                                 *)
(***************************************************************************)
SessionTypes == {"keepalive", "disconnect"} \cup (IF ServerPayloads THEN {"payload"} ELSE {})

ClientPackets ==
    [type : {"request"}, att : Att, cookie : BOOLEAN, fams : (SUBSET Families) \ {{}}]
    \cup [type : {"response", "keepalive", "disconnect"}, att : Att, nonce : Nonce]

ServerPackets ==
    [type : {"cookie"}, att : Att, fam : Families]
    \cup [type : {"challenge"}, att : Att, nonce : Nonce]
    \cup [type : {"denied"}, att : Att, nonce : Nonce \cup {0}]
    \cup [type : SessionTypes, att : Att, nonce : Nonce, id : Id]

SessionPackets == [type : SessionTypes, att : Att, nonce : Nonce, id : Id]

VARIABLES
    \* the client (sdk/rocketnet_client_protocol.h, struct rocketnet_client_protocol_t)
    cstate,        \* -7..4
    cAttempt,      \* the attempt number: salt and ephemeral, fresh per connect
    cHasCookie,    \* has_cookie
    cKeys,         \* the committed keys, NoKeys before the challenge
    cEph,          \* client_ephemeral_secret_key is live (not zeroed)
    cIndex,        \* client_index is known (set by the first keep-alive)
    cSinceSend,    \* ticks since last_send_time, saturating at SendInterval
    cSinceRecv,    \* ticks since last_receive_time, saturating at Timeout
    cAge,          \* ticks since attempt_start_time, saturating at Deadline
    cSeen,         \* replay_session: ids of accepted server session packets
    cReasm,        \* client reassembly state exists (a fragment was accepted)
    cDiscPending,  \* disconnect copies still to send after an app disconnect
    cDiscKeys,     \* the keys those copies are sealed under
    cLostKinds,    \* KindLoss bookkeeping: types whose first send was lost
    cFam,          \* the family the race committed to; "none" before the first cookie
    cReqFams,      \* ghost: the families a request carrying a cookie went to, this attempt
    \* the server, as far as the client can see it
    sMap,          \* the encryption mapping for the client's address
    sOthers,       \* slots held by other clients
    sReasm,        \* server reassembly state exists for the mapping's slot
    sDiscPending,  \* disconnect copies still to send after a server disconnect
    sDiscKeys, sDiscId,
    sLostKinds,
    \* the links
    c2sWire, c2sSent, c2sPulse,
    s2cWire, s2cSent, s2cPulse

raceVars   == <<cFam, cReqFams>>
clientVars == <<cstate, cAttempt, cHasCookie, cKeys, cEph, cIndex, cSinceSend,
                cSinceRecv, cAge, cSeen, cReasm, cDiscPending, cDiscKeys, cLostKinds, raceVars>>
serverVars == <<sMap, sOthers, sReasm, sDiscPending, sDiscKeys, sDiscId, sLostKinds>>
c2sVars    == <<c2sWire, c2sSent, c2sPulse>>
s2cVars    == <<s2cWire, s2cSent, s2cPulse>>
vars       == <<clientVars, serverVars, c2sVars, s2cVars>>

C2S == INSTANCE Link WITH wire <- c2sWire, sent <- c2sSent, pulse <- c2sPulse,
                          Packets <- ClientPackets, Pattern <- C2SPattern
S2C == INSTANCE Link WITH wire <- s2cWire, sent <- s2cSent, pulse <- s2cPulse,
                          Packets <- ServerPackets, Pattern <- S2CPattern

NoMap == [live |-> FALSE, att |-> 0, nonce |-> 0, slot |-> FALSE, age |-> 0,
          sinceRecv |-> 0, sinceKA |-> 0, sinceSend |-> 0, acked |-> FALSE]

Mapping == [live : BOOLEAN, att : Att \cup {0}, nonce : Nonce \cup {0}, slot : BOOLEAN,
            age : 0..ServerDeadline, sinceRecv : 0..Timeout,
            sinceKA : 0..SendInterval, sinceSend : 0..SendInterval, acked : BOOLEAN]

\* a saturating tick of a counter whose bound may be 0 (off)
Sat(x, bound) == IF bound = 0 THEN 0 ELSE IF x < bound THEN x + 1 ELSE bound

FreeSlots == MaxSlots - sOthers - (IF sMap.live /\ sMap.slot THEN 1 ELSE 0)

KeysOf(p) == [att |-> p.att, nonce |-> p.nonce]

(***************************************************************************)
(* Fresh values.  A nonce no packet in flight carries and no key in use    *)
(* holds; an id no server packet in flight carries.                        *)
(***************************************************************************)
WireNonces(w) == {w[i].pkt.nonce : i \in {j \in DOMAIN w : "nonce" \in DOMAIN w[j].pkt}}
WireIds(w)    == {w[i].pkt.id : i \in {j \in DOMAIN w : "id" \in DOMAIN w[j].pkt}}

FreshNonce == Nonce \ (WireNonces(c2sWire) \cup WireNonces(s2cWire)
                       \cup {cKeys.nonce, cDiscKeys.nonce, sDiscKeys.nonce})

FreshId == Id \ (WireIds(s2cWire) \cup (IF sDiscPending > 0 THEN {sDiscId} ELSE {}))

(***************************************************************************)
(* Sending through the links, with the first-of-kind loss in front.        *)
(***************************************************************************)
KindLost(lost, t) == KindLoss = "firstkind" /\ t \notin lost

ClientSendPacket(p) ==
    IF KindLost(cLostKinds, p.type)
      THEN cLostKinds' = cLostKinds \cup {p.type} /\ UNCHANGED c2sVars
      ELSE C2S!Send(p) /\ UNCHANGED cLostKinds

ServerSendPacket(p) ==
    IF KindLost(sLostKinds, p.type)
      THEN sLostKinds' = sLostKinds \cup {p.type} /\ UNCHANGED s2cVars
      ELSE S2C!Send(p) /\ UNCHANGED sLostKinds

\* a server session packet at a fresh id; the client's seen-set forgets the
\* id so the fresh packet is never read as a replay (see the header)
ServerSendSession(t, i) ==
    /\ ServerSendPacket([type |-> t, att |-> sMap.att, nonce |-> sMap.nonce, id |-> i])
    /\ cSeen' = cSeen \ {i}

Init ==
    /\ cstate = 0 /\ cAttempt = 0 /\ cHasCookie = FALSE /\ cKeys = NoKeys
    /\ cEph = FALSE /\ cIndex = FALSE
    /\ cSinceSend = 0 /\ cSinceRecv = 0 /\ cAge = 0 /\ cSeen = {}
    /\ cDiscPending = 0 /\ cDiscKeys = NoKeys /\ cLostKinds = {} /\ cReasm = FALSE
    /\ cFam = "none" /\ cReqFams = {}
    /\ sMap = NoMap /\ sOthers = Others /\ sReasm = FALSE
    /\ sDiscPending = 0 /\ sDiscKeys = NoKeys /\ sDiscId = 0 /\ sLostKinds = {}
    /\ C2S!Init /\ S2C!Init

(***************************************************************************)
(* The application.                                                        *)
(***************************************************************************)

\* rocketnet_client_protocol_connect (sdk/rocketnet_client_protocol.cpp:
\* struct reset, fresh salt and ephemeral, sequences at one, state 1; the
\* transport exposes it only at create, so it runs from a non-positive state)
AppConnect ==
    /\ cstate <= 0 /\ cAttempt < MaxAttempts
    /\ cstate' = 1 /\ cAttempt' = cAttempt + 1
    /\ cHasCookie' = FALSE /\ cKeys' = NoKeys /\ cEph' = TRUE /\ cIndex' = FALSE
    /\ cReasm' = FALSE
    /\ cSinceSend' = SendInterval        \* "the first update sends immediately"
    /\ cSinceRecv' = 0 /\ cAge' = 0 /\ cSeen' = {}
    /\ cFam' = "none" /\ cReqFams' = {}  \* every family in flight again
    /\ UNCHANGED <<cDiscPending, cDiscKeys, cLostKinds, serverVars, c2sVars, s2cVars>>

\* the pre-fix retry, only in SpecRetryKeepsAttempt: a connect from a failure
\* state (cstate < 0) that starts the new attempt but keeps the earlier
\* attempt's cookie and replay window. Not a disjunct of Next.
AppConnectKeepsAttempt ==
    /\ cstate < 0 /\ cAttempt < MaxAttempts
    /\ cstate' = 1 /\ cAttempt' = cAttempt + 1
    /\ cKeys' = NoKeys /\ cEph' = TRUE /\ cIndex' = FALSE /\ cReasm' = FALSE
    /\ cSinceSend' = SendInterval
    /\ cSinceRecv' = 0 /\ cAge' = 0
    /\ cFam' = "none" /\ cReqFams' = {}
    /\ UNCHANGED <<cHasCookie, cSeen, cDiscPending, cDiscKeys, cLostKinds,
                   serverVars, c2sVars, s2cVars>>

\* rocketnet_client_protocol_disconnect: in 3 or 4 a burst of sealed
\* disconnects, then state 0 and the keys and ephemeral zeroed; from any
\* other non-zero state (including a terminal one, F8) state 0 and the
\* zeroing alone
AppDisconnect ==
    /\ Disconnects /\ cstate # 0 /\ cDiscPending = 0
    /\ IF cstate \in {3, 4}
         THEN cDiscPending' = DisconnectCopies /\ cDiscKeys' = cKeys
         ELSE UNCHANGED <<cDiscPending, cDiscKeys>>
    /\ cstate' = 0 /\ cKeys' = NoKeys /\ cEph' = FALSE /\ cReasm' = FALSE
    /\ UNCHANGED <<cAttempt, cHasCookie, cIndex, cSinceSend, cSinceRecv, cAge, cSeen,
                   cLostKinds, raceVars, serverVars, c2sVars, s2cVars>>

(***************************************************************************)
(* The client's scheduled send: rocketnet_client_protocol_update, the one   *)
(* sending rule.  Nothing is sent because a packet arrived.                *)
(***************************************************************************)
\* the families the request goes to: every family in flight until the
\* first cookie commits the attempt, then the committed family alone
InFlight == IF cFam = "none" THEN RaceFamilies ELSE {cFam}

ClientPacket ==
    CASE cstate \in {1, 2} -> [type |-> "request", att |-> cAttempt, cookie |-> cHasCookie, fams |-> InFlight]
      [] cstate = 3        -> [type |-> "response", att |-> cKeys.att, nonce |-> cKeys.nonce]
      [] cstate = 4        -> [type |-> "keepalive", att |-> cKeys.att, nonce |-> cKeys.nonce]

ClientSend ==
    /\ cstate \in 1..4 /\ cSinceSend = SendInterval
    /\ ClientSendPacket(ClientPacket)
    /\ cSinceSend' = 0
    /\ cReqFams' = IF cstate \in {1, 2} /\ cHasCookie THEN cReqFams \cup InFlight ELSE cReqFams
    /\ UNCHANGED <<cstate, cAttempt, cHasCookie, cKeys, cEph, cIndex, cSinceRecv, cAge,
                   cSeen, cReasm, cDiscPending, cDiscKeys, cFam, serverVars, s2cVars>>

\* the reversed witness only (CommitBeforeAnswer): the client commits to a
\* family of its own choosing in requesting cookie, before any answer
ClientCommitEarly ==
    /\ CommitBeforeAnswer /\ cstate = 1 /\ cFam = "none" /\ Cardinality(RaceFamilies) > 1
    /\ \E f \in RaceFamilies : cFam' = f
    /\ UNCHANGED <<cstate, cAttempt, cHasCookie, cKeys, cEph, cIndex, cSinceSend, cSinceRecv,
                   cAge, cSeen, cReasm, cDiscPending, cDiscKeys, cLostKinds, cReqFams,
                   serverVars, c2sVars, s2cVars>>

\* one copy of the disconnect burst; the burst is never retransmitted
ClientDiscCopy ==
    /\ cDiscPending > 0
    /\ ClientSendPacket([type |-> "disconnect", att |-> cDiscKeys.att, nonce |-> cDiscKeys.nonce])
    /\ cDiscPending' = cDiscPending - 1
    /\ UNCHANGED <<cstate, cAttempt, cHasCookie, cKeys, cEph, cIndex, cSinceSend, cSinceRecv,
                   cAge, cSeen, cReasm, cDiscKeys, raceVars, serverVars, s2cVars>>

(***************************************************************************)
(* The client receives.  Every server packet has a receive step in every   *)
(* state; what the code's state gate or key check refuses is delivered and *)
(* discarded.  No receive sends.                                           *)
(***************************************************************************)
clientRecvUnchanged == <<cAttempt, cSinceSend, cAge, cDiscPending, cDiscKeys, cLostKinds,
                         cReqFams, serverVars, c2sVars>>

\* the cookie gate on family: before the commit a cookie from any family in
\* flight is the first answer; after it only the committed family's cookie
\* is processed (the other family's socket is closed at the commit, and a
\* late answer that was already received fails the source check and is
\* counted, sdk/rocketnet_client_transport.cpp)
CookieFamilyAccepted(p) == p.fam \in InFlight

\* rocketnet_client_process_cookie: states 1 and 2, salt must match; store,
\* 1 -> 2. The client trusts only the server: the salt is in the clear, so
\* the cookie never restarts the receive clock (TRANSPORT.md t11.1)
ClientRecvCookie(p) ==
    /\ p.type = "cookie"
    /\ S2C!Deliver(p)
    /\ IF cstate \in {1, 2} /\ p.att = cAttempt /\ CookieFamilyAccepted(p)
         THEN cHasCookie' = TRUE /\ cstate' = 2 /\ cFam' = p.fam
         ELSE UNCHANGED <<cHasCookie, cstate, cFam>>
    /\ UNCHANGED <<cKeys, cEph, cIndex, cSinceRecv, cSeen, cReasm>>
    /\ UNCHANGED clientRecvUnchanged

\* the pre-fix cookie, only in SpecCookieRefreshes: an accepted cookie also
\* restarts the receive clock. Not a disjunct of Next.
ClientRecvCookieRefreshes(p) ==
    /\ p.type = "cookie"
    /\ S2C!Deliver(p)
    /\ IF cstate \in {1, 2} /\ p.att = cAttempt /\ CookieFamilyAccepted(p)
         THEN cHasCookie' = TRUE /\ cSinceRecv' = 0 /\ cstate' = 2 /\ cFam' = p.fam
         ELSE UNCHANGED <<cHasCookie, cSinceRecv, cstate, cFam>>
    /\ UNCHANGED <<cKeys, cEph, cIndex, cSeen, cReasm>>
    /\ UNCHANGED clientRecvUnchanged

\* rocketnet_client_process_challenge: state 2 only; opens under temporary
\* keys from the carried nonce and this attempt's salt; commit keys and
\* nonce, erase the ephemeral, response built once, 2 -> 3
ClientRecvChallenge(p) ==
    /\ p.type = "challenge"
    /\ S2C!Deliver(p)
    /\ IF cstate = 2 /\ p.att = cAttempt
         THEN /\ cKeys' = KeysOf(p) /\ cEph' = FALSE /\ cSinceRecv' = 0
              /\ cstate' = 3 /\ cSeen' = {}
         ELSE UNCHANGED <<cKeys, cEph, cSinceRecv, cstate, cSeen>>
    /\ UNCHANGED <<cHasCookie, cIndex, cReasm>>
    /\ UNCHANGED <<cFam, clientRecvUnchanged>>

\* rocketnet_client_process_denied: state 2 (temporary keys from the carried
\* nonce) or 3 (the held handshake keys). A denial is
\* rocketnet_client_set_state(CONNECTION_DENIED). A state at or below
\* DISCONNECTED zeroes the keys and the ephemeral
\* (rocketnet_client_connection_reset).
ClientRecvDenied(p) ==
    /\ p.type = "denied"
    /\ S2C!Deliver(p)
    /\ IF \/ cstate = 2 /\ p.att = cAttempt
          \/ cstate = 3 /\ KeysOf(p) = cKeys
         THEN /\ cstate' = -1
              /\ cKeys' = NoKeys
              /\ cEph' = FALSE
         ELSE UNCHANGED <<cstate, cKeys, cEph>>
    /\ UNCHANGED <<cHasCookie, cIndex, cSinceRecv, cSeen, cReasm>>
    /\ UNCHANGED <<cFam, clientRecvUnchanged>>

\* the pre-fix denial, only in SpecDeniedKeepsKeys: state -1, and the keys
\* and the ephemeral stay. Not a disjunct of Next.
ClientRecvDeniedKeepsKeys(p) ==
    /\ p.type = "denied"
    /\ S2C!Deliver(p)
    /\ IF \/ cstate = 2 /\ p.att = cAttempt
          \/ cstate = 3 /\ KeysOf(p) = cKeys
         THEN cstate' = -1
         ELSE UNCHANGED cstate
    /\ UNCHANGED <<cHasCookie, cKeys, cEph, cIndex, cSinceRecv, cSeen, cReasm>>
    /\ UNCHANGED <<cFam, clientRecvUnchanged>>

\* rocketnet_client_open_session_packet then the per-type action: states 3
\* and 4, the session keys, the replay window; an accepted packet refreshes
\* the timeout; a keep-alive in 3 sets the index and connects; a disconnect
\* is state 0 immediately. A payload is accepted only in 4, as netcode.io's
\* client (rocketnet_client_session_state_accepts): the server's first
\* payloads can reach the client ahead of the keep-alive that connects it,
\* by loss or reorder of that keep-alive, and one delivered in 3 let the
\* application reply on a connection that could not yet send. This action
\* leaves the keys; set_state zeroes them, which KeysErasedWhenDown records
SessionStateAccepts(t) == cstate = 4 \/ (cstate = 3 /\ t # "payload")

ClientRecvSession(p) ==
    /\ p.type \in SessionTypes
    /\ S2C!Deliver(p)
    /\ IF SessionStateAccepts(p.type) /\ KeysOf(p) = cKeys /\ p.id \notin cSeen
         THEN /\ cSeen' = cSeen \cup {p.id} /\ cSinceRecv' = 0
              /\ cReasm' = IF p.type = "disconnect" THEN FALSE ELSE cReasm
              /\ CASE p.type = "keepalive"  -> cIndex' = TRUE /\ cstate' = 4
                   [] p.type = "payload"    -> UNCHANGED <<cIndex, cstate>>
                   [] p.type = "disconnect" -> cstate' = 0 /\ UNCHANGED cIndex
         ELSE UNCHANGED <<cSeen, cSinceRecv, cIndex, cstate, cReasm>>
    /\ UNCHANGED <<cHasCookie, cKeys, cEph>>
    /\ UNCHANGED <<cFam, clientRecvUnchanged>>

\* the pre-fix behaviour, kept only as the reversed witness's step: a
\* payload accepted in sending-connection-response (state 3) as well.
\* Not a disjunct of Next; SpecEarlyPayload below adds it.
ClientRecvEarlyPayload(p) ==
    /\ p.type = "payload"
    /\ S2C!Deliver(p)
    /\ cstate = 3 /\ KeysOf(p) = cKeys /\ p.id \notin cSeen
    /\ cSeen' = cSeen \cup {p.id} /\ cSinceRecv' = 0
    /\ UNCHANGED <<cIndex, cstate, cReasm, cHasCookie, cKeys, cEph>>
    /\ UNCHANGED <<cFam, clientRecvUnchanged>>

\* a fragment (type 9), modelled at the receive step itself: the sealed
\* packet's journey is the session packets' journey, so the action is the
\* acceptance, which is the only step that creates client reassembly state.
\* Accepted only in connected (state 4), per STANDARD.md _Fragmentation_.
ClientFragmentReceive ==
    /\ cstate = 4 /\ ~cReasm
    /\ cReasm' = TRUE
    /\ UNCHANGED <<cstate, cAttempt, cHasCookie, cKeys, cEph, cIndex, cSinceSend,
                   cSinceRecv, cAge, cSeen, cDiscPending, cDiscKeys, cLostKinds,
                   raceVars, serverVars, c2sVars, s2cVars>>

\* the pre-fix behaviour, kept only as the reversed witness's step: a
\* fragment accepted in sending-connection-response (state 3) as well.
\* Not a disjunct of Next; SpecEarlyFragment below adds it.
ClientFragmentReceiveEarly ==
    /\ cstate \in {3, 4} /\ ~cReasm
    /\ cReasm' = TRUE
    /\ UNCHANGED <<cstate, cAttempt, cHasCookie, cKeys, cEph, cIndex, cSinceSend,
                   cSinceRecv, cAge, cSeen, cDiscPending, cDiscKeys, cLostKinds,
                   raceVars, serverVars, c2sVars, s2cVars>>

ClientRecv(p) ==
    \/ ClientRecvCookie(p) \/ ClientRecvChallenge(p)
    \/ ClientRecvDenied(p) \/ ClientRecvSession(p)

(***************************************************************************)
(* The server receives.  rocketnet_server_protocol_process_connection_      *)
(* request and the sealed-packet ladder, as far as they answer the client. *)
(***************************************************************************)
serverRecvUnchanged == <<cstate, cAttempt, cHasCookie, cKeys, cEph, cIndex, cSinceSend,
                         cSinceRecv, cAge, cReasm, cDiscPending, cDiscKeys, cLostKinds, raceVars,
                         sOthers, sDiscPending, sDiscKeys, sDiscId>>

\* a request: a connected address is ignored; a half-open mapping answers
\* with its stored challenge, byte-identical, whatever the request carries;
\* no cookie -> a cookie; no slot -> a denied at request, no mapping made;
\* else a mapping at a fresh nonce and its challenge
ServerRecvRequest(p) ==
    /\ p.type = "request"
    /\ C2S!Deliver(p)
    /\ CASE ServerMute \/ (sMap.live /\ sMap.slot) \/ p.fams \cap ReachableFamilies = {} ->
                UNCHANGED <<sMap, s2cVars, sLostKinds, cSeen>>
         [] sMap.live ->
                /\ ServerSendPacket([type |-> "challenge", att |-> sMap.att, nonce |-> sMap.nonce])
                /\ UNCHANGED <<sMap, cSeen>>
         [] ~p.cookie ->
                \* the stateless answer, on a family the request reached
                /\ \E f \in p.fams \cap ReachableFamilies :
                       ServerSendPacket([type |-> "cookie", att |-> p.att, fam |-> f])
                /\ UNCHANGED <<sMap, cSeen>>
         [] FreeSlots = 0 ->
                /\ ServerSendPacket([type |-> "denied", att |-> p.att, nonce |-> 0])
                /\ UNCHANGED <<sMap, cSeen>>
         [] OTHER ->
                \E n \in FreshNonce :
                    /\ sMap' = [NoMap EXCEPT !.live = TRUE, !.att = p.att, !.nonce = n]
                    /\ ServerSendPacket([type |-> "challenge", att |-> p.att, nonce |-> n])
                    /\ UNCHANGED cSeen
    /\ UNCHANGED <<sReasm>>
    /\ UNCHANGED serverRecvUnchanged

\* a response that opens under the mapping's handshake keys: refreshes the
\* mapping; already connected -> ignored; no slot -> denied, mapping kept;
\* else the slot is assigned and the first keep-alive goes out
ServerRecvResponse(p) ==
    /\ p.type = "response"
    /\ C2S!Deliver(p)
    /\ IF ~ServerMute /\ sMap.live /\ KeysOf(p) = KeysOf(sMap)
         THEN CASE sMap.slot ->
                     /\ sMap' = [sMap EXCEPT !.sinceRecv = 0]
                     /\ UNCHANGED <<s2cVars, sLostKinds, cSeen>>
                [] FreeSlots = 0 ->
                     /\ sMap' = [sMap EXCEPT !.sinceRecv = 0]
                     /\ ServerSendPacket([type |-> "denied", att |-> sMap.att, nonce |-> sMap.nonce])
                     /\ UNCHANGED cSeen
                [] OTHER ->
                     \E i \in FreshId :
                        /\ sMap' = [sMap EXCEPT !.slot = TRUE, !.sinceRecv = 0, !.sinceKA = 0,
                                                !.sinceSend = 0, !.acked = FALSE]
                        /\ ServerSendSession("keepalive", i)
         ELSE UNCHANGED <<sMap, s2cVars, sLostKinds, cSeen>>
    /\ UNCHANGED <<sReasm>>
    /\ UNCHANGED serverRecvUnchanged

\* a client session packet that opens for a connected mapping: a keep-alive
\* refreshes the slot and its ack block acknowledges a keep-alive (the
\* latch); a disconnect releases the slot and destroys the mapping
ServerRecvSession(p) ==
    /\ p.type \in {"keepalive", "disconnect"}
    /\ C2S!Deliver(p)
    /\ IF ~ServerMute /\ sMap.live /\ sMap.slot /\ KeysOf(p) = KeysOf(sMap)
         THEN /\ sMap' = IF p.type = "keepalive"
                        THEN [sMap EXCEPT !.sinceRecv = 0, !.acked = TRUE]
                        ELSE NoMap
              /\ sReasm' = IF p.type = "keepalive" THEN sReasm ELSE FALSE
         ELSE UNCHANGED <<sMap, sReasm>>
    /\ UNCHANGED <<s2cVars, sLostKinds, cSeen>>
    /\ UNCHANGED serverRecvUnchanged

\* a fragment (type 9), modelled at the receive step itself, as on the
\* client side: acceptance for a mapping holding a slot is the only step
\* that creates server reassembly state for that slot (STANDARD.md
\* _Fragmentation_)
ServerFragmentReceive ==
    /\ sMap.live /\ sMap.slot /\ ~sReasm
    /\ sReasm' = TRUE
    /\ UNCHANGED <<clientVars, sMap, sOthers, sDiscPending, sDiscKeys, sDiscId,
                   sLostKinds, c2sVars, s2cVars>>

ServerRecv(p) == ServerRecvRequest(p) \/ ServerRecvResponse(p) \/ ServerRecvSession(p)

(***************************************************************************)
(* The server's own sends.                                                 *)
(***************************************************************************)
serverSendUnchanged == <<cstate, cAttempt, cHasCookie, cKeys, cEph, cIndex, cSinceSend,
                         cSinceRecv, cAge, cReasm, cDiscPending, cDiscKeys, cLostKinds, raceVars, c2sVars>>

\* rocketnet_server_protocol_write_keep_alive_if_due: paced off the
\* keep-alive's own clock until one is acknowledged, off the last send of
\* anything after; the witnesses change which clock and whether it repeats
ServerKeepAlive ==
    /\ ~ServerMute /\ sMap.live /\ sMap.slot
    /\ RepeatKeepAlive \/ sMap.acked
    /\ LET clock == IF KeepAliveClock = "anysend" \/ sMap.acked THEN sMap.sinceSend ELSE sMap.sinceKA
       IN clock = SendInterval
    /\ \E i \in FreshId :
        /\ ServerSendSession("keepalive", i)
        /\ sMap' = [sMap EXCEPT !.sinceKA = 0, !.sinceSend = 0]
    /\ UNCHANGED <<sOthers, sReasm, sDiscPending, sDiscKeys, sDiscId>>
    /\ UNCHANGED serverSendUnchanged

\* the server application sends a payload at most once per send interval
\* (a model-size bound: the code places no rate on payloads; a denser stream
\* is the streaming choice in Tick); it stamps the last send of anything,
\* never the keep-alive's clock
ServerPayload ==
    /\ ServerPayloads /\ ~ServerMute /\ sMap.live /\ sMap.slot
    /\ sMap.sinceSend = SendInterval
    /\ \E i \in FreshId :
        /\ ServerSendSession("payload", i)
        /\ sMap' = [sMap EXCEPT !.sinceSend = 0]
    /\ UNCHANGED <<sOthers, sReasm, sDiscPending, sDiscKeys, sDiscId>>
    /\ UNCHANGED serverSendUnchanged

\* rocketnet_server_protocol_disconnect_client: the burst, then the slot
\* released and the mapping destroyed at once
ServerAppDisconnect ==
    /\ Disconnects /\ ~ServerMute /\ sMap.live /\ sMap.slot /\ sDiscPending = 0
    /\ \E i \in FreshId :
        /\ sDiscKeys' = KeysOf(sMap) /\ sDiscId' = i /\ sDiscPending' = DisconnectCopies
        /\ cSeen' = cSeen \ {i}
    /\ sMap' = NoMap /\ sReasm' = FALSE
    /\ UNCHANGED <<sOthers, s2cVars, sLostKinds>>
    /\ UNCHANGED serverSendUnchanged

ServerDiscCopy ==
    /\ sDiscPending > 0
    /\ ServerSendPacket([type |-> "disconnect", att |-> sDiscKeys.att, nonce |-> sDiscKeys.nonce, id |-> sDiscId])
    /\ sDiscPending' = sDiscPending - 1
    /\ UNCHANGED <<sMap, sOthers, sReasm, sDiscKeys, sDiscId, cSeen>>
    /\ UNCHANGED serverSendUnchanged

\* the server loses the mapping without a word (power, restart)
ServerCrash ==
    /\ Disconnects /\ sMap.live
    /\ sMap' = NoMap /\ sReasm' = FALSE
    /\ UNCHANGED <<sOthers, sDiscPending, sDiscKeys, sDiscId, s2cVars, sLostKinds, cSeen>>
    /\ UNCHANGED serverSendUnchanged

\* another client takes or releases a slot
OtherJoins ==
    /\ OthersMayJoin /\ FreeSlots > 0
    /\ sOthers' = sOthers + 1
    /\ UNCHANGED <<sMap, sReasm, sDiscPending, sDiscKeys, sDiscId, s2cVars, sLostKinds, cSeen>>
    /\ UNCHANGED serverSendUnchanged

OtherLeaves ==
    /\ sOthers > 0
    /\ sOthers' = sOthers - 1
    /\ UNCHANGED <<sMap, sReasm, sDiscPending, sDiscKeys, sDiscId, s2cVars, sLostKinds, cSeen>>
    /\ UNCHANGED serverSendUnchanged

(***************************************************************************)
(* Time passes on both ends and on both wires.                             *)
(* rocketnet_client_check_deadlines: in 1..3 the total deadline first, then *)
(* in 1..4 the idle timeout to -(state+1). A deadline or a timeout zeroes   *)
(* the ephemeral; the session keys stay (KeysErasedWhenDown).               *)
(* rocketnet_server_protocol_update: a half-open mapping dies at its        *)
(* handshake deadline, an established one at the idle timeout.              *)
(***************************************************************************)
Tick ==
    /\ C2S!Age /\ S2C!Age
    /\ LET age1  == Sat(cAge, Deadline)
           recv1 == Sat(cSinceRecv, Timeout)
           dead  == cstate \in 1..3 /\ Deadline > 0 /\ age1 = Deadline
           timed == cstate \in 1..4 /\ Timeout > 0 /\ recv1 = Timeout
       IN /\ cReasm' = IF dead \/ timed THEN FALSE ELSE cReasm
          /\ cEph' = IF dead \/ timed THEN FALSE ELSE cEph
          /\ cAge' = age1 /\ cSinceRecv' = recv1
          /\ cSinceSend' = Sat(cSinceSend, SendInterval)
          /\ cstate' = IF dead THEN -7 ELSE IF timed THEN -(cstate + 1) ELSE cstate
    /\ IF sMap.live
         THEN \E streaming \in BOOLEAN :
              LET sage1  == Sat(sMap.age, ServerDeadline)
                  srecv1 == Sat(sMap.sinceRecv, Timeout)
                  expired == ~sMap.slot /\ ServerDeadline > 0 /\ sage1 = ServerDeadline
                  stimed  == sMap.slot /\ Timeout > 0 /\ srecv1 = Timeout
                  \* the server application sent this slot something during the
                  \* interval (payloads at any rate above the send rate): the
                  \* last-send clock is fresh at the next keep-alive check, the
                  \* keep-alive's own clock is not.  The packets themselves are
                  \* ServerPayload; this is their effect on the clock the #90
                  \* regression read.
                  ssend1 == IF ServerPayloads /\ sMap.slot /\ streaming
                              THEN 0 ELSE Sat(sMap.sinceSend, SendInterval)
              IN /\ sMap' = IF expired \/ stimed THEN NoMap
                         ELSE [sMap EXCEPT !.age = sage1, !.sinceRecv = srecv1,
                                           !.sinceKA = Sat(sMap.sinceKA, SendInterval),
                                           !.sinceSend = ssend1]
                 /\ sReasm' = IF expired \/ stimed THEN FALSE ELSE sReasm
         ELSE UNCHANGED <<sMap, sReasm>>
    /\ UNCHANGED <<cAttempt, cHasCookie, cKeys, cIndex, cSeen, cDiscPending, cDiscKeys,
                   cLostKinds, raceVars, sOthers, sDiscPending, sDiscKeys, sDiscId, sLostKinds>>

C2SEnvironment == C2S!Environment /\ UNCHANGED <<clientVars, serverVars, s2cVars>>
S2CEnvironment == S2C!Environment /\ UNCHANGED <<clientVars, serverVars, c2sVars>>

Next ==
    \/ AppConnect \/ AppDisconnect
    \/ ClientSend \/ ClientDiscCopy \/ ClientFragmentReceive \/ ServerFragmentReceive
    \/ ClientCommitEarly
    \/ \E p \in ServerPackets : ClientRecv(p)
    \/ \E p \in ClientPackets : ServerRecv(p)
    \/ ServerKeepAlive \/ ServerPayload \/ ServerAppDisconnect \/ ServerDiscCopy \/ ServerCrash
    \/ OtherJoins \/ OtherLeaves
    \/ Tick
    \/ C2SEnvironment \/ S2CEnvironment

(***************************************************************************)
(* Fairness: time passes, the scheduled sends go out, the application      *)
(* connects, the wires deliver what reaches their head, and a packet       *)
(* re-sent often enough eventually enters the wire.                        *)
(*                                                                         *)
(* The last two are Link!WireFair and Link!EventualDelivery restated at    *)
(* this model's size.  WireFair's strong fairness per packet value (forty  *)
(* values over two links at the safety instance) and EventualDelivery's    *)
(* per-value implication exhaust TLC: the implication makes the tableau    *)
(* degenerate ("temporal formula is a tautology") and per-value conditions *)
(* run the liveness check for hours.  Two restatements, both on a FIFO     *)
(* link (Reordering = FALSE, MaxDelay = 0, Duplication = FALSE; with       *)
(* reordering or latency on, Link's Reorder or Delay can hold a copy from  *)
(* the head forever, and with duplication on the path can keep a wire at   *)
(* Capacity exactly when the sender must send, where Link blocks the send  *)
(* rather than losing it; TLC produced both, README-ClientConnection.md):  *)
(*                                                                         *)
(*   DeliveryFairHead: the head of each wire is delivered (strong          *)
(*   fairness on the two receive-the-head actions).  Enough for the        *)
(*   deterministic patterns, where loss happens at Send and every copy on  *)
(*   the wire reaches the head.  Seconds per run.                          *)
(*                                                                         *)
(*   DeliveryFairTyped: that, and Link's EventualDelivery per packet      *)
(*   type: a type that enters the wire without bound is delivered without  *)
(*   bound.  Needed under "arbitrary", where Drop removes copies in        *)
(*   flight and TLC otherwise finds a path that drops every keep-alive     *)
(*   (behind a payload, so never at the head) and passes every payload.    *)
(*   Minutes per run: TLC's tableau grows with each implication.          *)
(*                                                                         *)
(* Re-sending is strongly fair on the sender in both: the two timer-driven *)
(* sends, when enabled with the pattern letting the send through,          *)
(* eventually go through.  The sender-side form is what a deterministic    *)
(* pattern can defeat (every re-send landing on a lost index) and what the *)
(* real path does not do.  The reversed witnesses show the fairness is not *)
(* vacuous.  The server application is under no obligation to send         *)
(* payloads or to disconnect.                                              *)
(***************************************************************************)
ServerTypes == {"cookie", "challenge", "denied"} \cup SessionTypes
ClientTypes == {"request", "response", "keepalive", "disconnect"}

ClientRecvHead == s2cWire # <<>> /\ ClientRecv(Head(s2cWire).pkt)
ServerRecvHead == c2sWire # <<>> /\ ServerRecv(Head(c2sWire).pkt)

ClientRecvType(t) == ClientRecvHead /\ Head(s2cWire).pkt.type = t
ServerRecvType(t) == ServerRecvHead /\ Head(c2sWire).pkt.type = t

ClientSendThrough      == ClientSend /\ c2sWire' # c2sWire
ServerKeepAliveThrough == ServerKeepAlive /\ s2cWire' # s2cWire

SendFair ==
    /\ WF_vars(Tick) /\ WF_vars(AppConnect)
    /\ WF_vars(ClientSend) /\ SF_vars(ClientSendThrough)
    /\ WF_vars(ServerKeepAlive) /\ SF_vars(ServerKeepAliveThrough)
    /\ WF_vars(ClientDiscCopy) /\ WF_vars(ServerDiscCopy)

\* a packet of type t enters the wire
S2CSendType(t) == \E p \in ServerPackets : p.type = t /\ S2C!Send(p) /\ s2cWire' # s2cWire
C2SSendType(t) == \E p \in ClientPackets : p.type = t /\ C2S!Send(p) /\ c2sWire' # c2sWire

DeliveryFairHead  == SF_vars(ClientRecvHead) /\ SF_vars(ServerRecvHead)

\* Link's EventualDelivery per type: a type sent without bound is delivered
\* without bound.  Under "arbitrary" the head condition alone lets Drop
\* target one type whenever it is behind another (TLC showed the
\* keep-alive dropped behind a payload every time).
DeliveryFairTyped ==
    /\ DeliveryFairHead
    /\ \A t \in ServerTypes : ([]<> <<S2CSendType(t)>>_vars) => ([]<> <<ClientRecvType(t)>>_vars)
    /\ \A t \in ClientTypes : ([]<> <<C2SSendType(t)>>_vars) => ([]<> <<ServerRecvType(t)>>_vars)

Fairness      == SendFair /\ DeliveryFairHead
FairnessTyped == SendFair /\ DeliveryFairTyped

Spec      == Init /\ [][Next]_vars /\ Fairness
SpecTyped == Init /\ [][Next]_vars /\ FairnessTyped

\* the reversed witness's spec: Next plus the pre-fix early fragment
SpecEarlyFragment == Init /\ [][Next \/ ClientFragmentReceiveEarly]_vars /\ Fairness

\* the reversed witness's spec: Next plus the pre-fix early payload
SpecEarlyPayload ==
    Init /\ [][Next \/ (\E p \in SessionPackets : ClientRecvEarlyPayload(p))]_vars /\ Fairness

\* the reversed witness's spec: Next plus the pre-fix denial that keeps keys
SpecDeniedKeepsKeys ==
    Init /\ [][Next \/ (\E p \in ServerPackets : ClientRecvDeniedKeepsKeys(p))]_vars /\ Fairness

\* the reversed witness's spec: Next plus the pre-fix cookie that restarts
\* the receive clock
SpecCookieRefreshes ==
    Init /\ [][Next \/ (\E p \in ServerPackets : ClientRecvCookieRefreshes(p))]_vars /\ Fairness

\* the reversed witness's spec: Next plus the pre-fix retry that keeps the
\* earlier attempt's cookie and replay window
SpecRetryKeepsAttempt == Init /\ [][Next \/ AppConnectKeepsAttempt]_vars /\ Fairness

(***************************************************************************)
(* Safety.                                                                 *)
(***************************************************************************)
TypeOK ==
    /\ cstate \in -7..4 /\ cAttempt \in 0..MaxAttempts
    /\ cHasCookie \in BOOLEAN /\ cKeys \in Keys \cup {NoKeys}
    /\ cEph \in BOOLEAN /\ cIndex \in BOOLEAN
    /\ cSinceSend \in 0..SendInterval /\ cSinceRecv \in 0..Timeout /\ cAge \in 0..Deadline
    /\ cSeen \subseteq Id
    /\ cDiscPending \in 0..DisconnectCopies /\ cDiscKeys \in Keys \cup {NoKeys}
    /\ cReasm \in BOOLEAN /\ sReasm \in BOOLEAN
    /\ sMap \in Mapping /\ sOthers \in 0..MaxSlots
    /\ sDiscPending \in 0..DisconnectCopies /\ sDiscKeys \in Keys \cup {NoKeys}
    /\ sDiscId \in Id \cup {0}
    /\ cFam \in Families \cup {"none"} /\ cReqFams \subseteq Families
    /\ C2S!TypeOK /\ S2C!TypeOK

\* never connected without a completed handshake: connected holds a cookie,
\* this attempt's committed keys, the index from a keep-alive, and no
\* ephemeral
ConnectedMeansHandshake ==
    cstate = 4 => /\ cHasCookie /\ cKeys # NoKeys /\ cKeys.att = cAttempt
                  /\ cIndex /\ ~cEph

\* reassembly state changes only while connected: client reassembly exists
\* only in state 4, server reassembly only for a mapping holding its slot
\* (STANDARD.md _Fragmentation_; sdk/rocketnet_client_protocol.cpp,
\* sdk/rocketnet_server_protocol.cpp)
FragmentOnlyWhenConnected ==
    /\ (cReasm => cstate = 4)
    /\ (sReasm => sMap.live /\ sMap.slot)

\* the machine steps forward one rung or fails; a positive state is entered
\* from the rung below or, for 1, from a connect (S:2042-2045)
StepsForwardOrFails ==
    [][cstate' # cstate =>
         \/ cstate >= 1 /\ cstate' = cstate + 1
         \/ cstate <= 0 /\ cstate' = 1 /\ cAttempt' = cAttempt + 1
         \/ cstate' <= 0]_vars

\* a disconnect is terminal until the application reconnects: the only way
\* out of a non-positive state is a fresh attempt
DisconnectIsTerminal ==
    [][(cstate <= 0 /\ cstate' > 0) => (cstate' = 1 /\ cAttempt' = cAttempt + 1)]_vars

\* a terminal state leaves only by connect, to 1, or by disconnect, to 0.
\* rocketnet_client_protocol_disconnect sets DISCONNECTED from any state
\* that is not already DISCONNECTED (sdk/rocketnet.h).
TerminalAbsorbing ==
    [][(cstate < 0 /\ cstate' # cstate) => cstate' \in {0, 1}]_vars

\* a connect from a failure state (-7..-1) is a fresh attempt: a new
\* attempt index, which is what gives it a fresh salt, a fresh ephemeral and
\* fresh keys, and nothing of the earlier attempt carried: no keys, no
\* cookie, no client index, no reassembly, an empty replay window and fresh
\* timers (rocketnet_client_protocol_connect runs
\* rocketnet_client_connection_reset first; rocketnet_client_connect accepts
\* every state at or below DISCONNECTED)
FreshAttemptFromFailure ==
    [][(cstate < 0 /\ cstate' = 1) =>
         /\ cAttempt' = cAttempt + 1 /\ cKeys' = NoKeys /\ ~cHasCookie' /\ cEph'
         /\ ~cIndex' /\ ~cReasm' /\ cSeen' = {} /\ cSinceRecv' = 0 /\ cAge' = 0
         /\ cFam' = "none" /\ cReqFams' = {}]_vars

\* a timeout from waiting state N lands in -(N+1) (S:2074-2077)
TimeoutPairing ==
    [][(cstate \in 1..4 /\ cstate' \in {-2, -3, -4, -5}) => cstate' = -(cstate + 1)]_vars

\* the client trusts only the server: within an attempt, before the
\* challenge commits keys (states 1 and 2), nothing restarts the receive
\* clock, so no packet anyone can forge keeps an attempt alive. Only the
\* challenge, which opens under the attempt's keys, and session packets
\* after it reset it (TRANSPORT.md t11 rule 8, t11.1)
OnlyAuthenticatedRefreshes ==
    [][(cstate \in {1, 2} /\ cstate' \in {1, 2} /\ cAttempt' = cAttempt) => cSinceRecv' >= cSinceRecv]_vars

\* the idle timeout and the total deadline fire when they are due
TimeoutHonoured  == (Timeout > 0 /\ cstate \in 1..4) => cSinceRecv < Timeout
DeadlineHonoured == (Deadline > 0 /\ cstate \in 1..3) => cAge < Deadline

\* at most one live session: the client accepts session packets under at
\* most one key pair, which is this attempt's; and a slot the server holds
\* for this attempt is under the keys the client holds, or the client has
\* already thrown its keys away
LiveSessions == IF cstate \in {3, 4} THEN {cKeys} ELSE {}

AtMostOneLiveSession ==
    /\ Cardinality(LiveSessions) <= 1
    /\ cstate \in {3, 4} => cKeys.att = cAttempt
    /\ (sMap.live /\ sMap.slot /\ sMap.att = cAttempt) => cKeys \in {KeysOf(sMap), NoKeys}

\* no packet is accepted outside the live session's keys: an accepted
\* session packet (its id enters the seen-set) opened under the held keys
\* in state 3 or 4
AcceptsSession(p) == S2C!Deliver(p) /\ p.id \notin cSeen /\ p.id \in cSeen'

AcceptedUnderLiveKeys ==
    [][\A p \in SessionPackets :
         AcceptsSession(p) => (cstate \in {3, 4} /\ KeysOf(p) = cKeys)]_vars

\* the application is handed a payload only while connected: a payload is
\* accepted (its id enters the seen-set) only in state 4, so the reply the
\* application sends to it can leave
PayloadOnlyWhenConnected ==
    [][\A p \in SessionPackets :
         (p.type = "payload" /\ AcceptsSession(p)) => cstate = 4]_vars

\* no receive sends (S:2178-2182)
NoSendOnArrival ==
    [][(\E p \in ServerPackets : S2C!Deliver(p)) => c2sSent' = c2sSent]_vars

\* the ephemeral secret is erased outside states 1 and 2. A denial and a
\* deadline or timeout zero it, as rocketnet_client_set_state does.
EphemeralErasedOutsideHandshake == cstate \notin {1, 2} => ~cEph

\* the session keys are gone once the attempt is over. A denial zeroes
\* them. A timeout and a received disconnect leave them in this model; that
\* is the observation, see README-ClientConnection.md
KeysErasedWhenDown == cstate <= 0 => cKeys = NoKeys

\* the family race: a request carrying a cookie goes to one family only,
\* never to two in one attempt (the cookie commits the attempt first)
OneRequestFamily == Cardinality(cReqFams) <= 1

\* the commit follows an answer: only a family that can answer (one the
\* route reaches) is ever committed to.  CommitBeforeAnswer breaks it.
CommitsOnAnswer == cFam # "none" => cFam \in ReachableFamilies

(***************************************************************************)
(* Liveness.                                                               *)
(***************************************************************************)

\* with a responsive server and fair loss, an attempt reaches connected
Connects == (cstate = 1) ~> (cstate = 4)

\* every attempt leaves the handshake range
AttemptResolves == (cstate \in 1..3) ~> (cstate \notin 1..3)

\* with a silent server the client reaches cookie request timed out
SilentServerTimesOut == (cstate = 1) ~> (cstate = -2)

\* a connected client whose server has gone silent reaches connection timed
\* out, unless its own application disconnects first
CrashedServerTimesOut ==
    (cstate = 4 /\ ~sMap.live /\ sDiscPending = 0) ~> (cstate \in {-5, 0})

\* neither side is left waiting for a packet nobody will send again: the
\* server's half-open mapping is answered or dies, and an unacknowledged
\* slot is acknowledged or released
ServerNotStranded ==
    /\ (sMap.live /\ ~sMap.slot) ~> (~sMap.live \/ sMap.slot)
    /\ (sMap.live /\ sMap.slot /\ ~sMap.acked) ~> (~sMap.live \/ sMap.acked)

=============================================================================
