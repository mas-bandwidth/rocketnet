----------------------------- MODULE Security -----------------------------
(***************************************************************************)
(* rocketnet: the security machines, with an attacker in the model.        *)
(*                                                                         *)
(* The connect token lifecycle (issued, presented, admitted, expired,      *)
(* replayed), the prefilter and the cookie gate, the used-token history    *)
(* and the admitted-token set, the per-mapping key exchange (salt, session *)
(* nonce, ephemeral pair), and the session replay window at the            *)
(* representation level (bits grouped in words), as                        *)
(* sdk/rocketnet_server_protocol.cpp, sdk/rocketnet_token_tables.cpp and   *)
(* sdk/rocketnet_replay.cpp write them and STANDARD.md specifies them      *)
(* (Keys, The Prefilter Header, Connect Token, Connect token sharing,      *)
(* Per-connection keys, The Handshake, Replay Protection, Processing       *)
(* Connection Requests, Processing Connection Response Packets).           *)
(*                                                                         *)
(* Every packet crosses the shared lossy link tla/Link.tla: C2S carries    *)
(* what any source sends to the server, S2C what the server sends.  One    *)
(* honest client at address HonestAddr runs the handshake the way          *)
(* sdk/rocketnet_client_protocol.cpp does, as far as the security machines *)
(* see it: request every tick in 1 and 2 (with the stored cookie in 2),    *)
(* the response every tick in 3, a keep-alive every tick in 4.  The        *)
(* attacker sits on the path: it sees every packet sent, may re-send any    *)
(* sealed packet it has seen from any source address, may                  *)
(* present any token it holds (a forged one, one sealed to another server, *)
(* an expired one, the honest client's captured one) with any cookie it    *)
(* has seen, no cookie, or a forged cookie, and may send unkeyed junk, as  *)
(* often as the link's Capacity allows (the flood).  It cannot sign, seal  *)
(* or open anything: it never holds a token's keys.  Replay of a packet in *)
(* flight toward either peer is Link's Duplicate.                          *)
(*                                                                         *)
(* Abstractions.  A token is a name with four attributes (signed, sealed   *)
(* to this server, client id, expiry).  Keys are the pair [tok, nonce]:    *)
(* the token whose keys the derivation is keyed with and the session nonce *)
(* the server drew for the mapping.  A sealed packet opens iff the         *)
(* receiver holds that pair: the server for its mapping, the honest client *)
(* for HonestToken and the nonce it committed.  Nobody else holds a        *)
(* token's keys.  A cookie is [addr, exp, gen]: the MAC is                 *)
(* the attacker's inability to make one.  The salt is per attempt and the  *)
(* honest client makes one attempt, so it is not a variable.  The server's *)
(* S2C replay window, the integrity limit (2^36), the client's deadlines,  *)
(* ack blocks and payloads are outside this module.  Time is a saturating  *)
(* clock in ticks; one tick is one send interval and one second of the     *)
(* token clock.                                                            *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    None, Zero, Bad,  \* model values: no entry; the zero cookie field; a cookie field that is not a MAC the server made
    Addrs,            \* the source addresses (strings); HonestAddr \in Addrs
    HonestAddr,       \* the honest client's address
    Tokens,           \* token names
    Signed,           \* SUBSET Tokens: carries the backend's signature
    ForThisServer,    \* SUBSET Tokens: sealed to this server's key
    Cid,              \* [Tokens -> Nat]: the client id in the sealed block
    Expire,           \* [Tokens -> Nat]: expire timestamp; 0 = never
    HonestToken,      \* the token the honest client presents
    MaxClock,         \* the clock saturates here
    CookieLife,       \* cookie lifetime, ticks (3 s in the code)
    Deadline,         \* the handshake deadline for a half-open mapping; 0 = off
    Timeout,          \* the idle timeout for an established mapping; 0 = off
    MaxClients,       \* client slots
    MapCap,           \* encryption mappings (2 x MaxClients in the code)
    HistoryCap,       \* used-token history (8 x MaxClients in the code)
    AdmittedCap,      \* admitted-token set (2 x MaxClients in the code)
    MaxNonces,        \* the pool of session nonces the server may draw
    Others,           \* other clients holding slots at the start
    OthersMayJoin,    \* BOOLEAN: may another client take a free slot
    OthersMayLeave,   \* BOOLEAN: may another client release a slot
    Attacker,         \* BOOLEAN: the attacker acts
    Rotation,         \* BOOLEAN: the cookie key rotates (the code: FALSE, F2)
    RotatePeriod,     \* ticks between rotations when Rotation
    AcceptPrevious,   \* BOOLEAN: cookies under the previous key verify
    Words, WordBits,  \* the replay window: Words x WordBits bits (64 x 64 in the code)
    ClearWords,       \* words zeroed on a skip >= W (4 in the code, F1)
    MaxSeq,           \* the honest client's session sequences run 1..MaxSeq
    RepeatKeepAlive,  \* the establishing keep-alive is re-sent on the timer until confirmed (the code, approximately: TRUE; see ServerKeepAlive)
    KindLoss,         \* "none" or "firstkind": each sender's first send of each type is lost
    C2SPattern, S2CPattern,                       \* Link patterns
    Capacity, MaxDelay, Duplication, Reordering,  \* Link parameters
    BurstStart, BurstLen

ASSUME /\ HonestAddr \in Addrs
       /\ Signed \subseteq Tokens /\ ForThisServer \subseteq Tokens
       /\ HonestToken \in Signed \cap ForThisServer
       /\ MaxClock \in Nat /\ CookieLife \in Nat \ {0}
       /\ Deadline \in Nat /\ Timeout \in Nat
       /\ MaxClients \in Nat \ {0} /\ MapCap \in Nat \ {0}
       /\ HistoryCap \in Nat \ {0} /\ AdmittedCap \in Nat \ {0}
       /\ MaxNonces \in Nat \ {0}
       /\ Others \in 0..MaxClients
       /\ OthersMayJoin \in BOOLEAN /\ OthersMayLeave \in BOOLEAN
       /\ Attacker \in BOOLEAN /\ Rotation \in BOOLEAN /\ AcceptPrevious \in BOOLEAN
       /\ RotatePeriod \in Nat \ {0}
       /\ Words \in Nat \ {0} /\ WordBits \in Nat \ {0}
       /\ ClearWords \in 1..Words
       /\ MaxSeq \in Nat \ {0}
       /\ RepeatKeepAlive \in BOOLEAN
       /\ KindLoss \in {"none", "firstkind"}

Nonces == 1..MaxNonces
Seqs   == 1..MaxSeq
Gens   == 1..3           \* cookie key generations; wraps 3 -> 1 (255 -> 1 in the keyed form)
Times  == 0..(MaxClock + CookieLife)

(***************************************************************************)
(* Packets.                                                                *)
(***************************************************************************)

\* A cookie the server issued: bound to the address (the MAC covers the
\* source address, the salt and the expiry under the cookie key of
\* generation gen).  rocketnet_generate_cookie_mac.
CookieVals == [addr : Addrs, exp : Times, gen : Gens]

\* The request's return cookie field: zero, a cookie, or bytes that are not
\* a MAC the server made (Bad).
CookieField == {Zero, Bad} \cup CookieVals

\* 0 = never: a token that never expires outlives the saturated clock
ExpTime(t) == IF Expire[t] = 0 THEN MaxClock + 1 ELSE Expire[t]

Request   == [type : {"request"}, from : Addrs, tok : Tokens, cookie : CookieField]
Junk      == [type : {"junk"}, from : Addrs, filt : BOOLEAN]
Response  == [type : {"response"}, from : Addrs, tok : Tokens, nonce : Nonces]
CKeepAlive == [type : {"keepalive"}, from : Addrs, tok : Tokens, nonce : Nonces, seq : Seqs]

ClientPackets == Request \cup Junk \cup Response \cup CKeepAlive

CookiePkt  == [type : {"cookie"}, to : Addrs, exp : Times, gen : Gens]
Challenge  == [type : {"challenge"}, to : Addrs, tok : Tokens, nonce : Nonces]
Denied     == [type : {"denied"}, to : Addrs, tok : Tokens, nonce : Nonces]
SKeepAlive == [type : {"keepalive"}, to : Addrs, tok : Tokens, nonce : Nonces]

ServerPackets == CookiePkt \cup Challenge \cup Denied \cup SKeepAlive

ClientKinds == {"request", "response", "keepalive"}
ServerKinds == {"cookie", "challenge", "denied", "keepalive"}

(***************************************************************************)
(* Variables.                                                              *)
(***************************************************************************)
VARIABLES
    \* the honest client (sdk/rocketnet_client_protocol.cpp)
    cstate,     \* 0 idle, 1 requesting cookie, 2 sending request, 3 sending response, 4 connected, -1 denied, -7 token expired
    ccookie,    \* 0 or the stored cookie (has_cookie, return_cookie)
    cnonce,     \* 0 or the session nonce the challenge carried (keys committed)
    cseq,       \* the next session sequence the client sends
    cdue,       \* a send is due this tick
    cfirst,     \* kinds the client has sent at least once (KindLoss)

    \* the server (sdk/rocketnet_server_protocol.cpp, rocketnet_token_tables.cpp)
    map,        \* [Addrs -> mapping or 0]: the encryption mappings
    slots,      \* the client slots: [cid : Nat, tok : Tokens, mapAddr : Addrs, confirmed : BOOLEAN], as a set
    others,     \* slots held by clients outside the model
    history,    \* [Tokens -> entry or 0]: the used-token history (addr, exp, order)
    admitted,   \* [Tokens -> exp or 0]: the admitted-token set
    insertOrder,\* the history's next insert order
    nonceCtr,   \* nonces drawn so far
    gen,        \* the current cookie key generation
    hasPrev,    \* a previous cookie key is held
    sdue,       \* the server's keep-alive timer fired this tick
    sfirst,     \* kinds the server has sent at least once (KindLoss)

    \* the attacker
    seenCookies, \* [Addrs -> cookie or None]: the latest cookie it has seen for each address
    seenSealed,  \* sealed client packets it has seen, without their source: [type, nonce, seq]

    \* time and the two links
    clock,
    c2sWire, c2sSent, c2sPulse,
    s2cWire, s2cSent, s2cPulse,

    \* ghosts for the properties
    admits,     \* [Tokens -> Nat]: how many connections each token has admitted
    lateAdmit,  \* a slot was assigned under an expired token
    cookieProven, \* SUBSET Addrs: addresses whose cookie the server has verified
    replayAccepted, \* a session packet whose sequence was already opened was accepted
    deniedPulse     \* flips on every step that sends a denied

clientVars == <<cstate, ccookie, cnonce, cseq, cdue, cfirst>>
serverVars == <<map, slots, others, history, admitted, insertOrder, nonceCtr, gen, hasPrev, sdue, sfirst>>
ghostVars  == <<admits, lateAdmit, cookieProven, replayAccepted, deniedPulse>>
c2sVars    == <<c2sWire, c2sSent, c2sPulse>>
s2cVars    == <<s2cWire, s2cSent, s2cPulse>>
attackVars == <<seenCookies, seenSealed>>
vars       == <<clientVars, serverVars, attackVars, clock, c2sVars, s2cVars, ghostVars>>

C2S == INSTANCE Link WITH wire <- c2sWire, sent <- c2sSent, pulse <- c2sPulse,
                          Packets <- ClientPackets, Pattern <- C2SPattern
S2C == INSTANCE Link WITH wire <- s2cWire, sent <- s2cSent, pulse <- s2cPulse,
                          Packets <- ServerPackets, Pattern <- S2CPattern

(***************************************************************************)
(* The replay window, as sdk/rocketnet_replay.cpp stores it: W = Words x   *)
(* WordBits bits in a ring, grouped in words; highest_opened beside it.    *)
(* win is [0..Words-1 -> SUBSET (0..WordBits-1)]: the set bits per word.   *)
(***************************************************************************)
W == Words * WordBits
WordOf(s) == (s % W) \div WordBits
BitOf(s)  == (s % W) % WordBits
ZeroWin   == [w \in 0..Words-1 |-> {}]

HasBit(win, s)   == BitOf(s) \in win[WordOf(s)]
SetBit(win, s)   == [win EXCEPT ![WordOf(s)] = @ \cup {BitOf(s)}]
ClearBit(win, s) == [win EXCEPT ![WordOf(s)] = @ \ {BitOf(s)}]

\* rocketnet_replay_already_received
AlreadyReceived(hi, win, s) ==
    IF s > hi THEN FALSE
    ELSE IF hi >= W /\ s <= hi - W THEN TRUE
    ELSE HasBit(win, s)

RECURSIVE ClearRange(_, _, _)
ClearRange(win, lo, hi) == IF lo > hi THEN win ELSE ClearRange(ClearBit(win, lo), lo + 1, hi)

\* rocketnet_replay_advance: the window after opening s
Advanced(hi, win, s) ==
    IF s > hi
      THEN LET skipped == s - hi
               cleared == IF skipped >= W
                            THEN [w \in 0..Words-1 |-> IF w < ClearWords THEN {} ELSE win[w]]
                            ELSE ClearRange(win, hi + 1, s - 1)
           IN SetBit(cleared, s)
      ELSE SetBit(win, s)

(***************************************************************************)
(* Mappings and table entries.                                             *)
(***************************************************************************)
Mapping == [est : BOOLEAN, tok : Tokens, nonce : Nonces, deadline : Nat, lastRecv : Nat,
            hi : Nat, win : [0..Words-1 -> SUBSET (0..WordBits-1)], opened : SUBSET Seqs]

HistEntry == [addr : Addrs, exp : Nat, order : Nat]

Live(a)  == map[a] # None
Half(a)  == Live(a) /\ ~map[a].est
Est(a)   == Live(a) /\ map[a].est
NumMappings == Cardinality({a \in Addrs : Live(a)})

TokExpired(t) == ExpTime(t) <= clock

\* an expired entry reads as absent (rocketnet_token_table_find)
HistLive(t) == history[t] # None /\ history[t].exp > clock
AdmLive(t)  == admitted[t] # 0 /\ admitted[t] > clock
HistCount   == Cardinality({t \in Tokens : history[t] # None})
AdmCount    == Cardinality({t \in Tokens : admitted[t] # 0})

CidConnected(c) == \E s \in slots : s.cid = c
NumFree == MaxClients - Cardinality(slots) - others

SatClock(c) == IF c < MaxClock THEN c + 1 ELSE c

(***************************************************************************)
(* Init.                                                                   *)
(***************************************************************************)
Init ==
    /\ cstate = 0 /\ ccookie = Zero /\ cnonce = 0 /\ cseq = 1 /\ cdue = FALSE /\ cfirst = {}
    /\ map = [a \in Addrs |-> None]
    /\ slots = {} /\ others = Others
    /\ history = [t \in Tokens |-> None] /\ admitted = [t \in Tokens |-> 0] /\ insertOrder = 0
    /\ nonceCtr = 0 /\ gen = 1 /\ hasPrev = FALSE /\ sdue = FALSE /\ sfirst = {}
    /\ seenCookies = [a \in Addrs |-> None] /\ seenSealed = {}
    /\ clock = 0
    /\ C2S!Init /\ S2C!Init
    /\ admits = [t \in Tokens |-> 0] /\ lateAdmit = FALSE /\ cookieProven = {}
    /\ replayAccepted = FALSE /\ deniedPulse = FALSE

(***************************************************************************)
(* Sending.  KindLoss "firstkind" loses each sender's first send of each    *)
(* type before it reaches the link (the request named this terrain; Link   *)
(* cannot express it without reading packets).  The attacker sees every    *)
(* packet handed to either link.                                           *)
(***************************************************************************)
Observe(p) ==
    IF ~Attacker THEN UNCHANGED attackVars
    ELSE IF p.type = "response" /\ "from" \in DOMAIN p
      THEN seenSealed' = seenSealed \cup {[type |-> "response", tok |-> p.tok, nonce |-> p.nonce, seq |-> 0]} /\ UNCHANGED seenCookies
    ELSE IF p.type = "keepalive" /\ "from" \in DOMAIN p
      THEN seenSealed' = seenSealed \cup {[type |-> "keepalive", tok |-> p.tok, nonce |-> p.nonce, seq |-> p.seq]} /\ UNCHANGED seenCookies
    ELSE IF p.type = "cookie"
      THEN seenCookies' = [seenCookies EXCEPT ![p.to] = [addr |-> p.to, exp |-> p.exp, gen |-> p.gen]] /\ UNCHANGED seenSealed
    ELSE UNCHANGED attackVars

Track(first, p) == IF KindLoss = "firstkind" THEN first \cup {p.type} ELSE first

ClientSendPkt(p) ==
    /\ IF KindLoss = "firstkind" /\ p.type \notin cfirst
         THEN UNCHANGED <<c2sVars, attackVars>>
         ELSE C2S!Send(p) /\ Observe(p)
    /\ cfirst' = Track(cfirst, p)

ServerSendPkt(p) ==
    /\ IF KindLoss = "firstkind" /\ p.type \notin sfirst
         THEN UNCHANGED <<s2cVars, attackVars>>
         ELSE S2C!Send(p) /\ Observe(p)
    /\ sfirst' = Track(sfirst, p)

ServerSendsNothing == UNCHANGED <<s2cVars, sfirst, attackVars>>

(***************************************************************************)
(* The honest client.                                                      *)
(***************************************************************************)

\* rocketnet_client_protocol_connect: the pre-send checks refuse an expired
\* token (-7); otherwise state 1 with no cookie.
Connect ==
    /\ cstate = 0
    /\ IF TokExpired(HonestToken)
         THEN cstate' = -7 /\ UNCHANGED cdue
         ELSE cstate' = 1 /\ cdue' = TRUE
    /\ UNCHANGED <<ccookie, cnonce, cseq, cfirst, serverVars, attackVars, clock, c2sVars, s2cVars, ghostVars>>

\* rocketnet_client_protocol_update: the scheduled send
ClientSend ==
    /\ cdue /\ cstate \in {1, 2, 3, 4}
    /\ cdue' = FALSE
    /\ CASE cstate \in {1, 2} ->
              /\ ClientSendPkt([type |-> "request", from |-> HonestAddr, tok |-> HonestToken, cookie |-> ccookie])
              /\ UNCHANGED cseq
         [] cstate = 3 ->
              /\ ClientSendPkt([type |-> "response", from |-> HonestAddr, tok |-> HonestToken, nonce |-> cnonce])
              /\ UNCHANGED cseq
         [] cstate = 4 ->
              /\ cseq \in Seqs
              /\ ClientSendPkt([type |-> "keepalive", from |-> HonestAddr, tok |-> HonestToken, nonce |-> cnonce, seq |-> cseq])
              /\ cseq' = cseq + 1
    /\ UNCHANGED <<cstate, ccookie, cnonce, serverVars, clock, s2cVars, ghostVars>>

\* rocketnet_client_process_cookie: in 1 or 2, store (replacing), 1 -> 2
ClientRecvCookie == \E p \in CookiePkt :
    /\ p.to = HonestAddr
    /\ S2C!Deliver(p)
    /\ IF cstate \in {1, 2}
         THEN /\ ccookie' = [addr |-> HonestAddr, exp |-> p.exp, gen |-> p.gen]
              /\ cstate' = 2
         ELSE UNCHANGED <<ccookie, cstate>>
    /\ UNCHANGED <<cnonce, cseq, cdue, cfirst, serverVars, attackVars, clock, c2sVars, ghostVars>>

\* rocketnet_client_process_challenge: in 2 only; the nonce commits the keys
ClientRecvChallenge == \E p \in Challenge :
    /\ p.to = HonestAddr
    /\ S2C!Deliver(p)
    /\ IF cstate = 2 /\ p.tok = HonestToken
         THEN cnonce' = p.nonce /\ cstate' = 3
         ELSE UNCHANGED <<cnonce, cstate>>
    /\ UNCHANGED <<ccookie, cseq, cdue, cfirst, serverVars, attackVars, clock, c2sVars, ghostVars>>

\* rocketnet_client_process_denied: in 2 or 3; sealed under the handshake
\* keys for this attempt's token and the carried nonce, which only the
\* server can make, so a denied under the client's token opens
ClientRecvDenied == \E p \in Denied :
    /\ p.to = HonestAddr
    /\ S2C!Deliver(p)
    /\ IF cstate \in {2, 3} /\ p.tok = HonestToken THEN cstate' = -1 ELSE UNCHANGED cstate
    /\ UNCHANGED <<ccookie, cnonce, cseq, cdue, cfirst, serverVars, attackVars, clock, c2sVars, ghostVars>>

\* the first keep-alive: 3 -> 4 when it opens under the committed keys
ClientRecvKeepAlive == \E p \in SKeepAlive :
    /\ p.to = HonestAddr
    /\ S2C!Deliver(p)
    /\ IF cstate = 3 /\ p.tok = HonestToken /\ p.nonce = cnonce THEN cstate' = 4 ELSE UNCHANGED cstate
    /\ UNCHANGED <<ccookie, cnonce, cseq, cdue, cfirst, serverVars, attackVars, clock, c2sVars, ghostVars>>

\* a server packet addressed elsewhere leaves the link (the attacker's
\* address receives it; it was already seen when sent)
OtherRecv == \E p \in ServerPackets :
    /\ p.to # HonestAddr
    /\ S2C!Deliver(p)
    /\ UNCHANGED <<clientVars, serverVars, attackVars, clock, c2sVars, ghostVars>>

(***************************************************************************)
(* The server: the connection request ladder,                              *)
(* rocketnet_server_protocol_process_connection_request and                *)
(* rocketnet_server_process_opened_request, in the code's order.           *)
(***************************************************************************)

\* the cookie gate: zero field, then the MAC (current key, then the
\* previous when held), then expiry
LiveGens == {gen} \cup (IF AcceptPrevious /\ hasPrev THEN {IF gen = 1 THEN 3 ELSE gen - 1} ELSE {})

CookieOk(c, from) ==
    /\ c \in CookieVals
    /\ c.addr = from
    /\ c.gen \in LiveGens
    /\ c.exp > clock

\* rocketnet_token_table_select_slot for the history: free, then expired, then the oldest
HistoryRecord(t, from) ==
    IF HistLive(t) THEN history
    ELSE IF HistCount < HistoryCap \/ history[t] # None
      THEN [history EXCEPT ![t] = [addr |-> from, exp |-> ExpTime(t), order |-> insertOrder]]
      ELSE LET expired == {u \in Tokens : history[u] # None /\ history[u].exp <= clock}
               victim  == IF expired # {} THEN CHOOSE u \in expired : TRUE
                          ELSE CHOOSE u \in {u \in Tokens : history[u] # None} :
                                 \A v \in Tokens : history[v] # None => history[u].order <= history[v].order
           IN [history EXCEPT ![victim] = None, ![t] = [addr |-> from, exp |-> ExpTime(t), order |-> insertOrder]]

NewMapping(t, n) ==
    [est |-> FALSE, tok |-> t, nonce |-> n, deadline |-> clock + Deadline, lastRecv |-> clock,
     hi |-> 0, win |-> ZeroWin, opened |-> {}]

ServerRecvRequest == \E p \in Request :
    /\ C2S!Deliver(p)
    /\ LET from == p.from  t == p.tok IN
       \* one lookup: a connected address is silent, a half-open one gets its stored challenge
       IF Est(from) THEN
            /\ ServerSendsNothing /\ UNCHANGED <<serverVars, ghostVars>>
       ELSE IF Half(from) THEN
            /\ ServerSendPkt([type |-> "challenge", to |-> from, tok |-> map[from].tok, nonce |-> map[from].nonce])
            /\ UNCHANGED <<map, slots, others, history, admitted, insertOrder, nonceCtr, gen, hasPrev, sdue, ghostVars>>
       \* fixed-offset checks: version, protocol id, expire, timeout, flags, address, server key
       ELSE IF TokExpired(t) \/ t \notin ForThisServer THEN
            /\ ServerSendsNothing /\ UNCHANGED <<serverVars, ghostVars>>
       \* the cookie gate: answered with a fresh cookie under the current key
       ELSE IF ~CookieOk(p.cookie, from) THEN
            /\ ServerSendPkt([type |-> "cookie", to |-> from, exp |-> clock + CookieLife, gen |-> gen])
            /\ UNCHANGED <<map, slots, others, history, admitted, insertOrder, nonceCtr, gen, hasPrev, sdue, ghostVars>>
       \* the used-token history: bound to another address while unexpired
       ELSE IF HistLive(t) /\ history[t].addr # from THEN
            /\ ServerSendsNothing /\ UNCHANGED serverVars
            /\ cookieProven' = cookieProven \cup {from}
            /\ UNCHANGED <<admits, lateAdmit, replayAccepted, deniedPulse>>
       \* the signature, then the seal and its consistency with the signed fields
       ELSE IF t \notin Signed THEN
            /\ ServerSendsNothing /\ UNCHANGED serverVars
            /\ cookieProven' = cookieProven \cup {from}
            /\ UNCHANGED <<admits, lateAdmit, replayAccepted, deniedPulse>>
       \* client id connected, or the token already admitted its connection
       ELSE IF CidConnected(Cid[t]) \/ AdmLive(t) THEN
            /\ ServerSendsNothing /\ UNCHANGED serverVars
            /\ cookieProven' = cookieProven \cup {from}
            /\ UNCHANGED <<admits, lateAdmit, replayAccepted, deniedPulse>>
       \* past those: record, draw the nonce and the ephemeral, then slots and the mapping table
       ELSE
            /\ history' = HistoryRecord(t, from)
            /\ insertOrder' = IF HistLive(t) THEN insertOrder ELSE insertOrder + 1
            /\ cookieProven' = cookieProven \cup {from}
            /\ UNCHANGED <<admits, lateAdmit, replayAccepted>>
            /\ IF nonceCtr >= MaxNonces THEN
                    \* the model's nonce pool is spent: the request is dropped (a bound, not the code)
                    /\ ServerSendsNothing /\ UNCHANGED deniedPulse
                    /\ UNCHANGED <<map, slots, others, admitted, nonceCtr, gen, hasPrev, sdue>>
               ELSE LET n == nonceCtr + 1 IN
                    /\ nonceCtr' = n
                    /\ IF NumFree <= 0 THEN
                            \* no slots: denied under the handshake keys just derived, no mapping
                            /\ ServerSendPkt([type |-> "denied", to |-> from, tok |-> t, nonce |-> n])
                            /\ deniedPulse' = ~deniedPulse
                            /\ UNCHANGED <<map, slots, others, admitted, gen, hasPrev, sdue>>
                       ELSE IF NumMappings >= MapCap THEN
                            /\ ServerSendsNothing /\ UNCHANGED deniedPulse
                            /\ UNCHANGED <<map, slots, others, admitted, gen, hasPrev, sdue>>
                       ELSE
                            /\ map' = [map EXCEPT ![from] = NewMapping(t, n)]
                            /\ ServerSendPkt([type |-> "challenge", to |-> from, tok |-> t, nonce |-> n])
                            /\ UNCHANGED <<slots, others, admitted, gen, hasPrev, sdue, deniedPulse>>
    /\ UNCHANGED <<clientVars, clock>>

\* the prefilter and the size and type checks: junk is dropped before any
\* state is touched, whatever its filter value says
ServerRecvJunk == \E p \in Junk :
    /\ C2S!Deliver(p)
    /\ UNCHANGED <<clientVars, serverVars, attackVars, clock, s2cVars, ghostVars>>

(***************************************************************************)
(* The sealed ladder for a response: mapping, open under the handshake     *)
(* key (the nonce), then rocketnet_server_process_connection_response.      *)
(***************************************************************************)

\* rocketnet_admitted_token_set_add: present means success (F9); else a
\* free or expired slot; else refuse, never evicting an unexpired entry
AdmittedAddOk(t) ==
    \/ AdmLive(t)
    \/ AdmCount < AdmittedCap
    \/ admitted[t] # 0
    \/ \E u \in Tokens : admitted[u] # 0 /\ admitted[u] <= clock

AdmittedAfterAdd(t) ==
    IF AdmLive(t) THEN admitted
    ELSE IF AdmCount < AdmittedCap \/ admitted[t] # 0 THEN [admitted EXCEPT ![t] = ExpTime(t)]
    ELSE LET victim == CHOOSE u \in Tokens : admitted[u] # 0 /\ admitted[u] <= clock
         IN [admitted EXCEPT ![victim] = 0, ![t] = ExpTime(t)]

ServerRecvResponse == \E p \in Response :
    /\ C2S!Deliver(p)
    /\ LET from == p.from IN
       IF ~Live(from) \/ map[from].nonce # p.nonce \/ map[from].tok # p.tok THEN
            \* no mapping, or did not open (an integrity failure)
            /\ ServerSendsNothing /\ UNCHANGED <<serverVars, ghostVars>>
       ELSE IF map[from].est THEN
            \* opened: the slot's last-received time is refreshed (F7), then ignored as connected
            /\ map' = [map EXCEPT ![from].lastRecv = clock]
            /\ ServerSendsNothing
            /\ UNCHANGED <<slots, others, history, admitted, insertOrder, nonceCtr, gen, hasPrev, sdue, sfirst, ghostVars>>
       ELSE LET m == map[from]  t == m.tok IN
            IF CidConnected(Cid[t]) THEN
                /\ map' = [map EXCEPT ![from].lastRecv = clock]
                /\ ServerSendsNothing
                /\ UNCHANGED <<slots, others, history, admitted, insertOrder, nonceCtr, gen, hasPrev, sdue, ghostVars>>
            ELSE IF NumFree <= 0 \/ ~AdmittedAddOk(t) THEN
                \* denied, mapping kept; the token is not consumed
                /\ map' = [map EXCEPT ![from].lastRecv = clock]
                /\ ServerSendPkt([type |-> "denied", to |-> from, tok |-> t, nonce |-> m.nonce])
                /\ deniedPulse' = ~deniedPulse
                /\ UNCHANGED <<slots, others, history, admitted, insertOrder, nonceCtr, gen, hasPrev, sdue, admits, lateAdmit, cookieProven, replayAccepted>>
            ELSE
                \* the token admits its connection: the set takes the entry, the slot is assigned, the first keep-alive goes
                /\ admitted' = AdmittedAfterAdd(t)
                /\ slots' = slots \cup {[cid |-> Cid[t], tok |-> t, mapAddr |-> from, confirmed |-> FALSE]}
                /\ map' = [map EXCEPT ![from].est = TRUE, ![from].lastRecv = clock]
                /\ ServerSendPkt([type |-> "keepalive", to |-> from, tok |-> t, nonce |-> m.nonce])
                /\ admits' = [admits EXCEPT ![t] = @ + 1]
                /\ lateAdmit' = (lateAdmit \/ TokExpired(t))
                /\ UNCHANGED <<others, history, insertOrder, nonceCtr, gen, hasPrev, sdue, cookieProven, replayAccepted, deniedPulse>>
    /\ UNCHANGED <<clientVars, clock>>

(***************************************************************************)
(* The sealed ladder for a session packet (a keep-alive): mapping,         *)
(* sequence, replay check, open under the session key, replay advance,     *)
(* then not-connected or accepted (last-received refreshed, slot           *)
(* confirmed).                                                             *)
(***************************************************************************)
ServerRecvKeepAlive == \E p \in CKeepAlive :
    /\ C2S!Deliver(p)
    /\ ServerSendsNothing
    /\ LET from == p.from IN
       IF ~Live(from) THEN
            UNCHANGED <<serverVars, ghostVars>>
       ELSE LET m == map[from] IN
            IF AlreadyReceived(m.hi, m.win, p.seq) THEN
                \* refused before the open, whatever the keys
                UNCHANGED <<serverVars, ghostVars>>
            ELSE IF m.nonce # p.nonce \/ m.tok # p.tok THEN
                \* did not open (an integrity failure)
                UNCHANGED <<serverVars, ghostVars>>
            ELSE
                /\ replayAccepted' = (replayAccepted \/ p.seq \in m.opened)
                /\ LET hi2 == IF p.seq > m.hi THEN p.seq ELSE m.hi
                       win2 == Advanced(m.hi, m.win, p.seq)
                   IN IF ~m.est THEN
                        /\ map' = [map EXCEPT ![from].hi = hi2, ![from].win = win2, ![from].opened = @ \cup {p.seq}]
                        /\ UNCHANGED slots
                      ELSE
                        /\ map' = [map EXCEPT ![from].hi = hi2, ![from].win = win2, ![from].opened = @ \cup {p.seq}, ![from].lastRecv = clock]
                        /\ slots' = {IF s.mapAddr = from THEN [s EXCEPT !.confirmed = TRUE] ELSE s : s \in slots}
                /\ UNCHANGED <<others, history, admitted, insertOrder, nonceCtr, gen, hasPrev, sdue, sfirst>>
                /\ UNCHANGED <<admits, lateAdmit, cookieProven, deniedPulse>>
    /\ UNCHANGED <<clientVars, clock>>

\* rocketnet_server_protocol_write_keep_alive_if_due
\* (rocketnet_server_protocol.cpp:1239-1316).  The code never stops sending
\* keep-alives: until the client has acknowledged one (keep_alive_acked,
\* latched at rocketnet_ack.cpp:385) it sends on the keep-alive's own clock,
\* which other traffic does not suppress; after that, whenever nothing else
\* went out in the interval (last_send_time).  The code rejects the confirmed
\* flag as its test (:1284-1294).  This model has no ack blocks and no other
\* server traffic, so it stands the slot's confirmation in for the ack: here
\* only a client keep-alive confirms, and the honest client sends one only
\* in 4, after a keep-alive reached it, which is when the code's latch can
\* first be set.  It then stops, because past that point nothing in the
\* model waits on a keep-alive.  So the model sends no more keep-alives than
\* the code, and possibly fewer.
ServerKeepAlive == \E a \in Addrs :
    /\ sdue /\ RepeatKeepAlive
    /\ Est(a)
    /\ \E s \in slots : s.mapAddr = a /\ ~s.confirmed
    /\ sdue' = FALSE
    /\ ServerSendPkt([type |-> "keepalive", to |-> a, tok |-> map[a].tok, nonce |-> map[a].nonce])
    /\ UNCHANGED <<map, slots, others, history, admitted, insertOrder, nonceCtr, gen, hasPrev>>
    /\ UNCHANGED <<clientVars, clock, c2sVars, ghostVars>>

(***************************************************************************)
(* Other clients, outside the model, taking and releasing slots.           *)
(***************************************************************************)
OtherJoins ==
    /\ OthersMayJoin /\ NumFree > 0
    /\ others' = others + 1
    /\ UNCHANGED <<clientVars, map, slots, history, admitted, insertOrder, nonceCtr, gen, hasPrev, sdue, sfirst, attackVars, clock, c2sVars, s2cVars, ghostVars>>

OtherLeaves ==
    /\ OthersMayLeave /\ others > 0
    /\ others' = others - 1
    /\ UNCHANGED <<clientVars, map, slots, history, admitted, insertOrder, nonceCtr, gen, hasPrev, sdue, sfirst, attackVars, clock, c2sVars, s2cVars, ghostVars>>

(***************************************************************************)
(* The attacker.  On the path: sees everything, signs and seals nothing.    *)
(***************************************************************************)
SeenCookies == {seenCookies[a] : a \in {b \in Addrs : seenCookies[b] # None}}

\* present any token it holds with any cookie it has seen, none, or a forgery, from any source
InjectRequest == \E from \in Addrs, t \in Tokens, c \in {Zero, Bad} \cup SeenCookies :
    /\ Attacker
    /\ C2S!Send([type |-> "request", from |-> from, tok |-> t, cookie |-> c])
    /\ UNCHANGED <<clientVars, serverVars, attackVars, clock, s2cVars, ghostVars>>

\* an unkeyed header, with the right filter constant or a wrong one
InjectJunk == \E from \in Addrs, f \in BOOLEAN :
    /\ Attacker
    /\ C2S!Send([type |-> "junk", from |-> from, filt |-> f])
    /\ UNCHANGED <<clientVars, serverVars, attackVars, clock, s2cVars, ghostVars>>

\* re-send a sealed packet it has seen, from any source address
ReplaySealed == \E q \in seenSealed, from \in Addrs :
    /\ Attacker
    /\ C2S!Send(IF q.type = "response"
                  THEN [type |-> "response", from |-> from, tok |-> q.tok, nonce |-> q.nonce]
                  ELSE [type |-> "keepalive", from |-> from, tok |-> q.tok, nonce |-> q.nonce, seq |-> q.seq])
    /\ UNCHANGED <<clientVars, serverVars, attackVars, clock, s2cVars, ghostVars>>

(***************************************************************************)
(* Time.  One tick: the clock, both links age, the server sweeps its       *)
(* mappings (handshake deadline for a half-open one, idle timeout for an   *)
(* established one), the cookie key rotates on its period, and both        *)
(* senders' timers fire.                                                   *)
(***************************************************************************)
Swept(a, c) ==
    IF ~Live(a) THEN FALSE
    ELSE IF ~map[a].est THEN Deadline > 0 /\ c >= map[a].deadline
    ELSE Timeout > 0 /\ c >= map[a].lastRecv + Timeout

\* a copy that is due is delivered before time moves on: latency is bounded
\* by MaxDelay ticks, and a tick is one send interval
NothingDue == /\ IF c2sWire = <<>> THEN TRUE ELSE Head(c2sWire).due > 0
              /\ IF s2cWire = <<>> THEN TRUE ELSE Head(s2cWire).due > 0

Tick ==
    /\ NothingDue
    /\ clock' = SatClock(clock)
    /\ C2S!Age /\ S2C!Age
    /\ map' = [a \in Addrs |-> IF Swept(a, clock') THEN None ELSE map[a]]
    /\ slots' = {s \in slots : ~Swept(s.mapAddr, clock')}
    /\ IF Rotation /\ clock' % RotatePeriod = 0 /\ clock' # clock
         THEN gen' = (gen % 3) + 1 /\ hasPrev' = TRUE
         ELSE UNCHANGED <<gen, hasPrev>>
    /\ cdue' = TRUE /\ sdue' = TRUE
    /\ UNCHANGED <<cstate, ccookie, cnonce, cseq, cfirst, others, history, admitted, insertOrder, nonceCtr, sfirst, attackVars, ghostVars>>

C2SEnvironment == C2S!Environment /\ UNCHANGED <<clientVars, serverVars, attackVars, clock, s2cVars, ghostVars>>
S2CEnvironment == S2C!Environment /\ UNCHANGED <<clientVars, serverVars, attackVars, clock, c2sVars, ghostVars>>

Next ==
    \/ Connect \/ ClientSend
    \/ ClientRecvCookie \/ ClientRecvChallenge \/ ClientRecvDenied \/ ClientRecvKeepAlive \/ OtherRecv
    \/ ServerRecvRequest \/ ServerRecvJunk \/ ServerRecvResponse \/ ServerRecvKeepAlive \/ ServerKeepAlive
    \/ OtherJoins \/ OtherLeaves
    \/ InjectRequest \/ InjectJunk \/ ReplaySealed
    \/ Tick \/ C2SEnvironment \/ S2CEnvironment

\* Fairness: time passes, and the peers send and receive what is due.
\* Nothing is assumed of the attacker, of other clients, or of the path's
\* own actions.  Link!Fairness is not conjoined as written: its SF ranges
\* over every packet value, and ClientPackets is large enough that TLC
\* refuses the disjunctive normal form.  Its two halves are restated here
\* at the grain this model needs.  WireFair: every receive action is weakly
\* fair, and Tick (which conjoins Age) is weakly fair; with Tick disabled
\* while a copy is due (NothingDue), a due head is delivered before time
\* moves on, which is the stronger form of Link's SF on Deliver.
\* KindDelivery is Link's EventualDelivery per packet kind rather than per
\* packet value: a kind sent infinitely often is delivered infinitely
\* often.  It is what rules out the path dropping every copy under
\* "arbitrary"; under the deterministic patterns WireFair alone suffices.
PeerFairness ==
    /\ WF_vars(Tick) /\ WF_vars(Connect) /\ WF_vars(ClientSend) /\ WF_vars(ServerKeepAlive)
    /\ WF_vars(ClientRecvCookie) /\ WF_vars(ClientRecvChallenge) /\ WF_vars(ClientRecvDenied)
    /\ WF_vars(ClientRecvKeepAlive) /\ WF_vars(OtherRecv)
    /\ WF_vars(ServerRecvRequest) /\ WF_vars(ServerRecvJunk) /\ WF_vars(ServerRecvResponse)
    /\ WF_vars(ServerRecvKeepAlive)

C2SSendKind(k)    == \E p \in ClientPackets : p.type = k /\ C2S!Send(p)
C2SDeliverKind(k) == \E p \in ClientPackets : p.type = k /\ C2S!Deliver(p)
S2CSendKind(k)    == \E p \in ServerPackets : p.type = k /\ S2C!Send(p)
S2CDeliverKind(k) == \E p \in ServerPackets : p.type = k /\ S2C!Deliver(p)

KindDelivery ==
    /\ \A k \in ClientKinds : ([]<> <<C2SSendKind(k)>>_c2sVars) => ([]<> <<C2SDeliverKind(k)>>_c2sVars)
    /\ \A k \in ServerKinds : ([]<> <<S2CSendKind(k)>>_s2cVars) => ([]<> <<S2CDeliverKind(k)>>_s2cVars)

\* The path duplicates, reorders and delays any given traffic finitely
\* often: eventually no step is the path's own.  Without it TLC finds the
\* path duplicating one challenge forever, delivering a copy and making
\* another, so the wire never empties and time never passes (the lasso in
\* README-Security.md).  The loss patterns are decided at Send and are not
\* affected; Drop under "arbitrary" is, which is why liveness under
\* "arbitrary" is checked with SpecArbitrary and no path actions enabled.
PathSettles == <>[][~(C2SEnvironment \/ S2CEnvironment)]_vars

\* The attacker sends finitely often: eventually no step is its own.  Without
\* it TLC's first counterexample to HonestConnects with the attacker on is an
\* attacker that injects a request every time the wire empties, so Tick
\* never fires and the honest client never sends: an attacker of unbounded
\* rate, which is the flood no protocol survives and not what the design
\* claims to resist.  Safety never relies on this; liveness with the
\* attacker on is about what a bounded attacker can leave behind.
AttackerSettles == <>[][~(InjectRequest \/ InjectJunk \/ ReplaySealed)]_vars

Fairness == PeerFairness /\ PathSettles /\ AttackerSettles

Spec == Init /\ [][Next]_vars /\ Fairness

\* for liveness under Pattern "arbitrary": Drop may fire forever, so
\* PathSettles is not assumed and KindDelivery takes its place; run it with
\* Duplication = FALSE and Reordering = FALSE
SpecArbitrary == Init /\ [][Next]_vars /\ PeerFairness /\ KindDelivery

(***************************************************************************)
(* Invariants.                                                             *)
(***************************************************************************)
TypeOK ==
    /\ cstate \in {-7, -1, 0, 1, 2, 3, 4}
    /\ ccookie \in {Zero} \cup CookieVals
    /\ cnonce \in {0} \cup Nonces
    /\ cseq \in 1..MaxSeq+1
    /\ \A a \in Addrs : map[a] = None \/ map[a] \in Mapping
    /\ \A t \in Tokens : history[t] = None \/ history[t] \in HistEntry
    /\ \A t \in Tokens : admitted[t] \in Nat
    /\ nonceCtr \in 0..MaxNonces
    /\ gen \in Gens
    /\ clock \in 0..MaxClock
    /\ C2S!TypeOK /\ S2C!TypeOK

\* a token admits at most one connection (STANDARD.md, "a token admits one connection")
TokenAdmitsAtMostOnce == \A t \in Tokens : admits[t] <= 1
OneConnectionPerToken == \A s1, s2 \in slots : s1.tok = s2.tok => s1 = s2

\* an expired token never admits.  The request path checks expiry at its
\* top; the response path does not, so a token that expires between the
\* challenge and the response still admits (bounded by the handshake
\* deadline).  Checked in its own run; see README-Security.md.
ExpiredNeverAdmits == ~lateAdmit

\* a replayed session packet is refused, inside the window and beyond it
NeverAcceptsReplay == ~replayAccepted

\* the window agrees with what was opened, inside the window (F1 breaks this)
InWindowExact == \A a \in Addrs : Live(a) =>
    LET m == map[a] IN \A s \in Seqs : (s <= m.hi /\ s > m.hi - W) => (AlreadyReceived(m.hi, m.win, s) <=> s \in m.opened)

\* beyond the window everything is refused
BeyondWindowRefused == \A a \in Addrs : Live(a) =>
    LET m == map[a] IN \A s \in Seqs : (m.hi >= W /\ s <= m.hi - W) => AlreadyReceived(m.hi, m.win, s)

\* nothing authenticated is derived from an unkeyed value: every mapping and
\* slot rests on a signed token sealed to this server, presented from an
\* address whose cookie verified; the filter and the junk it admits touch nothing
NothingFromUnkeyed ==
    /\ \A a \in Addrs : Live(a) => map[a].tok \in Signed /\ map[a].tok \in ForThisServer /\ a \in cookieProven
    /\ \A s \in slots : s.tok \in Signed /\ s.tok \in ForThisServer

\* the state kept for unauthenticated peers is bounded: the cookie gate
\* stores nothing (there is no server variable for it), the mapping table is
\* bounded, and the tables never exceed their capacity
UnauthStateBounded ==
    /\ NumMappings <= MapCap
    /\ HistCount <= HistoryCap
    /\ AdmCount <= AdmittedCap
    /\ Cardinality(slots) + others <= MaxClients

\* key exchange: a nonce is drawn once; no two mappings share one; the
\* client's committed nonce was drawn for its own mapping
NonceFresh ==
    /\ \A a, b \in Addrs : (Live(a) /\ Live(b) /\ a # b) => map[a].nonce # map[b].nonce
    /\ \A a \in Addrs : Live(a) => map[a].nonce <= nonceCtr
    /\ cnonce # 0 => cnonce <= nonceCtr

\* a slot is held only by an established mapping, one slot per mapping, one per client id
SlotMappingBijection ==
    /\ \A s \in slots : Est(s.mapAddr) /\ map[s.mapAddr].tok = s.tok
    /\ \A s1, s2 \in slots : s1.mapAddr = s2.mapAddr => s1 = s2
    /\ \A s1, s2 \in slots : s1.cid = s2.cid => s1 = s2

\* an admitted entry leaves the set only by expiry
AdmittedNeverEvictedUnexpired == [][\A t \in Tokens : AdmLive(t) => admitted'[t] = admitted[t]]_vars

\* a denied does not consume the token: the set is unchanged by any step that sends one
DeniedTokenStaysUsable == [][deniedPulse' # deniedPulse => admitted' = admitted]_vars

\* junk never changes server state (the prefilter and the size checks)
JunkTouchesNothing == [][(\E p \in Junk : C2S!Deliver(p)) => UNCHANGED serverVars]_vars

\* while a history entry lives, only its first address passes (expected to
\* fail by flush-then-replay once the history evicts at capacity, S:2566-2570)
BindingFirstAddress == \A a \in Addrs : Live(a) => (history[map[a].tok] # None => history[map[a].tok].addr = a)

\* a client at -1 has no slot held for it (expected to fail: F5, a denied
\* response overtaken by the retransmission that a freed slot then admits)
DeniedMeansNoSlot == cstate = -1 => ~\E s \in slots : s.mapAddr = HonestAddr

\* The clock saturates at MaxClock, and once it has, nothing expires any
\* more; a liveness property about the cookie gate therefore passes
\* vacuously after saturation.  The gate is checked as an invariant instead:
\* this holds exactly when the honest client never gets past the gate while
\* time is still moving, which is the cookie race the standard conditions
\* against (lifetime at least two send intervals) and the rotation window
\* (a cookie under the previous key verifies).  It is expected to hold for
\* the broken settings and to be violated for the working ones.
GatePassedOnlyAtSaturation == cstate \in {3, 4} => clock = MaxClock

\* a mapping is created once and never replaced while it lives
MappingCreatedOnce == [][\A a \in Addrs : (Live(a) /\ Live(a)') => map'[a].nonce = map[a].nonce]_vars

SafetyInvariants ==
    /\ TypeOK
    /\ TokenAdmitsAtMostOnce /\ OneConnectionPerToken
    /\ NeverAcceptsReplay /\ BeyondWindowRefused
    /\ NothingFromUnkeyed
    /\ UnauthStateBounded
    /\ NonceFresh
    /\ SlotMappingBijection

(***************************************************************************)
(* Liveness (needs Fairness; checked with Attacker = FALSE).                *)
(* HonestConnects also fails, as expected, with the attacker on: the       *)
(* attacker who saw the honest request can bind the token to its own       *)
(* address first (README-Security.md).                                     *)
(***************************************************************************)

\* the honest client, once it asks, connects or is denied
HonestResolves == (cstate = 1) ~> (cstate \in {4, -1})

\* with a slot free for it, it connects
HonestConnects == (cstate = 1) ~> (cstate = 4)

\* the server is never left waiting for a packet nobody will send again: an
\* established slot is confirmed, or goes away
ServerNotStranded == \A a \in Addrs :
    (Est(a) /\ \E s \in slots : s.mapAddr = a /\ ~s.confirmed) ~> (~Est(a) \/ \A s \in slots : s.mapAddr = a => s.confirmed)

\* a half-open mapping leaves by its deadline, whatever the peer sends
HalfBounded == \A a \in Addrs : Half(a) ~> ~Half(a)

=============================================================================
