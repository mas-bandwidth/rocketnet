------------------------- MODULE ServerConnection -------------------------
(***************************************************************************)
(* rocketnet: the server's slots and encryption mappings.                  *)
(*                                                                         *)
(* The server of sdk/rocketnet_server_protocol.cpp, driven by              *)
(* rocketnet_server_transport_update (sdk/rocketnet_server_transport.cpp), *)
(* with the tables it keys on: the encryption map                          *)
(* (sdk/rocketnet_encryption_map.cpp: per-address mappings, capacity       *)
(* 2 x max_clients, a LIFO free list of indices), the client slots         *)
(* (sdk/rocketnet_client_slots.cpp: the lowest free slot, the client-id    *)
(* index), the used-token history and the admitted-token set               *)
(* (sdk/rocketnet_token_tables.cpp).  Several clients talk to it over two  *)
(* instances of the shared lossy link (tla/Link.tla): C2S carries what     *)
(* every client sends, S2C what the server sends.  Every packet names an   *)
(* address: the client's source on C2S, the destination on S2C.            *)
(*                                                                         *)
(* The client address is the connection identity (STANDARD.md, "The       *)
(* client address is the connection identity").  The server finds the     *)
(* mapping for a packet by its source address and nothing else; a client   *)
(* may move to another address (NAT rebinding), including one a departed   *)
(* client used, and from there its packets meet whatever mapping lives at  *)
(* the new address.  What keeps them out of another client's slot is that  *)
(* they do not open under that mapping's keys.                             *)
(*                                                                         *)
(* Keys.  A key pair is [att, nonce]: the attempt's salt and the mapping's *)
(* session nonce are the two fresh inputs every key is derived from, so a  *)
(* sealed packet opens iff its [att, nonce] is the receiver's.  An attempt *)
(* is <<c, n>>, client c's n-th connect.  The connect token an attempt     *)
(* presents is its own unless SharedToken, where client 2's second attempt *)
(* presents client 1's first token (the sharing STANDARD.md, "Connect      *)
(* token sharing, and the connected set", is about).  A token's client id  *)
(* is the client it was issued to.                                         *)
(*                                                                         *)
(* Time.  Tick is one send interval on every end; every timer is a         *)
(* saturating counter of ticks.  A timeout or deadline of 0 is off.        *)
(* Token expiry, the cookie's lifetime and key rotation are not modelled:  *)
(* every token outlives the run, which is the case the token tables have   *)
(* to get right without expiry's help.                                     *)
(*                                                                         *)
(* Loss.  Both links take a Link pattern.  KindLoss = "firstkind" drops    *)
(* the first packet of every kind of every connection attempt, each way,   *)
(* before it reaches the link; KindLoss = "alternate" drops every other    *)
(* packet of every connection attempt, each way, the first one included.  *)
(*                                                                         *)
(* Witnesses.  RepeatKeepAlive = FALSE sends the establishing keep-alive   *)
(* once, at the response; ResendChallenge = FALSE leaves a half-open       *)
(* mapping silent to a retransmitted request.  Either is the hang the     *)
(* request named: a server that moves to a new state, announces it once,   *)
(* and never again.  ReleaseMapping = FALSE releases a timed-out slot and  *)
(* leaves its mapping (the leak the audits suspect); KeyCheck = FALSE       *)
(* accepts a session packet for whatever mapping lives at its source       *)
(* address.  The code is TRUE for all four.                                *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    NumClients,        \* clients 1..NumClients; client c starts at address c
    NumAddrs,          \* addresses 1..NumAddrs, >= NumClients
    Slots,             \* max_clients
    MapCap,            \* the encryption map's capacity (2 x max_clients in the code)
    HistCap,           \* the used-token history's capacity (8 x max_clients in the code)
    AdmCap,            \* the admitted-token set's capacity (2 x max_clients in the code)
    MaxAttempts,       \* connects per client
    MaxNonces,         \* the pool of session nonces
    Timeout,           \* the server's idle timeout, ticks; 0 = off
    HsDeadline,        \* the server's handshake deadline, ticks; 0 = off
    ClientTimeout,     \* the client's idle timeout, ticks; 0 = off
    DisconnectCopies,  \* the disconnect burst (10 in the code)
    ClientDisconnects, \* BOOLEAN: may a client application disconnect
    ServerDisconnects, \* BOOLEAN: may the server application disconnect a client
    Vanishes,          \* BOOLEAN: may a client vanish without a word
    Moves,             \* BOOLEAN: may a client's address change mid-attempt
    SharedToken,       \* BOOLEAN: client 2's second attempt presents client 1's first token
    Attacker,          \* BOOLEAN: an on-path attacker re-injects captured responses
    KindLoss,          \* "none", "firstkind" or "alternate" (per connection)
    RepeatKeepAlive,   \* witness knob; the code is TRUE
    ResendChallenge,   \* witness knob; the code is TRUE
    ReleaseMapping,    \* witness knob: the idle timeout removes the mapping with the slot; the code is TRUE
    KeyCheck,          \* witness knob: a session packet must open under the mapping's keys; the code is TRUE
    C2SPattern, S2CPattern,                         \* Link patterns
    Capacity, MaxDelay, Duplication, Reordering,    \* Link parameters
    BurstStart, BurstLen

ASSUME /\ NumClients \in Nat \ {0} /\ NumAddrs \in Nat /\ NumAddrs >= NumClients
       /\ Slots \in Nat \ {0} /\ MapCap \in Nat \ {0}
       /\ HistCap \in Nat \ {0} /\ AdmCap \in Nat \ {0}
       /\ MaxAttempts \in Nat \ {0} /\ MaxNonces \in Nat \ {0}
       /\ Timeout \in Nat /\ HsDeadline \in Nat /\ ClientTimeout \in Nat
       /\ DisconnectCopies \in Nat \ {0}
       /\ {ClientDisconnects, ServerDisconnects, Vanishes, Moves, SharedToken,
           Attacker, RepeatKeepAlive, ResendChallenge, ReleaseMapping, KeyCheck} \subseteq BOOLEAN
       /\ KindLoss \in {"none", "firstkind", "alternate"}
       /\ SharedToken => (NumClients >= 2 /\ MaxAttempts >= 2)

Clients == 1..NumClients
Addr    == 1..NumAddrs
SlotIdx == 1..Slots
MapIdx  == 1..MapCap
Nonce   == 1..MaxNonces
Att     == Clients \X (1..MaxAttempts)
Tokens  == Att

\* the token an attempt presents, and the client id sealed in it
TokOf(a) == IF SharedToken /\ a = <<2, 2>> THEN <<1, 1>> ELSE a
CidOf(t) == t[1]

NoKey == [att |-> <<0, 0>>, nonce |-> 0]
Key   == [att : Att, nonce : Nonce]

(***************************************************************************)
(* Client states, the standard's numbers where they exist: 0 disconnected, *)
(* 1 requesting cookie, 2 sending connection request, 3 sending connection *)
(* response, 4 connected, -1 denied, -5 timed out (every client timeout;   *)
(* which one is ClientConnection's business), -9 vanished (the outside:    *)
(* the process or its network went away; final).                          *)
(***************************************************************************)
CStates == {-9, -5, -1, 0, 1, 2, 3, 4}

(***************************************************************************)
(* The packets.  "by" is a ghost field on client packets: the client that  *)
(* built it, which no server code reads; it is how NoCrossDelivery knows   *)
(* whose packet reached which slot.                                        *)
(***************************************************************************)
ClientPackets ==
    [type : {"request"}, by : Clients, from : Addr, att : Att, cookie : BOOLEAN]
    \cup [type : {"response", "keepalive", "disconnect"}, by : Clients, from : Addr, key : Key]

ServerPackets ==
    [type : {"cookie"}, to : Addr, att : Att]
    \cup [type : {"challenge", "disconnect"}, to : Addr, key : Key]
    \cup [type : {"denied"}, to : Addr, key : [att : Att, nonce : Nonce \cup {0}]]
    \cup [type : {"keepalive"}, to : Addr, key : Key, slot : SlotIdx]

ClientTypes == {"request", "response", "keepalive", "disconnect"}
ServerTypes == {"cookie", "challenge", "denied", "keepalive", "disconnect"}

(***************************************************************************)
(* The server's records.                                                   *)
(*                                                                         *)
(* A mapping (struct rocketnet_encryption_mapping_t, keyed by address in   *)
(* the map): st is "half" (client_index = -1, established = 0) or "est"     *)
(* (a slot); key, the token and its client id, the slot, the handshake age *)
(* (handshake_deadline is fixed at creation) and the idle age               *)
(* (last_receive_time).                                                     *)
(*                                                                         *)
(* A slot (struct rocketnet_client_slots_t, one index): the mapping index  *)
(* it names (mapping_index), the client id, and the keep-alive-            *)
(* acknowledged latch (struct rocketnet_ack_endpoint_t keep_alive_acked,   *)
(* reset with the slot at assignment).                                     *)
(***************************************************************************)
NoMap  == [st |-> "none", addr |-> 0, key |-> NoKey, tok |-> <<0, 0>>, slot |-> 0,
           age |-> 0, idle |-> 0]
NoSlot == [held |-> FALSE, mi |-> 0, cid |-> 0, acked |-> FALSE]
NoBurst == [left |-> 0, to |-> 0, key |-> NoKey]

VARIABLES
    \* the clients
    cst,        \* state, CStates
    catt,       \* attempts made; the live attempt is <<c, catt[c]>>
    caddr,      \* the client's current address
    cmoved,     \* the address changed during this attempt (at most once)
    ccookie,    \* has_cookie
    ckey,       \* the committed keys
    cslot,      \* client_index, from the first keep-alive; 0 unknown
    cdue,       \* the send interval has elapsed
    cidle,      \* ticks since anything was accepted
    cburst,     \* the client's disconnect burst still to send
    \* the server
    map,        \* MapIdx -> mapping
    free,       \* the map's free list, head first (LIFO, encryption_map.cpp)
    slots,      \* SlotIdx -> slot
    kaDue,      \* SlotIdx -> the keep-alive interval has elapsed
    history,    \* the used-token history: Seq of [tok, addr], oldest first
    admitted,   \* the admitted-token set
    sburst,     \* Att -> the server's disconnect burst still to send
    \* ghosts
    admits,     \* Tokens -> connections admitted under the token
    misrouted,  \* a client's packet was accepted for a mapping not its own
    ctrConn, ctrDisc, ctrTimed,  \* CLIENT_CONNECTED, _DISCONNECTED, _TIMED_OUT
    lost,       \* KindLoss bookkeeping (see SendVia)
    captured,   \* responses an on-path attacker has seen
    starved,    \* a request found the nonce pool empty (a model bound, never the code)
    \* the links
    c2sWire, c2sSent, c2sPulse,
    s2cWire, s2cSent, s2cPulse

clientVars == <<cst, catt, caddr, cmoved, ccookie, ckey, cslot, cdue, cidle, cburst>>
serverVars == <<map, free, slots, kaDue, history, admitted, sburst>>
ghostVars  == <<admits, misrouted, ctrConn, ctrDisc, ctrTimed, lost, captured, starved>>
c2sVars    == <<c2sWire, c2sSent, c2sPulse>>
s2cVars    == <<s2cWire, s2cSent, s2cPulse>>
vars       == <<clientVars, serverVars, ghostVars, c2sVars, s2cVars>>

C2S == INSTANCE Link WITH wire <- c2sWire, sent <- c2sSent, pulse <- c2sPulse,
                          Packets <- ClientPackets, Pattern <- C2SPattern
S2C == INSTANCE Link WITH wire <- s2cWire, sent <- s2cSent, pulse <- s2cPulse,
                          Packets <- ServerPackets, Pattern <- S2CPattern

Sat(x, bound) == IF bound = 0 THEN 0 ELSE IF x < bound THEN x + 1 ELSE bound

Min(S) == CHOOSE x \in S : \A y \in S : x <= y

MyAtt(c) == <<c, catt[c]>>

Live(mi) == map[mi].st # "none"

\* rocketnet_encryption_map_find: the one mapping at this address, or 0
MapAt(a) == IF \E mi \in MapIdx : Live(mi) /\ map[mi].addr = a
              THEN CHOOSE mi \in MapIdx : Live(mi) /\ map[mi].addr = a
              ELSE 0

\* rocketnet_client_slots_num_free, _find, and the lowest-index scan of _assign
NumHeld      == Cardinality({i \in SlotIdx : slots[i].held})
NumFree      == Slots - NumHeld
CidConnected(cid) == \E i \in SlotIdx : slots[i].held /\ slots[i].cid = cid
LowestFree   == Min({i \in SlotIdx : ~slots[i].held})

\* the receiver at an address: the one client there that has not vanished
At(a) == {c \in Clients : caddr[c] = a /\ cst[c] # -9}

(***************************************************************************)
(* Fresh nonces: none a live mapping, a client, a burst, a captured packet *)
(* or a copy in flight holds.  The pool is a model-size bound;             *)
(* ModelBoundsOK checks it never runs dry.                                 *)
(***************************************************************************)
KeyNonces(w) == {w[i].pkt.key.nonce : i \in {j \in DOMAIN w : "key" \in DOMAIN w[j].pkt}}

NoncesInUse ==
    {map[mi].key.nonce : mi \in MapIdx} \cup {ckey[c].nonce : c \in Clients}
    \cup {cburst[c].key.nonce : c \in Clients} \cup {sburst[a].key.nonce : a \in Att}
    \cup {p.key.nonce : p \in captured}
    \cup KeyNonces(c2sWire) \cup KeyNonces(s2cWire)

FreshNonces == Nonce \ NoncesInUse

(***************************************************************************)
(* The token tables (sdk/rocketnet_token_tables.cpp), without expiry.      *)
(***************************************************************************)

\* rocketnet_used_token_history_check: refuse a token bound to another address
HistBoundElsewhere(t, a) == \E i \in DOMAIN history : history[i].tok = t /\ history[i].addr # a

\* rocketnet_used_token_history_record: a no-op if present; else the free
\* list, then (nothing expires here) the oldest entry is evicted
HistRecord(t, a) ==
    IF \E i \in DOMAIN history : history[i].tok = t THEN history
    ELSE IF Len(history) < HistCap THEN Append(history, [tok |-> t, addr |-> a])
    ELSE Append(Tail(history), [tok |-> t, addr |-> a])

\* rocketnet_admitted_token_set_add: success when already present (F9),
\* else a free entry, else refuse (no unexpired entry is ever evicted)
AdmitOK(t) == t \in admitted \/ Cardinality(admitted) < AdmCap

(***************************************************************************)
(* Sending, with the first-of-kind loss in front of the link.              *)
(***************************************************************************)
AttOf(p) == IF "att" \in DOMAIN p THEN p.att ELSE p.key.att

\* "firstkind": the first send of each <<side, attempt, type>> is lost.
\* "alternate": per <<side, attempt>> stream, the 1st, 3rd, 5th ... sends are
\* lost and the even ones go through; lost holds the streams whose next send
\* goes through.  (Link's own "alternate" counts every send on the link, so
\* with several senders on one link the schedule can line one sender's
\* re-sends up with the lost indices forever; see README-ServerConnection.md.)
SendVia(side, p) ==
    LET k  == <<side, AttOf(p), p.type>>
        st == <<side, AttOf(p)>>
        drop == CASE KindLoss = "firstkind" -> k \notin lost
                  [] KindLoss = "alternate" -> st \notin lost
                  [] OTHER                  -> FALSE
    IN /\ lost' = CASE KindLoss = "firstkind" -> lost \cup {k}
                    [] KindLoss = "alternate" -> IF drop THEN lost \cup {st} ELSE lost \ {st}
                    [] OTHER                  -> lost
       /\ IF drop
            THEN IF side = "c" THEN UNCHANGED c2sVars ELSE UNCHANGED s2cVars
            ELSE IF side = "c" THEN C2S!Send(p) ELSE S2C!Send(p)

ClientSendPacket(p) == SendVia("c", p)
ServerSendPacket(p) == SendVia("s", p)

Init ==
    /\ cst = [c \in Clients |-> 0] /\ catt = [c \in Clients |-> 0]
    /\ caddr = [c \in Clients |-> c] /\ cmoved = [c \in Clients |-> FALSE]
    /\ ccookie = [c \in Clients |-> FALSE] /\ ckey = [c \in Clients |-> NoKey]
    /\ cslot = [c \in Clients |-> 0] /\ cdue = [c \in Clients |-> FALSE]
    /\ cidle = [c \in Clients |-> 0] /\ cburst = [c \in Clients |-> NoBurst]
    /\ map = [mi \in MapIdx |-> NoMap]
    /\ free = [i \in 1..MapCap |-> i]          \* rocketnet_encryption_map_create
    /\ slots = [i \in SlotIdx |-> NoSlot] /\ kaDue = [i \in SlotIdx |-> FALSE]
    /\ history = <<>> /\ admitted = {}
    /\ sburst = [a \in Att |-> NoBurst]
    /\ admits = [t \in Tokens |-> 0] /\ misrouted = FALSE
    /\ ctrConn = 0 /\ ctrDisc = 0 /\ ctrTimed = 0
    /\ lost = {} /\ captured = {} /\ starved = FALSE
    /\ C2S!Init /\ S2C!Init

(***************************************************************************)
(* The clients.  The client's own machine is ClientConnection's; here it   *)
(* is what the server sees: the scheduled sends of each state, the         *)
(* disconnect burst, and what each server packet does to it.               *)
(***************************************************************************)
clientOnly(c) == UNCHANGED <<serverVars, ghostVars, c2sVars, s2cVars>>

\* rocketnet_client_protocol_connect: fresh attempt (salt, ephemeral,
\* token), state 1, the first update sends at once.  A vanished client is
\* gone for good.
Connect(c) ==
    /\ cst[c] \in {0, -1, -5} /\ catt[c] < MaxAttempts /\ cburst[c].left = 0
    /\ cst' = [cst EXCEPT ![c] = 1] /\ catt' = [catt EXCEPT ![c] = @ + 1]
    /\ cmoved' = [cmoved EXCEPT ![c] = FALSE] /\ ccookie' = [ccookie EXCEPT ![c] = FALSE]
    /\ ckey' = [ckey EXCEPT ![c] = NoKey] /\ cslot' = [cslot EXCEPT ![c] = 0]
    /\ cdue' = [cdue EXCEPT ![c] = TRUE] /\ cidle' = [cidle EXCEPT ![c] = 0]
    /\ UNCHANGED <<caddr, cburst>> /\ clientOnly(c)

\* the scheduled send of rocketnet_client_protocol_update: a request in 1
\* and 2 (carrying the cookie in 2), the stored response in 3, a keep-alive
\* in 4, once per send interval
ClientPacket(c) ==
    CASE cst[c] \in {1, 2} -> [type |-> "request", by |-> c, from |-> caddr[c],
                               att |-> MyAtt(c), cookie |-> ccookie[c]]
      [] cst[c] = 3 -> [type |-> "response", by |-> c, from |-> caddr[c], key |-> ckey[c]]
      [] cst[c] = 4 -> [type |-> "keepalive", by |-> c, from |-> caddr[c], key |-> ckey[c]]

ClientSend(c) ==
    /\ cst[c] \in 1..4 /\ cdue[c]
    /\ ClientSendPacket(ClientPacket(c))
    /\ cdue' = [cdue EXCEPT ![c] = FALSE]
    /\ captured' = IF Attacker /\ cst[c] = 3 THEN captured \cup {ClientPacket(c)} ELSE captured
    /\ UNCHANGED <<cst, catt, caddr, cmoved, ccookie, ckey, cslot, cidle, cburst>>
    /\ UNCHANGED <<serverVars, admits, misrouted, ctrConn, ctrDisc, ctrTimed, starved, s2cVars>>

\* rocketnet_client_protocol_disconnect: the burst from 3 or 4, then 0
ClientDisconnect(c) ==
    /\ ClientDisconnects /\ cst[c] \in 1..4
    /\ cburst' = [cburst EXCEPT ![c] = IF cst[c] \in {3, 4}
                                        THEN [left |-> DisconnectCopies, to |-> 0, key |-> ckey[c]]
                                        ELSE NoBurst]
    /\ cst' = [cst EXCEPT ![c] = 0] /\ ckey' = [ckey EXCEPT ![c] = NoKey]
    /\ cdue' = [cdue EXCEPT ![c] = FALSE]
    /\ UNCHANGED <<catt, caddr, cmoved, ccookie, cslot, cidle>> /\ clientOnly(c)

\* one copy of the burst, from wherever the client is now
ClientBurst(c) ==
    /\ cburst[c].left > 0
    /\ ClientSendPacket([type |-> "disconnect", by |-> c, from |-> caddr[c], key |-> cburst[c].key])
    /\ cburst' = [cburst EXCEPT ![c].left = @ - 1,
                                ![c].key = IF cburst[c].left = 1 THEN NoKey ELSE @]
    /\ UNCHANGED <<cst, catt, caddr, cmoved, ccookie, ckey, cslot, cdue, cidle>>
    /\ UNCHANGED <<serverVars, admits, misrouted, ctrConn, ctrDisc, ctrTimed, captured, starved, s2cVars>>

\* the outside: the client's process or network is gone, nothing more is sent
Vanish(c) ==
    /\ Vanishes /\ cst[c] \in 1..4
    /\ cst' = [cst EXCEPT ![c] = -9] /\ ckey' = [ckey EXCEPT ![c] = NoKey]
    /\ cburst' = [cburst EXCEPT ![c] = NoBurst] /\ cdue' = [cdue EXCEPT ![c] = FALSE]
    /\ UNCHANGED <<catt, caddr, cmoved, ccookie, cslot, cidle>> /\ clientOnly(c)

\* the outside: NAT rebinding moves the client to another address, which
\* may be one a departed client used; never one a present client holds.
\* The client does not know.
Move(c) ==
    /\ Moves /\ cst[c] \in 1..4 /\ ~cmoved[c]
    /\ \E a \in Addr \ {caddr[c]} :
        /\ At(a) = {}
        /\ caddr' = [caddr EXCEPT ![c] = a]
    /\ cmoved' = [cmoved EXCEPT ![c] = TRUE]
    /\ UNCHANGED <<cst, catt, ccookie, ckey, cslot, cdue, cidle, cburst>> /\ clientOnly(c)

(***************************************************************************)
(* A client receives the head of S2C if it is at the packet's address;     *)
(* with nobody there the packet is delivered to no one.  Every packet has  *)
(* a receive step in every state (what the gate or the keys refuse is      *)
(* discarded), so the head is always deliverable.  Gates and keys as       *)
(* sdk/rocketnet_client_protocol.cpp: cookie in 1 and 2 for this salt;     *)
(* challenge in 2 for this salt; denied in 2 (this salt) or 3 (the held    *)
(* keys); keep-alive and disconnect in 3 and 4 under the held keys.        *)
(***************************************************************************)
ClientRecv(c, p) ==
    LET me    == MyAtt(c)
        keyed == cst[c] \in {3, 4} /\ p.key = ckey[c]
    IN CASE p.type = "cookie" /\ cst[c] \in {1, 2} /\ p.att = me ->
              /\ ccookie' = [ccookie EXCEPT ![c] = TRUE] /\ cst' = [cst EXCEPT ![c] = 2]
              /\ cidle' = [cidle EXCEPT ![c] = 0]
              /\ UNCHANGED <<ckey, cslot>>
         [] p.type = "challenge" /\ cst[c] = 2 /\ p.key.att = me ->
              /\ ckey' = [ckey EXCEPT ![c] = p.key] /\ cst' = [cst EXCEPT ![c] = 3]
              /\ cidle' = [cidle EXCEPT ![c] = 0]
              /\ UNCHANGED <<ccookie, cslot>>
         [] p.type = "denied" /\ ((cst[c] = 2 /\ p.key.att = me) \/ (cst[c] = 3 /\ p.key = ckey[c])) ->
              /\ cst' = [cst EXCEPT ![c] = -1] /\ ckey' = [ckey EXCEPT ![c] = NoKey]
              /\ UNCHANGED <<ccookie, cslot, cidle>>
         [] p.type = "keepalive" /\ keyed ->
              /\ cst' = [cst EXCEPT ![c] = 4] /\ cslot' = [cslot EXCEPT ![c] = p.slot]
              /\ cidle' = [cidle EXCEPT ![c] = 0]
              /\ UNCHANGED <<ccookie, ckey>>
         [] p.type = "disconnect" /\ keyed ->
              /\ cst' = [cst EXCEPT ![c] = 0] /\ ckey' = [ckey EXCEPT ![c] = NoKey]
              /\ UNCHANGED <<ccookie, cslot, cidle>>
         [] OTHER -> UNCHANGED <<cst, ccookie, ckey, cslot, cidle>>

ClientRecvHead ==
    /\ s2cWire # <<>>
    /\ LET p == Head(s2cWire).pkt IN
         /\ S2C!Deliver(p)
         /\ IF At(p.to) = {}
              THEN UNCHANGED <<cst, ccookie, ckey, cslot, cidle>>
              ELSE ClientRecv(CHOOSE c \in At(p.to) : TRUE, p)
    /\ UNCHANGED <<catt, caddr, cmoved, cdue, cburst>>
    /\ UNCHANGED <<serverVars, ghostVars, c2sVars>>

(***************************************************************************)
(* The server receives the head of C2S: rocketnet_server_protocol_process_ *)
(* packet.  Each handler computes its outcome, then the state it writes    *)
(* and the one packet it answers with, if any (the transport commits at    *)
(* most one response frame per received packet).                           *)
(***************************************************************************)

\* remove a mapping: rocketnet_encryption_map_remove pushes the index on the
\* free list's head; the slot, if it names one, is released with it
RemoveMap(mi) ==
    /\ map' = [map EXCEPT ![mi] = NoMap]
    /\ free' = <<mi>> \o free

\* rocketnet_server_protocol_process_connection_request
ServerRecvRequest(p) ==
    LET a   == p.from
        mi  == MapAt(a)
        tok == TokOf(p.att)
        outcome ==
            CASE mi # 0 /\ map[mi].st = "est"        -> "ignore"       \* :91-101
              [] mi # 0                             -> IF ResendChallenge THEN "resend" ELSE "ignore" \* :102-106
              [] ~p.cookie                          -> "cookie"       \* :213-275
              [] HistBoundElsewhere(tok, a)         -> "ignore"       \* :283-288
              [] CidConnected(CidOf(tok))           -> "ignore"       \* :345-350
              [] tok \in admitted                   -> "ignore"       \* :352-357
              [] NumFree = 0                        -> "denied"       \* :359, 395-413
              [] free = <<>>                        -> "full"         \* :415-423
              [] FreshNonces = {}                   -> "nononce"      \* model bound only
              [] OTHER                              -> "create"       \* :415-470
        n   == Min(FreshNonces)
        nmi == Head(free)
        reply ==
            CASE outcome = "resend" -> [type |-> "challenge", to |-> a, key |-> map[mi].key]
              [] outcome = "cookie" -> [type |-> "cookie", to |-> a, att |-> p.att]
              [] outcome = "denied" -> [type |-> "denied", to |-> a, key |-> [att |-> p.att, nonce |-> 0]]
              [] outcome = "create" -> [type |-> "challenge", to |-> a, key |-> [att |-> p.att, nonce |-> n]]
    IN /\ history' = IF outcome \in {"denied", "full", "nononce", "create"} THEN HistRecord(tok, a) ELSE history
       /\ starved' = (starved \/ outcome = "nononce")
       /\ IF outcome = "create"
            THEN /\ map' = [map EXCEPT ![nmi] = [st |-> "half", addr |-> a,
                                                 key |-> [att |-> p.att, nonce |-> n],
                                                 tok |-> tok, slot |-> 0, age |-> 0, idle |-> 0]]
                 /\ free' = Tail(free)
            ELSE UNCHANGED <<map, free>>
       /\ IF outcome \in {"resend", "cookie", "denied", "create"}
            THEN ServerSendPacket(reply)
            ELSE UNCHANGED <<lost, s2cVars>>
       /\ UNCHANGED <<slots, kaDue, admitted, sburst, admits, misrouted, ctrConn, ctrDisc, ctrTimed>>

\* the sealed ladder for a response (rocketnet_server_process_sealed_packet,
\* then rocketnet_server_process_connection_response).  It opens only under
\* the mapping at its source address; once open it refreshes
\* last_receive_time whatever follows (:779).
ServerRecvResponse(p) ==
    LET a  == p.from
        mi == MapAt(a)
        m  == map[mi]
        opens == mi # 0 /\ m.key = p.key
        i  == LowestFree
        outcome ==
            CASE ~opens                          -> "ignore"    \* no mapping, or did not open
              [] m.st = "est"                    -> "refresh"   \* :491-496, F7
              [] CidConnected(CidOf(m.tok))      -> "refresh"   \* :498-503
              [] NumFree = 0                     -> "denied"    \* :514-519
              [] ~AdmitOK(m.tok)                 -> "denied"    \* :521-526
              [] OTHER                           -> "assign"    \* :546-576
    IN /\ misrouted' = (misrouted \/ (opens /\ p.by # m.key.att[1]))
       /\ CASE outcome = "ignore" ->
                 UNCHANGED <<map, free, slots, kaDue, admitted, admits, ctrConn, lost, s2cVars>>
            [] outcome \in {"refresh", "denied"} ->
                 /\ map' = [map EXCEPT ![mi].idle = 0]
                 /\ IF outcome = "denied"
                      THEN ServerSendPacket([type |-> "denied", to |-> a, key |-> m.key])
                      ELSE UNCHANGED <<lost, s2cVars>>
                 /\ UNCHANGED <<free, slots, kaDue, admitted, admits, ctrConn>>
            [] outcome = "assign" ->
                 /\ map' = [map EXCEPT ![mi].st = "est", ![mi].slot = i, ![mi].idle = 0]
                 /\ slots' = [slots EXCEPT ![i] = [held |-> TRUE, mi |-> mi,
                                                   cid |-> CidOf(m.tok), acked |-> FALSE]]
                 /\ kaDue' = [kaDue EXCEPT ![i] = FALSE]
                 /\ admitted' = admitted \cup {m.tok}
                 /\ admits' = [admits EXCEPT ![m.tok] = @ + 1]
                 /\ ctrConn' = ctrConn + 1
                 \* the first keep-alive: how the client learns its index
                 /\ ServerSendPacket([type |-> "keepalive", to |-> a, key |-> m.key, slot |-> i])
                 /\ UNCHANGED free
       /\ UNCHANGED <<history, sburst, ctrDisc, ctrTimed, starved>>

\* a session packet from a client: no mapping or did not open -> ignored;
\* a half-open mapping -> ignored as not connected (:791-801); a keep-alive
\* for a slot refreshes it and its ack block sets the latch (ack.cpp:385);
\* a disconnect releases the slot and removes the mapping (:938-942)
ServerRecvSession(p) ==
    LET mi == MapAt(p.from)
        m  == map[mi]
        accepted == mi # 0 /\ (m.key = p.key \/ ~KeyCheck) /\ m.st = "est"
    IN /\ misrouted' = (misrouted \/ (accepted /\ p.by # m.key.att[1]))
       /\ IF accepted /\ p.type = "keepalive"
            THEN /\ map' = [map EXCEPT ![mi].idle = 0]
                 /\ slots' = [slots EXCEPT ![m.slot].acked = TRUE]
                 /\ UNCHANGED <<free, ctrDisc>>
            ELSE IF accepted /\ p.type = "disconnect"
            THEN /\ RemoveMap(mi)
                 /\ slots' = [slots EXCEPT ![m.slot] = NoSlot]
                 /\ ctrDisc' = ctrDisc + 1
            ELSE UNCHANGED <<map, free, slots, ctrDisc>>
       /\ UNCHANGED <<kaDue, history, admitted, sburst, admits, ctrConn, ctrTimed, lost, s2cVars, starved>>

ServerRecv(p) ==
    CASE p.type = "request"  -> ServerRecvRequest(p)
      [] p.type = "response" -> ServerRecvResponse(p)
      [] OTHER               -> ServerRecvSession(p)

ServerRecvHead ==
    /\ c2sWire # <<>>
    /\ LET p == Head(c2sWire).pkt IN C2S!Deliver(p) /\ ServerRecv(p)
    /\ UNCHANGED <<clientVars, captured>>

(***************************************************************************)
(* The server's own sends.                                                 *)
(***************************************************************************)

\* the maintenance loop of rocketnet_server_transport_update
\* (sdk/rocketnet_server_transport.cpp:1151-1172) and
\* rocketnet_server_protocol_write_keep_alive_if_due: for a held slot, a
\* keep-alive to the mapping's address once per interval -- until one is
\* acknowledged off its own clock, after that off the last send of anything
\* (no payloads here, so the same interval).  RepeatKeepAlive = FALSE is the
\* witness: nothing after the first until the client acknowledges one.
ServerKeepAlive(i) ==
    /\ slots[i].held /\ kaDue[i]
    /\ RepeatKeepAlive \/ slots[i].acked
    /\ LET m == map[slots[i].mi] IN
         ServerSendPacket([type |-> "keepalive", to |-> m.addr, key |-> m.key, slot |-> i])
    /\ kaDue' = [kaDue EXCEPT ![i] = FALSE]
    /\ UNCHANGED <<clientVars, map, free, slots, history, admitted, sburst>>
    /\ UNCHANGED <<admits, misrouted, ctrConn, ctrDisc, ctrTimed, captured, starved, c2sVars>>

\* rocketnet_server_protocol_disconnect_client: the burst to the mapping's
\* address under its keys, then the slot released and the mapping removed
ServerDisconnect(i) ==
    /\ ServerDisconnects /\ slots[i].held
    /\ LET mi == slots[i].mi
           m  == map[mi]
       IN /\ sburst[m.key.att].left = 0
          /\ sburst' = [sburst EXCEPT ![m.key.att] = [left |-> DisconnectCopies, to |-> m.addr, key |-> m.key]]
          /\ RemoveMap(mi)
    /\ slots' = [slots EXCEPT ![i] = NoSlot] /\ kaDue' = [kaDue EXCEPT ![i] = FALSE]
    /\ ctrDisc' = ctrDisc + 1
    /\ UNCHANGED <<clientVars, history, admitted, admits, misrouted, ctrConn, ctrTimed>>
    /\ UNCHANGED <<lost, captured, starved, c2sVars, s2cVars>>

ServerBurst(a) ==
    /\ sburst[a].left > 0
    /\ ServerSendPacket([type |-> "disconnect", to |-> sburst[a].to, key |-> sburst[a].key])
    /\ sburst' = [sburst EXCEPT ![a] = IF @.left = 1 THEN NoBurst ELSE [@ EXCEPT !.left = @ - 1]]
    /\ UNCHANGED <<clientVars, map, free, slots, kaDue, history, admitted>>
    /\ UNCHANGED <<admits, misrouted, ctrConn, ctrDisc, ctrTimed, captured, starved, c2sVars>>

\* an on-path attacker re-injects a captured response from its source
\* address (handshake types are exempt from replay protection, S:1475)
Replay ==
    /\ Attacker
    /\ \E p \in captured : C2S!Send(p)
    /\ UNCHANGED <<clientVars, serverVars, ghostVars, s2cVars>>

(***************************************************************************)
(* Time.  The clients: the send interval, and the idle timeout to -5.      *)
(* rocketnet_server_protocol_update: walking the map in index order, a     *)
(* half-open mapping is removed at its handshake deadline, an established *)
(* one at the idle timeout with its slot (:1031-1060); each removal pushes *)
(* its index on the free list, so the last removed is the next reused.     *)
(* The keep-alive interval elapses for every held slot.                    *)
(***************************************************************************)
RECURSIVE PushAsc(_, _)
PushAsc(fl, S) == IF S = {} THEN fl ELSE LET m == Min(S) IN PushAsc(<<m>> \o fl, S \ {m})

Tick ==
    /\ C2S!Age /\ S2C!Age
    /\ LET active(c) == cst[c] \in 1..4
           idle1(c)  == Sat(cidle[c], ClientTimeout)
           timed(c)  == active(c) /\ ClientTimeout > 0 /\ idle1(c) = ClientTimeout
       IN /\ cst'   = [c \in Clients |-> IF timed(c) THEN -5 ELSE cst[c]]
          /\ ckey'  = [c \in Clients |-> IF timed(c) THEN NoKey ELSE ckey[c]]
          /\ cidle' = [c \in Clients |-> IF active(c) THEN idle1(c) ELSE 0]
          /\ cdue'  = [c \in Clients |-> active(c) /\ ~timed(c)]
    /\ LET age1(mi)  == Sat(map[mi].age, HsDeadline)
           idle1(mi) == Sat(map[mi].idle, Timeout)
           expired   == {mi \in MapIdx : map[mi].st = "half" /\ HsDeadline > 0 /\ age1(mi) = HsDeadline}
           timedOut  == {mi \in MapIdx : map[mi].st = "est" /\ Timeout > 0 /\ idle1(mi) = Timeout}
           gone      == expired \cup (IF ReleaseMapping THEN timedOut ELSE {})
       IN /\ map' = [mi \in MapIdx |->
                       IF mi \in gone THEN NoMap
                       ELSE IF Live(mi) THEN [map[mi] EXCEPT !.age = age1(mi), !.idle = idle1(mi)]
                       ELSE map[mi]]
          /\ free' = PushAsc(free, gone)
          /\ slots' = [i \in SlotIdx |-> IF slots[i].held /\ slots[i].mi \in timedOut
                                           THEN NoSlot ELSE slots[i]]
          /\ kaDue' = [i \in SlotIdx |-> slots[i].held /\ slots[i].mi \notin timedOut]
          /\ ctrTimed' = ctrTimed + Cardinality(timedOut)
    /\ UNCHANGED <<catt, caddr, cmoved, ccookie, cslot, cburst>>
    /\ UNCHANGED <<history, admitted, sburst, admits, misrouted, ctrConn, ctrDisc, lost, captured, starved>>

C2SEnvironment == C2S!Environment /\ UNCHANGED <<clientVars, serverVars, ghostVars, s2cVars>>
S2CEnvironment == S2C!Environment /\ UNCHANGED <<clientVars, serverVars, ghostVars, c2sVars>>

Next ==
    \/ \E c \in Clients : Connect(c) \/ ClientSend(c) \/ ClientDisconnect(c) \/ ClientBurst(c)
                          \/ Vanish(c) \/ Move(c)
    \/ ClientRecvHead \/ ServerRecvHead
    \/ \E i \in SlotIdx : ServerKeepAlive(i) \/ ServerDisconnect(i)
    \/ \E a \in Att : ServerBurst(a)
    \/ Replay
    \/ Tick
    \/ C2SEnvironment \/ S2CEnvironment

(***************************************************************************)
(* Fairness, as ClientConnection states it (README-ClientConnection.md,    *)
(* "Fairness"): time passes; the applications connect; every timer-driven  *)
(* send goes out, and one enabled infinitely often with the pattern        *)
(* letting it through goes through (Link's EventualDelivery stated on the  *)
(* sender); the head of each wire is delivered (Link's WireFair on the     *)
(* head).  For the deterministic patterns on a FIFO link.  Nothing obliges *)
(* an application to disconnect, a client to vanish or move, or the        *)
(* attacker to act.                                                        *)
(***************************************************************************)
ClientSendThrough(c)    == ClientSend(c) /\ c2sWire' # c2sWire
ServerKeepAliveThrough(i) == ServerKeepAlive(i) /\ s2cWire' # s2cWire

Fairness ==
    /\ WF_vars(Tick)
    /\ \A c \in Clients : /\ WF_vars(Connect(c))
                          /\ WF_vars(ClientSend(c)) /\ SF_vars(ClientSendThrough(c))
                          /\ WF_vars(ClientBurst(c))
    /\ \A i \in SlotIdx : WF_vars(ServerKeepAlive(i)) /\ SF_vars(ServerKeepAliveThrough(i))
    /\ \A a \in Att : WF_vars(ServerBurst(a))
    /\ SF_vars(ClientRecvHead) /\ SF_vars(ServerRecvHead)

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* Safety.                                                                 *)
(***************************************************************************)
MapRec  == [st : {"none", "half", "est"}, addr : Addr \cup {0}, key : Key \cup {NoKey},
            tok : Tokens \cup {<<0, 0>>}, slot : 0..Slots, age : 0..HsDeadline, idle : 0..Timeout]
SlotRec == [held : BOOLEAN, mi : 0..MapCap, cid : Clients \cup {0}, acked : BOOLEAN]
BurstRec == [left : 0..DisconnectCopies, to : Addr \cup {0}, key : Key \cup {NoKey}]

TypeOK ==
    /\ cst \in [Clients -> CStates] /\ catt \in [Clients -> 0..MaxAttempts]
    /\ caddr \in [Clients -> Addr] /\ cmoved \in [Clients -> BOOLEAN]
    /\ ccookie \in [Clients -> BOOLEAN] /\ ckey \in [Clients -> Key \cup {NoKey}]
    /\ cslot \in [Clients -> 0..Slots] /\ cdue \in [Clients -> BOOLEAN]
    /\ cidle \in [Clients -> 0..ClientTimeout] /\ cburst \in [Clients -> BurstRec]
    /\ map \in [MapIdx -> MapRec] /\ slots \in [SlotIdx -> SlotRec]
    /\ kaDue \in [SlotIdx -> BOOLEAN]
    /\ admitted \subseteq Tokens /\ Len(history) <= HistCap
    /\ sburst \in [Att -> BurstRec]
    /\ admits \in [Tokens -> Nat] /\ misrouted \in BOOLEAN /\ starved \in BOOLEAN
    /\ C2S!TypeOK /\ S2C!TypeOK

\* the model's own bounds were never what stopped it
ModelBoundsOK == ~starved

\* the encryption map's allocator: the free list is exactly the free
\* indices, each once, and the table never holds more than its capacity
FreeListExact ==
    /\ {free[k] : k \in DOMAIN free} = {mi \in MapIdx : ~Live(mi)}
    /\ Len(free) = Cardinality({mi \in MapIdx : ~Live(mi)})

\* the address is the connection identity: one mapping per address
AddressIdentity ==
    \A m1, m2 \in MapIdx : (Live(m1) /\ Live(m2) /\ m1 # m2) => map[m1].addr # map[m2].addr

\* no slot without its mapping while live, no established mapping without
\* its slot, a half-open mapping holds none (S:2829, S:2852; the leak the
\* audits suspect at the 2x mapping table)
SlotMappingBijection ==
    /\ \A i \in SlotIdx : slots[i].held =>
          /\ slots[i].mi \in MapIdx /\ map[slots[i].mi].st = "est"
          /\ map[slots[i].mi].slot = i
          /\ slots[i].cid = CidOf(map[slots[i].mi].tok)
    /\ \A mi \in MapIdx :
          /\ map[mi].st = "est" => (map[mi].slot \in SlotIdx /\ slots[map[mi].slot].mi = mi
                                    /\ slots[map[mi].slot].held)
          /\ map[mi].st = "half" => map[mi].slot = 0
    /\ \A i \in SlotIdx : ~slots[i].held => slots[i] = NoSlot

\* no two live clients in one slot: on the server one slot names one
\* mapping, and one client id holds at most one slot (S:2647); on the
\* clients, at most one client in state 4 holds the keys of the mapping a
\* slot names
NoTwoClientsInOneSlot ==
    /\ \A i, j \in SlotIdx : (i # j /\ slots[i].held /\ slots[j].held) =>
          (slots[i].mi # slots[j].mi /\ slots[i].cid # slots[j].cid)
    /\ \A i \in SlotIdx : slots[i].held =>
          Cardinality({c \in Clients : cst[c] = 4 /\ ckey[c] = map[slots[i].mi].key}) <= 1

\* the same token never admits two connections (S:2649, S:3591 "a connect
\* token buys exactly one connection")
OneAdmissionPerToken == \A t \in Tokens : admits[t] <= 1

\* a client's packets never reach another client's mapping or slot, after
\* an address change or otherwise
NoCrossDelivery == ~misrouted

\* the ledger of the transport's counters (server_protocol.cpp:589-592)
Ledger == ctrConn = ctrDisc + ctrTimed + NumHeld

\* a released slot takes its mapping with it, in the same step
ReleaseRemovesMapping ==
    [][\A i \in SlotIdx :
         (slots[i].held /\ ~slots'[i].held) => map'[slots[i].mi].st = "none"]_vars

\* a client told it is denied holds no slot (TLA-INVENTORY F5: expected to
\* fail; a run of its own)
DeniedMeansNoSlot ==
    \A c \in Clients : cst[c] = -1 =>
        ~\E i \in SlotIdx : slots[i].held /\ map[slots[i].mi].key.att = MyAtt(c)

(***************************************************************************)
(* Liveness.                                                               *)
(***************************************************************************)

\* an attempt is gone when its client has ended it, vanished, or moved off
\* the address it connected from; gone is stable
Gone(a) == ~(catt[a[1]] = a[2] /\ cst[a[1]] \in 1..4 /\ ~cmoved[a[1]])

HoldsSlot(a) == \E i \in SlotIdx : slots[i].held /\ map[slots[i].mi].key.att = a

\* a timed-out slot is freed: a slot whose client is gone is released
SlotFreed == \A a \in Att : (HoldsSlot(a) /\ Gone(a)) ~> ~HoldsSlot(a)

\* a half-open mapping leaves half-open (by its handshake deadline)
HalfBounded == \A mi \in MapIdx : (map[mi].st = "half") ~> (map[mi].st # "half")

\* the hang check: with nobody leaving, every attempt resolves (connected or
\* denied), and every slot's establishing keep-alive is acknowledged --
\* neither peer is left waiting for a packet nobody will send again
Connects == \A c \in Clients : (cst[c] = 1) ~> (cst[c] \in {4, -1})

ServerNotStranded ==
    /\ \A i \in SlotIdx : (slots[i].held /\ ~slots[i].acked) ~> (~slots[i].held \/ slots[i].acked)
    /\ HalfBounded

=============================================================================
