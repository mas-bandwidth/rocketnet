-------------------------- MODULE ServerConnection --------------------------
(***************************************************************************)
(* rocketnet: the server's slot lifecycle and encryption mapping.          *)
(*                                                                         *)
(* The server of sdk/rocketnet_server_protocol.cpp (the request ladder,    *)
(* the response, the sealed session types, the sweep, the keep-alive and   *)
(* the application disconnect), with its two tables, the encryption map   *)
(* keyed by source address (sdk/rocketnet_encryption_map.cpp) and the      *)
(* client slots (sdk/rocketnet_client_slots.cpp), and the two token tables *)
(* as far as they decide admission (sdk/rocketnet_token_tables.cpp).  It   *)
(* is the server-side counterpart of tla/ClientConnection.tla: the same    *)
(* packet names and the same phases, with the clients as the environment   *)
(* this time, modelled only as far as they drive the server.  A client     *)
(* here is a ClientConnection client with its deadlines folded into two    *)
(* outside events (the application disconnects, or the host vanishes), and *)
(* a ClientConnection packet is one of these packets with the address      *)
(* fields projected away.                                                  *)
(*                                                                         *)
(* Both links are instances of the shared lossy link (tla/Link.tla): C2S   *)
(* carries every client's packets to the server, S2C the server's packets  *)
(* to every address.  A packet carries its source address (from) or its    *)
(* destination (to); the link never reads either.  The server keys          *)
(* everything on from (STANDARD.md, The client address is the connection   *)
(* identity); a client receives only what is addressed to the address it   *)
(* holds now, and a packet to an address nobody holds is delivered to the  *)
(* void (received and discarded, so the link's fairness is never vacuous). *)
(*                                                                         *)
(* Mappings.  One record per address: live, established (a slot held),    *)
(* the keys as [salt, nonce] (the attempt's salt and the mapping's fresh   *)
(* session nonce, the two values every key derives from, so "opens under   *)
(* the mapping's keys" is equality on the pair), the token, the slot       *)
(* index, the handshake age (fixed deadline, never refreshed), the idle    *)
(* age (refreshed by every opened packet), the keep-alive clock and the    *)
(* keep-alive-acknowledged latch.  At most MapCap mappings are live (the   *)
(* code's table is 2 x max_clients, sdk/rocketnet_server_transport.cpp).    *)
(* Slots: used, the address of the mapping it is bound to, the token (the  *)
(* client id is a function of the token), confirmed.  The lowest free slot *)
(* is assigned, as rocketnet_client_slots_assign scans.                     *)
(*                                                                         *)
(* Tokens.  A token is a number; its client id is IdOf(t).  A client       *)
(* presents its own token (TokenChoice = "own") or any token the           *)
(* environment holds ("any", so two clients may share one).  The used-token *)
(* history binds a token to the first address whose response opens under    *)
(* its keys (bound); it is checked at later requests. Tokens are unexpired  *)
(* throughout a run unless listed in ExpiredTokens, an abstraction of the   *)
(* early expiry gate before cryptographic work.                             *)
(*                                                                         *)
(* Time.  Tick is one send interval on every clock; every timer is a       *)
(* saturating counter.  HandshakeDeadline = 0 and Timeout = 0 are off      *)
(* (the model asking what the protocol alone does).                        *)
(*                                                                         *)
(* Loss.  Both links take a Link pattern; KindLoss = "firstkind" loses the *)
(* first send of each packet type at each sender before the link.          *)
(*                                                                         *)
(* Witness knobs, each reproducing one known-bad server (the code is the   *)
(* first value): RepeatKeepAlive (TRUE: the establishing keep-alive is     *)
(* re-sent on its own clock until acknowledged; FALSE: sent once at the    *)
(* response, the hang class the request named); SweepRemovesMapping        *)
(* (TRUE: a timed-out slot takes its mapping with it; FALSE: the slot is   *)
(* released and the mapping leaks); HoldUntilExpire (TRUE: the reversed    *)
(* lifetime-hold rule, using the history as its finite ghost; FALSE: code); *)
(* ClientIdCheckAtRequest and ClientIdCheckAtResponse (TRUE: an already    *)
(* connected client id refuses at that rung; FALSE: skipped);              *)
(* AddressIsIdentity (TRUE: a sealed packet is looked up by its source     *)
(* address; FALSE: by whichever live mapping its keys open, migration       *)
(* without validation).                                                    *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    NumClients,        \* clients in the environment
    NumAddrs,          \* addresses they may hold; >= NumClients
    NumTokens,         \* tokens they may present
    MaxSlots,          \* the server's client slots (max_clients)
    MapCap,            \* the encryption map's capacity (2 x max_clients in the code)
    HeldCapacity,      \* only used by the reversed lifetime-hold witness
    MaxAttempts,       \* connects per client
    MaxNonces,         \* the pool of session nonces
    MaxMoves,          \* address changes per client
    SendInterval,      \* ticks between scheduled sends, both ends; >= 1
    Timeout,           \* the idle timeout in ticks, either state; 0 = off
    HandshakeDeadline, \* the half-open mapping's fixed deadline in ticks; 0 = off
    Disconnects,       \* BOOLEAN: may either application disconnect, may a client vanish
    DisconnectCopies,  \* the redundant disconnect burst (10 in the code)
    SameId,            \* BOOLEAN: every token carries one client id
    TokenChoice,       \* "own": client c presents token c; "any": any token, so clients may share one
    History,           \* "kept" (the history binds) or "none" (flushed)
    KindLoss,          \* "none" or "firstkind"
    RepeatKeepAlive, SweepRemovesMapping, HoldUntilExpire,
    ClientIdCheckAtRequest, ClientIdCheckAtResponse, AddressIsIdentity,
    ExpiredTokens,
    C2SPattern, S2CPattern,                         \* Link patterns
    Capacity, MaxDelay, Duplication, Reordering,    \* Link parameters
    BurstStart, BurstLen

ASSUME /\ NumClients \in Nat \ {0} /\ NumAddrs \in Nat /\ NumAddrs >= NumClients
       /\ NumTokens \in Nat \ {0} /\ MaxSlots \in Nat \ {0}
       /\ MapCap \in Nat \ {0} /\ HeldCapacity \in Nat \ {0}
       /\ MaxAttempts \in Nat \ {0} /\ MaxNonces \in Nat \ {0} /\ MaxMoves \in Nat
       /\ SendInterval \in Nat \ {0} /\ Timeout \in Nat /\ HandshakeDeadline \in Nat
       /\ Disconnects \in BOOLEAN /\ DisconnectCopies \in Nat \ {0}
       /\ SameId \in BOOLEAN /\ History \in {"kept", "none"}
       /\ TokenChoice \in {"own", "any"} /\ (TokenChoice = "own" => NumTokens >= NumClients)
       /\ KindLoss \in {"none", "firstkind"}
       /\ RepeatKeepAlive \in BOOLEAN /\ SweepRemovesMapping \in BOOLEAN
       /\ HoldUntilExpire \in BOOLEAN
       /\ ClientIdCheckAtRequest \in BOOLEAN /\ ClientIdCheckAtResponse \in BOOLEAN
       /\ AddressIsIdentity \in BOOLEAN /\ ExpiredTokens \subseteq 1..NumTokens

(***************************************************************************)
(* Address families.  A connect token names one or two server addresses,   *)
(* at most one per family (rocketnet_write_production_connect_token,       *)
(* sdk/rocketnet_connect_token.cpp).  The server config names one family   *)
(* or both on one port (ConfiguredFamilies).  ServedFamilies is the set    *)
(* the server actually serves: ConfiguredFamilies, unless                  *)
(* ServeFirstFamilyOnly, the reversed witness in which the server serves   *)
(* only the first configured family (ipv4 before ipv6, the create call's   *)
(* address when the config names both).  ServedFamily remains the single   *)
(* family the older cfgs override; ConfiguredFamilies follows it unless a  *)
(* cfg replaces ConfiguredFamilies.  Each client has its own host families *)
(* (ClientHost(c)); the default is HostFamilies for every client.  The     *)
(* client selects the IPv6 address when the token names one and that       *)
(* client's host has IPv6, or the token names no IPv4 address, else the    *)
(* IPv4 address (rocketnet_client_protocol_pick_server_address,            *)
(* sdk/rocketnet_client_transport.cpp); a selected family the host lacks   *)
(* refuses the create.  A request sent on a family the server does not     *)
(* serve reaches no server (ReachesServer: PickFamily(c) \in               *)
(* ServedFamilies).  The server accepts a token when the token's address   *)
(* of the family the request arrived on is one it serves                   *)
(* (AddressMatch = "either", rocketnet_server_token_address_is_advertised, *)
(* sdk/rocketnet_server_protocol.cpp).  It does not consult the token's    *)
(* other-family address.  The reversed witness "ipv6only" checks only the  *)
(* IPv6 address whenever its flag is set.  These are definitions, so every *)
(* run keeps its cfg; the family runs replace them (<- in the cfg).  The   *)
(* defaults reach the server, as every other run assumes.                  *)
(***************************************************************************)
TokenFamilies == {"ipv4"}
HostFamilies  == {"ipv4", "ipv6"}
ServedFamily  == "ipv4"
AddressMatch  == "either"
ClientHost(c) == IF c \in Nat THEN HostFamilies ELSE HostFamilies
ConfiguredFamilies == {ServedFamily}
ServeFirstFamilyOnly == FALSE
FamilyRank(f) == IF f = "ipv4" THEN 1 ELSE 2
FirstConfiguredFamily ==
    CHOOSE f \in ConfiguredFamilies :
        \A g \in ConfiguredFamilies : FamilyRank(f) <= FamilyRank(g)
ServedFamilies ==
    IF ServeFirstFamilyOnly THEN {FirstConfiguredFamily} ELSE ConfiguredFamilies

PickFamily(c) ==
    IF "ipv6" \in TokenFamilies /\ ("ipv6" \in ClientHost(c) \/ "ipv4" \notin TokenFamilies)
      THEN "ipv6" ELSE "ipv4"
CreateSucceeds(c) == PickFamily(c) \in ClientHost(c)
ReachesServer(c)  == PickFamily(c) \in ServedFamilies
TokenAddressAdvertised(arrival, tokFams) ==
    CASE AddressMatch = "either" -> arrival \in tokFams /\ arrival \in ServedFamilies
      [] "ipv6" \in tokFams     -> ServedFamilies = {"ipv6"}
      [] OTHER                  -> ServedFamilies = {"ipv4"}

Clients == 1..NumClients
Addrs   == 1..NumAddrs
Tokens  == 1..NumTokens
Slots   == 1..MaxSlots
Att     == 1..MaxAttempts
Nonce   == 1..MaxNonces

\* the connection salt: 64 random bits in the code, so no two attempts of
\* any two clients ever share one; here the pair <<client, attempt>>.  The
\* first draft keyed it on the attempt number alone and TLC found two
\* clients sharing a token and an attempt number opening each other's
\* challenge (README-ServerConnection.md, model errors).
Salts   == Clients \X Att
NoSalt  == <<0, 0>>

IdOf(t) == IF SameId THEN 1 ELSE t

NoKeys == [salt |-> NoSalt, nonce |-> 0]
Keys   == [salt : Salts, nonce : Nonce]

(***************************************************************************)
(* The packets.  Types are the standard's names, as in ClientConnection.   *)
(* The client's standalone ack and both ends' payloads are not here:       *)
(* neither moves a slot or a mapping (a payload confirms a slot as a       *)
(* keep-alive does).                                                       *)
(***************************************************************************)
ClientPackets ==
    [type : {"request"}, from : Addrs, salt : Salts, cookie : BOOLEAN, tok : Tokens]
    \cup [type : {"response", "keepalive", "disconnect"}, from : Addrs, salt : Salts, nonce : Nonce]

ClientSealed == [type : {"response", "keepalive", "disconnect"}, from : Addrs, salt : Salts, nonce : Nonce]

ServerPackets ==
    [type : {"cookie"}, to : Addrs, salt : Salts]
    \cup [type : {"challenge"}, to : Addrs, salt : Salts, nonce : Nonce]
    \cup [type : {"denied"}, to : Addrs, salt : Salts, nonce : Nonce \cup {0}]
    \cup [type : {"keepalive", "disconnect"}, to : Addrs, salt : Salts, nonce : Nonce]

VARIABLES
    \* the clients (the environment), one value per client
    cstate,        \* -1 denied, 0 disconnected, 1 requesting cookie, 2 sending request,
                   \* 3 sending response, 4 connected
    cAtt,          \* the attempt number: the salt, fresh per connect
    cTok,          \* the token this attempt presents
    cAddr,         \* the address the client holds now
    cCookieAddr,   \* the address the held cookie was issued to (0: none); the cookie MAC covers the address
    cKeys,         \* the committed session nonce (0 before the challenge); the keys are [cAtt, cKeys]
    cMoves,        \* address changes so far
    cSinceSend,    \* ticks since the last scheduled send, saturating at SendInterval
    cDiscPending,  \* disconnect copies still to send after an application disconnect
    cDiscPkt,      \* the copy being sent
    cLostKinds,    \* KindLoss bookkeeping
    \* the server (struct rocketnet_server_protocol_t and its tables)
    smap,          \* [Addrs -> Mapping]: the encryption map
    slot,          \* [Slots -> SlotRec]: the client slots
    bound,         \* [Tokens -> Addrs \cup {0}]: the used-token history
    churnDenied,   \* ghost: a fresh, valid token was refused while a slot was free
    sDiscPending,  \* disconnect copies still to send after a server disconnect
    sDiscPkt,
    sLostKinds,
    \* the links
    c2sWire, c2sSent, c2sPulse,
    s2cWire, s2cSent, s2cPulse

clientVars == <<cstate, cAtt, cTok, cAddr, cCookieAddr, cKeys, cMoves, cSinceSend,
                cDiscPending, cDiscPkt, cLostKinds>>
serverVars == <<smap, slot, bound, churnDenied, sDiscPending, sDiscPkt, sLostKinds>>
c2sVars    == <<c2sWire, c2sSent, c2sPulse>>
s2cVars    == <<s2cWire, s2cSent, s2cPulse>>
vars       == <<clientVars, serverVars, c2sVars, s2cVars>>

C2S == INSTANCE Link WITH wire <- c2sWire, sent <- c2sSent, pulse <- c2sPulse,
                          Packets <- ClientPackets, Pattern <- C2SPattern
S2C == INSTANCE Link WITH wire <- s2cWire, sent <- s2cSent, pulse <- s2cPulse,
                          Packets <- ServerPackets, Pattern <- S2CPattern

NoMap == [live |-> FALSE, est |-> FALSE, salt |-> NoSalt, nonce |-> 0,
          csalt |-> NoSalt, cnonce |-> 0, tok |-> 0, slot |-> 0,
          age |-> 0, sinceRecv |-> 0, sinceKA |-> 0, acked |-> FALSE]

Mapping == [live : BOOLEAN, est : BOOLEAN, salt : Salts \cup {NoSalt}, nonce : Nonce \cup {0},
            csalt : Salts \cup {NoSalt}, cnonce : Nonce \cup {0},
            tok : Tokens \cup {0}, slot : Slots \cup {0},
            age : 0..HandshakeDeadline, sinceRecv : 0..Timeout, sinceKA : 0..SendInterval,
            acked : BOOLEAN]

FreeSlot == [used |-> FALSE, addr |-> 0, tok |-> 0, confirmed |-> FALSE]

SlotRec == [used : BOOLEAN, addr : Addrs \cup {0}, tok : Tokens \cup {0}, confirmed : BOOLEAN]

NoPkt == [type |-> "none"]

\* a saturating tick of a counter whose bound may be 0 (off)
Sat(x, bound_) == IF bound_ = 0 THEN 0 ELSE IF x < bound_ THEN x + 1 ELSE bound_

SaltOf(c)     == <<c, cAtt[c]>>
KeysOf(p)     == [salt |-> p.salt, nonce |-> p.nonce]
ClientKeys(c) == [salt |-> SaltOf(c), nonce |-> cKeys[c]]

UsedSlots   == {i \in Slots : slot[i].used}
FreeSlots   == MaxSlots - Cardinality(UsedSlots)
LiveAddrs   == {a \in Addrs : smap[a].live}
NumMappings == Cardinality(LiveAddrs)

\* the lowest free slot, as rocketnet_client_slots_assign scans
LowestFree == CHOOSE i \in Slots \ UsedSlots : \A j \in Slots \ UsedSlots : i <= j

\* a client with this id holds a slot (rocketnet_client_slots_find)
ClientIdConnectedAtRequest(t) ==
    ClientIdCheckAtRequest /\ \E i \in UsedSlots : IdOf(slot[i].tok) = IdOf(t)
ClientIdConnectedAtResponse(t) ==
    ClientIdCheckAtResponse /\ \E i \in UsedSlots : IdOf(slot[i].tok) = IdOf(t)
Fresh(t) ==
    t \notin ExpiredTokens /\ bound[t] = 0
    /\ ~\E i \in UsedSlots : IdOf(slot[i].tok) = IdOf(t)

\* the mapping a sealed packet is looked up under: its source address
\* (rocketnet_encryption_map_find on from), or, for the witness, whichever
\* live mapping its keys open
MapAddr(p) ==
    IF AddressIsIdentity \/ ~(\E a \in LiveAddrs : KeysOf(smap[a]) = KeysOf(p))
      THEN p.from
      ELSE CHOOSE a \in LiveAddrs : KeysOf(smap[a]) = KeysOf(p)

\* the family a request arrived on: the sending client's selected family.
\* Addresses are unique (init and ClientMove), so the sender is one client.
ArrivalFamily(a) ==
    IF \E c \in Clients : cAddr[c] = a
      THEN PickFamily(CHOOSE c \in Clients : cAddr[c] = a)
      ELSE "ipv4"

(***************************************************************************)
(* Fresh values: a nonce no live mapping holds, no client holds, and no    *)
(* packet in flight carries.                                               *)
(***************************************************************************)
WireNonces(w) == {w[i].pkt.nonce : i \in {j \in DOMAIN w : "nonce" \in DOMAIN w[j].pkt}}

PktNonce(p) == IF "nonce" \in DOMAIN p THEN {p.nonce} ELSE {}

FreshNonce == Nonce \ (WireNonces(c2sWire) \cup WireNonces(s2cWire)
                       \cup {smap[a].nonce : a \in LiveAddrs}
                       \cup {cKeys[c] : c \in Clients}
                       \cup UNION {PktNonce(cDiscPkt[c]) : c \in Clients}
                       \cup PktNonce(sDiscPkt))

(***************************************************************************)
(* Sending through the links, with the first-of-kind loss in front.        *)
(***************************************************************************)
KindLost(lost, t) == KindLoss = "firstkind" /\ t \notin lost

ClientSendPacket(c, p) ==
    IF KindLost(cLostKinds[c], p.type)
      THEN cLostKinds' = [cLostKinds EXCEPT ![c] = @ \cup {p.type}] /\ UNCHANGED c2sVars
      ELSE C2S!Send(p) /\ UNCHANGED cLostKinds

ServerSendPacket(p) ==
    IF KindLost(sLostKinds, p.type)
      THEN sLostKinds' = sLostKinds \cup {p.type} /\ UNCHANGED s2cVars
      ELSE S2C!Send(p) /\ UNCHANGED sLostKinds

ServerSilent == UNCHANGED <<s2cVars, sLostKinds>>

Init ==
    /\ cstate = [c \in Clients |-> 0] /\ cAtt = [c \in Clients |-> 0]
    /\ cTok = [c \in Clients |-> 1] /\ cAddr = [c \in Clients |-> c]
    /\ cCookieAddr = [c \in Clients |-> 0] /\ cKeys = [c \in Clients |-> 0]
    /\ cMoves = [c \in Clients |-> 0] /\ cSinceSend = [c \in Clients |-> 0]
    /\ cDiscPending = [c \in Clients |-> 0] /\ cDiscPkt = [c \in Clients |-> NoPkt]
    /\ cLostKinds = [c \in Clients |-> {}]
    /\ smap = [a \in Addrs |-> NoMap] /\ slot = [i \in Slots |-> FreeSlot]
    /\ bound = [t \in Tokens |-> 0] /\ churnDenied = FALSE
    /\ sDiscPending = 0 /\ sDiscPkt = NoPkt /\ sLostKinds = {}
    /\ C2S!Init /\ S2C!Init

(***************************************************************************)
(* The clients.                                                            *)
(***************************************************************************)

\* rocketnet_client_protocol_connect, with the client's own token or any
\* token the environment holds (TokenChoice)
ClientConnect(c) ==
    /\ cstate[c] <= 0 /\ cAtt[c] < MaxAttempts /\ CreateSucceeds(c)
    /\ \E t \in (IF TokenChoice = "own" THEN {c} ELSE Tokens) :
        /\ cTok' = [cTok EXCEPT ![c] = t]
        /\ cstate' = [cstate EXCEPT ![c] = 1] /\ cAtt' = [cAtt EXCEPT ![c] = @ + 1]
        /\ cCookieAddr' = [cCookieAddr EXCEPT ![c] = 0] /\ cKeys' = [cKeys EXCEPT ![c] = 0]
        /\ cSinceSend' = [cSinceSend EXCEPT ![c] = SendInterval]   \* the first update sends at once
    /\ UNCHANGED <<cAddr, cMoves, cDiscPending, cDiscPkt, cLostKinds, serverVars, c2sVars, s2cVars>>

\* rocketnet_client_protocol_disconnect: in 3 or 4 a burst of sealed
\* disconnects from the address held now, then state 0 and the keys gone
ClientAppDisconnect(c) ==
    /\ Disconnects /\ cstate[c] # 0 /\ cDiscPending[c] = 0
    /\ IF cstate[c] \in {3, 4}
         THEN /\ cDiscPending' = [cDiscPending EXCEPT ![c] = DisconnectCopies]
              /\ cDiscPkt' = [cDiscPkt EXCEPT ![c] = [type |-> "disconnect", from |-> cAddr[c],
                                                       salt |-> SaltOf(c), nonce |-> cKeys[c]]]
         ELSE UNCHANGED <<cDiscPending, cDiscPkt>>
    /\ cstate' = [cstate EXCEPT ![c] = 0] /\ cKeys' = [cKeys EXCEPT ![c] = 0]
    /\ UNCHANGED <<cAtt, cTok, cAddr, cCookieAddr, cMoves, cSinceSend, cLostKinds,
                   serverVars, c2sVars, s2cVars>>

\* the host vanishes (power, a cable): nothing is sent, ever again
ClientVanish(c) ==
    /\ Disconnects /\ cstate[c] # 0
    /\ cstate' = [cstate EXCEPT ![c] = 0] /\ cKeys' = [cKeys EXCEPT ![c] = 0]
    /\ UNCHANGED <<cAtt, cTok, cAddr, cCookieAddr, cMoves, cSinceSend, cDiscPending, cDiscPkt,
                   cLostKinds, serverVars, c2sVars, s2cVars>>

\* the client's address changes (NAT rebinding, a handover) to an address
\* no client holds; the old address keeps whatever mapping it had, and a
\* held cookie no longer verifies (its MAC covers the address)
ClientMove(c) ==
    /\ cMoves[c] < MaxMoves
    /\ \E a \in Addrs : /\ \A d \in Clients : cAddr[d] # a
                        /\ cAddr' = [cAddr EXCEPT ![c] = a]
    /\ cMoves' = [cMoves EXCEPT ![c] = @ + 1]
    /\ UNCHANGED <<cstate, cAtt, cTok, cCookieAddr, cKeys, cSinceSend, cDiscPending, cDiscPkt,
                   cLostKinds, serverVars, c2sVars, s2cVars>>

\* the scheduled send, rocketnet_client_protocol_update: nothing is sent
\* because a packet arrived
ClientPacket(c) ==
    CASE cstate[c] \in {1, 2} -> [type |-> "request", from |-> cAddr[c], salt |-> SaltOf(c),
                                  cookie |-> cCookieAddr[c] = cAddr[c], tok |-> cTok[c]]
      [] cstate[c] = 3        -> [type |-> "response", from |-> cAddr[c], salt |-> SaltOf(c), nonce |-> cKeys[c]]
      [] cstate[c] = 4        -> [type |-> "keepalive", from |-> cAddr[c], salt |-> SaltOf(c), nonce |-> cKeys[c]]

\* a client whose selected family the server does not serve sends to an
\* address nobody serves
ClientSend(c) ==
    /\ cstate[c] \in 1..4 /\ cSinceSend[c] = SendInterval
    /\ IF ReachesServer(c) THEN ClientSendPacket(c, ClientPacket(c))
                        ELSE UNCHANGED <<c2sVars, cLostKinds>>
    /\ cSinceSend' = [cSinceSend EXCEPT ![c] = 0]
    /\ UNCHANGED <<cstate, cAtt, cTok, cAddr, cCookieAddr, cKeys, cMoves, cDiscPending, cDiscPkt,
                   serverVars, s2cVars>>

\* one copy of the disconnect burst; the burst is never retransmitted
ClientDiscCopy(c) ==
    /\ cDiscPending[c] > 0
    /\ ClientSendPacket(c, cDiscPkt[c])
    /\ cDiscPending' = [cDiscPending EXCEPT ![c] = @ - 1]
    /\ cDiscPkt' = IF cDiscPending[c] = 1 THEN [cDiscPkt EXCEPT ![c] = NoPkt] ELSE cDiscPkt
    /\ UNCHANGED <<cstate, cAtt, cTok, cAddr, cCookieAddr, cKeys, cMoves, cSinceSend,
                   serverVars, s2cVars>>

(***************************************************************************)
(* A client receives what is addressed to the address it holds; the state  *)
(* gate and the key check are the client's (ClientConnection has the       *)
(* detail).  A packet to an address nobody holds goes to the void.         *)
(***************************************************************************)
clientRecvUnchanged == <<cAtt, cTok, cAddr, cMoves, cSinceSend, cDiscPending, cDiscPkt, cLostKinds,
                         serverVars, c2sVars>>

ClientRecvAt(c, p) ==
    /\ S2C!Deliver(p)
    /\ CASE p.type = "cookie" ->
                /\ IF cstate[c] \in {1, 2} /\ p.salt = SaltOf(c)
                     THEN cCookieAddr' = [cCookieAddr EXCEPT ![c] = cAddr[c]]
                          /\ cstate' = [cstate EXCEPT ![c] = 2]
                     ELSE UNCHANGED <<cCookieAddr, cstate>>
                /\ UNCHANGED cKeys
         [] p.type = "challenge" ->
                /\ IF cstate[c] = 2 /\ p.salt = SaltOf(c)
                     THEN cKeys' = [cKeys EXCEPT ![c] = p.nonce] /\ cstate' = [cstate EXCEPT ![c] = 3]
                     ELSE UNCHANGED <<cKeys, cstate>>
                /\ UNCHANGED cCookieAddr
         [] p.type = "denied" ->
                /\ IF \/ cstate[c] = 2 /\ p.salt = SaltOf(c)
                      \/ cstate[c] = 3 /\ KeysOf(p) = ClientKeys(c)
                     THEN cstate' = [cstate EXCEPT ![c] = -1]
                     ELSE UNCHANGED cstate
                /\ UNCHANGED <<cCookieAddr, cKeys>>
         [] p.type \in {"keepalive", "disconnect"} ->
                /\ IF cstate[c] \in {3, 4} /\ KeysOf(p) = ClientKeys(c)
                     THEN IF p.type = "keepalive"
                            THEN cstate' = [cstate EXCEPT ![c] = 4] /\ UNCHANGED cKeys
                            ELSE cstate' = [cstate EXCEPT ![c] = 0] /\ cKeys' = [cKeys EXCEPT ![c] = 0]
                     ELSE UNCHANGED <<cstate, cKeys>>
                /\ UNCHANGED cCookieAddr
    /\ UNCHANGED clientRecvUnchanged

ClientRecv(p) ==
    \/ \E c \in Clients : cAddr[c] = p.to /\ ClientRecvAt(c, p)
    \/ /\ \A c \in Clients : cAddr[c] # p.to
       /\ S2C!Deliver(p)
       /\ UNCHANGED <<clientVars, serverVars, c2sVars>>

(***************************************************************************)
(* The server receives.  rocketnet_server_protocol_process_packet: a       *)
(* request runs the ladder, a sealed type is looked up by source address,  *)
(* opened under the mapping's keys, and dispatched.  What the code refuses *)
(* is delivered and discarded.                                             *)
(***************************************************************************)
serverRecvUnchanged == <<clientVars, sDiscPending, sDiscPkt>>

\* rocketnet_server_protocol_process_connection_request, the ladder in the
\* standard's order as far as this model sees it: a connected address is
\* silent; a half-open mapping resends its stored challenge whatever salt
\* the request carries; no cookie (or a cookie issued to another
\* address) -> a cookie; an expired token is refused before cookie/decrypt;
\* a token bound elsewhere or a connected client id is silent; history
\* binds only when a response opens; no slot is denied; else map+challenge
ServerRecvRequest(p) ==
    /\ p.type = "request"
    /\ C2S!Deliver(p)
    /\ LET m == smap[p.from]
           boundElsewhere == History = "kept" /\ bound[p.tok] \notin {0, p.from}
           heldFull == HoldUntilExpire /\ bound[p.tok] = 0
                       /\ Cardinality({t \in Tokens : bound[t] # 0}) >= HeldCapacity
           fresh == Fresh(p.tok)
       IN CASE m.live /\ m.slot # 0 ->
                 ServerSilent /\ UNCHANGED <<smap, bound, churnDenied>>
            [] m.live ->
                 \* STANDARD.md: a mapped address always gets its original challenge.
                 /\ ServerSendPacket([type |-> "challenge", to |-> p.from, salt |-> m.csalt, nonce |-> m.cnonce])
                 /\ smap' = [smap EXCEPT ![p.from].sinceRecv = 0]
                 /\ UNCHANGED <<bound, churnDenied>>
            [] p.tok \in ExpiredTokens ->
                 ServerSilent /\ UNCHANGED <<smap, bound, churnDenied>>
            [] ~TokenAddressAdvertised(ArrivalFamily(p.from), TokenFamilies) ->
                 ServerSilent /\ UNCHANGED <<smap, bound, churnDenied>>
            [] ~p.cookie ->
                 ServerSendPacket([type |-> "cookie", to |-> p.from, salt |-> p.salt])
                 /\ UNCHANGED <<smap, bound, churnDenied>>
            [] (boundElsewhere \/ ClientIdConnectedAtRequest(p.tok)
                 \/ (HoldUntilExpire /\ bound[p.tok] # 0) \/ heldFull) ->
                 ServerSilent /\ UNCHANGED <<smap, bound>>
                 /\ churnDenied' = (churnDenied \/ (fresh /\ (FreeSlots > 0)))
            [] OTHER ->
                 /\ UNCHANGED bound
                 /\ CASE FreeSlots = 0 ->
                           /\ ServerSendPacket([type |-> "denied", to |-> p.from, salt |-> p.salt, nonce |-> 0])
                           /\ UNCHANGED smap
                           /\ churnDenied' = (churnDenied \/ (fresh /\ (FreeSlots > 0)))
                      [] NumMappings = MapCap ->
                           ServerSilent /\ UNCHANGED <<smap, churnDenied>>
                       [] OTHER ->
                            /\ \E n \in FreshNonce :
                                 /\ smap' = [smap EXCEPT ![p.from] = [NoMap EXCEPT !.live = TRUE, !.salt = p.salt,
                                                                                   !.nonce = n, !.csalt = p.salt,
                                                                                   !.cnonce = n, !.tok = p.tok]]
                                 /\ ServerSendPacket([type |-> "challenge", to |-> p.from, salt |-> p.salt, nonce |-> n])
                            /\ UNCHANGED churnDenied
    /\ UNCHANGED <<slot, serverRecvUnchanged>>

\* a response that opens under the mapping's handshake keys refreshes the
\* idle clock. It binds the token to this address in the used-token history,
\* then an already-connected address or client id is ignored; no slot is
\* denied; otherwise the lowest free slot is assigned and a keep-alive goes
\* out (rocketnet_server_process_connection_response).
\* Reversed witness only: the implementation keeps this FALSE.
RefreshConnectedOnResponse == FALSE

ServerRecvResponse(p) ==
    /\ p.type = "response"
    /\ C2S!Deliver(p)
    /\ LET a  == MapAddr(p)
           m  == smap[a]
           m1 == IF m.slot = 0 \/ RefreshConnectedOnResponse THEN [m EXCEPT !.sinceRecv = 0] ELSE m
           bound1 == IF History = "kept" /\ m.tok \notin ExpiredTokens
                        THEN [bound EXCEPT ![m.tok] = IF @ = 0 THEN a ELSE @]
                        ELSE bound
       IN IF m.live /\ KeysOf(p) = KeysOf(m)
            THEN CASE m.slot # 0 \/ ClientIdConnectedAtResponse(m.tok) ->
                        /\ smap' = [smap EXCEPT ![a] = m1]
                        /\ bound' = bound1
                        /\ ServerSilent /\ UNCHANGED <<slot, churnDenied>>
                   [] FreeSlots = 0 ->
                        /\ smap' = [smap EXCEPT ![a] = m1]
                        /\ bound' = bound1
                        /\ ServerSendPacket([type |-> "denied", to |-> p.from, salt |-> m.salt, nonce |-> m.nonce])
                        /\ churnDenied' = (churnDenied \/ (Fresh(m.tok) /\ (FreeSlots > 0)))
                        /\ UNCHANGED slot
                   [] OTHER ->
                        LET i == LowestFree IN
                        /\ slot' = [slot EXCEPT ![i] = [used |-> TRUE, addr |-> a, tok |-> m.tok, confirmed |-> FALSE]]
                        /\ smap' = [smap EXCEPT ![a] = [m EXCEPT !.est = TRUE, !.slot = i, !.age = 0,
                                                                  !.sinceRecv = 0, !.sinceKA = 0, !.acked = FALSE]]
                        /\ bound' = bound1
                        /\ ServerSendPacket([type |-> "keepalive", to |-> p.from, salt |-> m.salt, nonce |-> m.nonce])
                        /\ UNCHANGED churnDenied
            ELSE ServerSilent /\ UNCHANGED <<smap, slot, bound, churnDenied>>
    /\ UNCHANGED serverRecvUnchanged

\* The slot and mapping are released together. A same-address reconnect
\* remains permitted while the history is live.
Release(a) ==
    /\ smap' = [smap EXCEPT ![a] = NoMap]
    /\ slot' = [slot EXCEPT ![smap[a].slot] = FreeSlot]
    /\ UNCHANGED churnDenied

\* a sealed session packet that opens for a mapping with a slot: a
\* keep-alive refreshes the idle clock, confirms the slot and its block
\* acknowledges a keep-alive (the latch); a disconnect releases the slot
\* and removes the mapping at once.  A mapping without a slot ignores it
\* (NOT_CONNECTED), and so does a wrong key.
ServerRecvSession(p) ==
    /\ p.type \in {"keepalive", "disconnect"}
    /\ C2S!Deliver(p)
    /\ LET a == MapAddr(p)
           m == smap[a]
       IN IF m.live /\ m.slot # 0 /\ KeysOf(p) = KeysOf(m)
            THEN IF p.type = "keepalive"
                   THEN /\ smap' = [smap EXCEPT ![a] = [m EXCEPT !.sinceRecv = 0, !.acked = TRUE]]
                        /\ slot' = [slot EXCEPT ![m.slot].confirmed = TRUE]
                        /\ UNCHANGED churnDenied
                   ELSE Release(a)
            ELSE UNCHANGED <<smap, slot, churnDenied>>
    /\ ServerSilent
    /\ UNCHANGED bound
    /\ UNCHANGED serverRecvUnchanged

ServerRecv(p) == ServerRecvRequest(p) \/ ServerRecvResponse(p) \/ ServerRecvSession(p)

(***************************************************************************)
(* The server's own sends.                                                 *)
(***************************************************************************)
serverSendUnchanged == <<clientVars, bound, churnDenied, c2sVars>>

\* rocketnet_server_protocol_write_keep_alive_if_due: paced off the
\* keep-alive's own clock, to a client that has not acknowledged one
\* (RepeatKeepAlive) and to one that has
\*
\* The transport's deferred-maintenance flush (established clients only:
\* the sweep defers, send_packets re-runs the due predicate after the fill
\* phase, and a payload sealed in that fill suppresses the write) is
\* model-preserving here and needs no action of its own: this module has
\* no payload send and no intra-update phase distinction, so deferral
\* changes neither ServerKeepAlive's enabledness over modelled state nor
\* any state it writes (sinceKA still resets exactly when a keep-alive is
\* sent). The unacknowledged regime the liveness properties turn on --
\* RepeatKeepAlive recurrence until acked -- is never deferred in the
\* code, so Connects and ServerNotStranded are unaffected.
ServerKeepAlive(i) ==
    /\ slot[i].used
    /\ LET a == slot[i].addr
           m == smap[a]
       IN /\ m.live /\ m.est
          /\ RepeatKeepAlive \/ m.acked
          /\ m.sinceKA = SendInterval
          /\ ServerSendPacket([type |-> "keepalive", to |-> a, salt |-> m.salt, nonce |-> m.nonce])
          /\ smap' = [smap EXCEPT ![a] = [m EXCEPT !.sinceKA = 0]]
    /\ UNCHANGED <<slot, churnDenied, sDiscPending, sDiscPkt>>
    /\ UNCHANGED serverSendUnchanged

\* rocketnet_server_protocol_disconnect_client: the burst, then the slot
\* released and the mapping removed at once
ServerAppDisconnect(i) ==
    /\ Disconnects /\ slot[i].used /\ sDiscPending = 0
    /\ LET a == slot[i].addr
           m == smap[a]
       IN /\ m.live /\ m.est
          /\ sDiscPkt' = [type |-> "disconnect", to |-> a, salt |-> m.salt, nonce |-> m.nonce]
          /\ sDiscPending' = DisconnectCopies
          /\ Release(a)
    /\ UNCHANGED <<s2cVars, sLostKinds>>
    /\ UNCHANGED serverSendUnchanged

ServerDiscCopy ==
    /\ sDiscPending > 0
    /\ ServerSendPacket(sDiscPkt)
    /\ sDiscPending' = sDiscPending - 1
    /\ sDiscPkt' = IF sDiscPending = 1 THEN NoPkt ELSE sDiscPkt
    /\ UNCHANGED <<smap, slot, churnDenied>>
    /\ UNCHANGED serverSendUnchanged

(***************************************************************************)
(* Time passes on both ends and on both wires.  Clients: the send clocks.  *)
(* Server, rocketnet_server_protocol_update (the sweep): a half-open        *)
(* mapping is removed at whichever of its idle and handshake deadlines     *)
(* comes first; an established one idle for the timeout releases its slot  *)
(* and is removed with it (the witness SweepRemovesMapping = FALSE          *)
(* releases the slot and leaves the mapping).                              *)
(***************************************************************************)
Expired(m)   == m.live /\ ~m.est /\ HandshakeDeadline > 0 /\ Sat(m.age, HandshakeDeadline) = HandshakeDeadline
IdleExpired(m) == m.live /\ ~m.est /\ Timeout > 0 /\ Sat(m.sinceRecv, Timeout) = Timeout
TimedOut(m)  == m.live /\ m.est /\ Timeout > 0 /\ Sat(m.sinceRecv, Timeout) = Timeout

Tick ==
    /\ C2S!Age /\ S2C!Age
    /\ cSinceSend' = [c \in Clients |-> Sat(cSinceSend[c], SendInterval)]
    /\ smap' = [a \in Addrs |->
                  LET m == smap[a] IN
                  CASE ~m.live -> m
                    [] Expired(m) -> NoMap
                    [] IdleExpired(m) -> NoMap
                    [] ~m.est -> [m EXCEPT !.age = Sat(m.age, HandshakeDeadline),
                                          !.sinceRecv = Sat(m.sinceRecv, Timeout)]
                    [] TimedOut(m) /\ SweepRemovesMapping -> NoMap
                    [] OTHER -> [m EXCEPT !.sinceRecv = Sat(m.sinceRecv, Timeout),
                                          !.sinceKA = Sat(m.sinceKA, SendInterval)]]
    /\ slot' = [i \in Slots |-> IF slot[i].used /\ TimedOut(smap[slot[i].addr]) THEN FreeSlot ELSE slot[i]]
    /\ UNCHANGED <<cstate, cAtt, cTok, cAddr, cCookieAddr, cKeys, cMoves, cDiscPending, cDiscPkt,
                   cLostKinds, bound, churnDenied, sDiscPending, sDiscPkt, sLostKinds>>

C2SEnvironment == C2S!Environment /\ UNCHANGED <<clientVars, serverVars, s2cVars>>
S2CEnvironment == S2C!Environment /\ UNCHANGED <<clientVars, serverVars, c2sVars>>

Next ==
    \/ \E c \in Clients : ClientConnect(c) \/ ClientAppDisconnect(c) \/ ClientVanish(c)
                          \/ ClientMove(c) \/ ClientSend(c) \/ ClientDiscCopy(c)
    \/ \E p \in ServerPackets : ClientRecv(p)
    \/ \E p \in ClientPackets : ServerRecv(p)
    \/ \E i \in Slots : ServerKeepAlive(i) \/ ServerAppDisconnect(i)
    \/ ServerDiscCopy
    \/ Tick
    \/ C2SEnvironment \/ S2CEnvironment

(***************************************************************************)
(* Fairness, as ClientConnection states it: time passes, the scheduled     *)
(* sends go out and a re-sent packet eventually enters the wire (strong    *)
(* fairness on the through-sends), the applications connect, the bursts    *)
(* drain, and the head of each wire is delivered.  Liveness runs use a     *)
(* FIFO link (Reordering FALSE, MaxDelay 0, Duplication FALSE); README-    *)
(* ClientConnection.md records why.  SpecTyped adds Link's                 *)
(* EventualDelivery per packet type, for "arbitrary".                      *)
(***************************************************************************)
ServerTypes == {"cookie", "challenge", "denied", "keepalive", "disconnect"}
ClientTypes == {"request", "response", "keepalive", "disconnect"}

ClientRecvHead == s2cWire # <<>> /\ ClientRecv(Head(s2cWire).pkt)
ServerRecvHead == c2sWire # <<>> /\ ServerRecv(Head(c2sWire).pkt)

ClientRecvType(t) == ClientRecvHead /\ Head(s2cWire).pkt.type = t
ServerRecvType(t) == ServerRecvHead /\ Head(c2sWire).pkt.type = t

ClientSendThrough(c)      == ClientSend(c) /\ c2sWire' # c2sWire
ServerKeepAliveThrough(i) == ServerKeepAlive(i) /\ s2cWire' # s2cWire

SendFair ==
    /\ WF_vars(Tick)
    /\ \A c \in Clients : /\ WF_vars(ClientConnect(c))
                          /\ WF_vars(ClientSend(c)) /\ SF_vars(ClientSendThrough(c))
                          /\ WF_vars(ClientDiscCopy(c))
    /\ \A i \in Slots : WF_vars(ServerKeepAlive(i)) /\ SF_vars(ServerKeepAliveThrough(i))
    /\ WF_vars(ServerDiscCopy)

S2CSendType(t) == \E p \in ServerPackets : p.type = t /\ S2C!Send(p) /\ s2cWire' # s2cWire
C2SSendType(t) == \E p \in ClientPackets : p.type = t /\ C2S!Send(p) /\ c2sWire' # c2sWire

DeliveryFairHead == SF_vars(ClientRecvHead) /\ SF_vars(ServerRecvHead)

DeliveryFairTyped ==
    /\ DeliveryFairHead
    /\ \A t \in ServerTypes : ([]<> <<S2CSendType(t)>>_vars) => ([]<> <<ClientRecvType(t)>>_vars)
    /\ \A t \in ClientTypes : ([]<> <<C2SSendType(t)>>_vars) => ([]<> <<ServerRecvType(t)>>_vars)

Fairness      == SendFair /\ DeliveryFairHead
FairnessTyped == SendFair /\ DeliveryFairTyped

Spec      == Init /\ [][Next]_vars /\ Fairness
SpecTyped == Init /\ [][Next]_vars /\ FairnessTyped

(***************************************************************************)
(* Safety.                                                                 *)
(***************************************************************************)
TypeOK ==
    /\ cstate \in [Clients -> -1..4] /\ cAtt \in [Clients -> 0..MaxAttempts]
    /\ cTok \in [Clients -> Tokens] /\ cAddr \in [Clients -> Addrs]
    /\ cCookieAddr \in [Clients -> Addrs \cup {0}] /\ cKeys \in [Clients -> Nonce \cup {0}]
    /\ cMoves \in [Clients -> 0..MaxMoves] /\ cSinceSend \in [Clients -> 0..SendInterval]
    /\ cDiscPending \in [Clients -> 0..DisconnectCopies]
    /\ cDiscPkt \in [Clients -> ClientSealed \cup {NoPkt}]
    /\ smap \in [Addrs -> Mapping] /\ slot \in [Slots -> SlotRec]
    /\ bound \in [Tokens -> Addrs \cup {0}] /\ churnDenied \in BOOLEAN
    /\ sDiscPending \in 0..DisconnectCopies /\ sDiscPkt \in ServerPackets \cup {NoPkt}
    /\ C2S!TypeOK /\ S2C!TypeOK

\* no two live clients in one slot: at most one client holds the keys a
\* used slot's mapping opens under, and no two used slots share an address
\* (the map is a function of the address, so this is the slots' half)
OneClientPerSlot ==
    /\ \A i \in UsedSlots :
         Cardinality({c \in Clients : cstate[c] \in {3, 4} /\ ClientKeys(c) = KeysOf(smap[slot[i].addr])}) <= 1
    /\ \A i, j \in UsedSlots : i # j => slot[i].addr # slot[j].addr

\* a client id holds at most one slot (S:2647, S:2817)
OneSlotPerClientId ==
    \A i, j \in UsedSlots : i # j => IdOf(slot[i].tok) # IdOf(slot[j].tok)

\* no slot without a live mapping behind it, and no established mapping
\* without the slot it names (the leak the audits suspect at the 2x table):
\* a used slot's address holds a live, established mapping naming that
\* slot, and an established mapping's slot is used and bound to it
SlotMappingBijection ==
    /\ \A i \in UsedSlots : LET m == smap[slot[i].addr] IN m.live /\ m.est /\ m.slot = i
    /\ \A a \in Addrs : smap[a].est => /\ smap[a].live /\ smap[a].slot # 0
                                       /\ slot[smap[a].slot].used /\ slot[smap[a].slot].addr = a
    /\ \A a \in Addrs : smap[a].live /\ ~smap[a].est => smap[a].slot = 0

\* a slot is assigned only on an opened response: a live mapping holds a
\* slot only once established, so no amount of requests consumes one
\* (S:2712-2715); and the map stays inside its capacity
NoSlotBeforeResponse == \A a \in Addrs : smap[a].slot # 0 => smap[a].est
MappingsBounded      == NumMappings <= MapCap

\* One token may hold one live connection, but may connect again after release.
OneTokenOneLiveConnection ==
    \A t \in Tokens : Cardinality({i \in UsedSlots : slot[i].tok = t}) <= 1

\* Compatibility name used by the existing MCServerConnection wrapper.
OneConnectionPerToken == OneTokenOneLiveConnection

\* A fresh, valid token whose identity is free is not refused while a slot is free.
ChurnNeverDenied == ~churnDenied

\* Expiry is checked before a token can create a mapping.
ExpiredTokenNeverMapped ==
    \A a \in LiveAddrs : smap[a].tok \notin ExpiredTokens

\* no two live mappings share keys: the nonce is fresh per mapping
NonceFresh == \A a, b \in LiveAddrs : a # b => KeysOf(smap[a]) # KeysOf(smap[b])

\* Each live mapping retains the challenge chosen at its creation.
StoredChallengeNeverReplaced ==
    \A a \in LiveAddrs :
        /\ smap[a].csalt = smap[a].salt
        /\ smap[a].cnonce = smap[a].nonce
        /\ smap[a].csalt # NoSalt

\* the timeouts fire when they are due
TimeoutHonoured  == \A a \in Addrs : (smap[a].est /\ Timeout > 0) => smap[a].sinceRecv < Timeout
DeadlineHonoured == \A a \in Addrs : (smap[a].live /\ ~smap[a].est /\ HandshakeDeadline > 0)
                                       => smap[a].age < HandshakeDeadline
HalfOpenIdleHonoured == \A a \in Addrs : (smap[a].live /\ ~smap[a].est /\ Timeout > 0)
                                           => smap[a].sinceRecv < Timeout

\* a client's packets never reach another client's slot: a client packet
\* that touches a used slot (its record, or the mapping behind it) came
\* from that slot's address and opened under that mapping's keys.  After
\* an address change the client's packets come from an address with no
\* mapping (or another client's), so they touch nothing.
Touches(i) == slot'[i] # slot[i] \/ smap'[slot[i].addr] # smap[slot[i].addr]

PacketsReachOwnSlotOnly ==
    [][\A p \in ClientPackets :
         C2S!Deliver(p) =>
           \A i \in UsedSlots :
             Touches(i) => /\ p.type # "request"
                           /\ p.from = slot[i].addr
                           /\ KeysOf(p) = KeysOf(smap[slot[i].addr])]_vars

\* no receive sends but the ladder's own answer: a session packet never
\* provokes a send (S:2852-2853; the keep-alive is timer driven)
SessionNeverAnswered ==
    [][(\E p \in ClientPackets : p.type \in {"keepalive", "disconnect"} /\ C2S!Deliver(p))
         => s2cSent' = s2cSent]_vars

\* A response retransmission cannot provide fresh activity for a live slot.
ResponseNeverRefreshesConnected ==
    [][(\E p \in ClientPackets : p.type = "response" /\ C2S!Deliver(p))
         => \A i \in UsedSlots : smap'[slot[i].addr].sinceRecv = smap[slot[i].addr].sinceRecv]_vars

(***************************************************************************)
(* Liveness.                                                               *)
(***************************************************************************)

\* with a slot free and fair loss, every attempt connects (runs with one
\* client, or as many slots and tokens as clients)
Connects == \A c \in Clients : (cstate[c] = 1) ~> (cstate[c] = 4)

\* a client whose host has one family connects with a token naming both
\* families to a server serving the host's family
OneFamilyHostConnects ==
    (Cardinality(HostFamilies) = 1 /\ HostFamilies \subseteq TokenFamilies /\ ServedFamily \in HostFamilies)
      => \A c \in Clients : <>(cstate[c] = 4)

\* an IPv4-only client and an IPv6-only client, each with a token naming both
\* families, both reach connected against one server whose config names both.
\* The consequent does not mention ServedFamilies: a server that names both
\* but serves only the first family leaves the other client at state 1.
BothFamiliesConnect ==
    (Cardinality(Clients) = 2
     /\ ClientHost(1) = {"ipv4"} /\ ClientHost(2) = {"ipv6"}
     /\ TokenFamilies = {"ipv4", "ipv6"}
     /\ ConfiguredFamilies = {"ipv4", "ipv6"})
      => (\A c \in Clients : <>(cstate[c] = 4))

\* neither peer is left waiting for a packet nobody will send again: a
\* half-open mapping is established or dies, and an unacknowledged slot is
\* acknowledged or released
ServerNotStranded ==
    \A a \in Addrs :
      /\ (smap[a].live /\ ~smap[a].est) ~> (~smap[a].live \/ smap[a].est)
      /\ (smap[a].est /\ ~smap[a].acked) ~> (~smap[a].est \/ smap[a].acked)

\* a timed-out slot is freed: a slot whose client is gone (no client holds
\* its keys in a session state, so nothing will ever refresh it) is
\* eventually released
Gone(i) == \A c \in Clients : ~(cstate[c] \in {3, 4} /\ ClientKeys(c) = KeysOf(smap[slot[i].addr]))

TimedOutSlotFreed == \A i \in Slots : (slot[i].used /\ Gone(i)) ~> ~slot[i].used

\* a half-open mapping whose client is gone dies at its deadline
AbandonedMappingDies ==
    \A a \in Addrs :
      (smap[a].live /\ ~smap[a].est /\ \A c \in Clients : ~(cstate[c] \in {1, 2, 3} /\ cAddr[c] = a /\ SaltOf(c) = smap[a].salt))
        ~> ~smap[a].live

=============================================================================
