----------------------------- MODULE RNPathProbe -----------------------------
(***************************************************************************)
(* The path packet size probe of the handshake: the client's walk down its *)
(* candidate packet sizes after the challenge, the server's answer to each *)
(* probe, the size the connection response carries and the slot the server *)
(* assigns with it (the MTU fallback design: link_mtus in the client and   *)
(* server configs, packet type 9 connection probe, the response's u16      *)
(* packet size, the client state -14 mtu probe failed).                    *)
(*                                                                         *)
(* The model starts after the challenge: the client holds the handshake   *)
(* keys and the server's mapping for the client's address is half-open    *)
(* (proven).  One client C and one unproven address X, which has no        *)
(* mapping and no keys: anything X sends fails to open.                    *)
(*                                                                         *)
(* Sizes are abstract wire sizes 1 .. Ceiling, Ceiling the packet ceiling  *)
(* (1452 bytes in the code); a control packet (response, keep-alive) is    *)
(* size 0 and fits any path.  ClientList is the client's candidate list   *)
(* for the committed family, strictly descending (P of each link_mtus      *)
(* entry, equal sizes collapsed); ServerList is the server's own candidate *)
(* set for that family, which may differ.  A one-entry ClientList does no  *)
(* probing: the response goes at once with that entry's size, as today.    *)
(*                                                                         *)
(* The path has a capacity in each direction, chosen at Init from CapsUp   *)
(* and CapsDown and fixed for the behaviour: a packet larger than its      *)
(* direction's capacity is dropped when sent, always; a packet that fits   *)
(* may be lost (LoseUp, LoseDown: at most MaxLoss in a behaviour, an      *)
(* honest probe the server's allowance withholds counted as one) and      *)
(* packets in flight are a set, delivered in any order.  An on-path       *)
(* attacker may replay a client probe in flight (a second copy, n = 1) or *)
(* send a probe of any size from X, at most AttackBudget times in all.     *)
(*                                                                         *)
(* Time.  Tick is one client send interval: the client may send one packet *)
(* (maySend), and the server runs at least one update, which resets the   *)
(* mapping's probe reply allowance (answered); ServerUpdate is a further   *)
(* server update inside the interval.  The probe window of candidate idx   *)
(* is an abstract timer: W sends of the candidate's probe, the client      *)
(* sending again only when nothing of the last exchange is in flight (the  *)
(* window, max(4 send intervals, 3 x the challenge RTT), covers a round    *)
(* trip), and Expire when all W are spent and nothing is in flight.  With  *)
(* LateExpire the window may expire early, after any send and with packets *)
(* still in flight, so a reply may arrive in a later window: that is the  *)
(* safety case of a slow round trip and has no liveness claim.  The        *)
(* response phase has the same shape (W responses, then the response      *)
(* timeout, the client state -4).                                          *)
(*                                                                         *)
(* After connected, both ends send payload at the connection's packet     *)
(* size (the client's size, the server's slot size); upGot and downGot are *)
(* ghosts set when one arrives.  No connection timeout is modelled: in the *)
(* code small keep-alives keep a connection whose full-size packets are   *)
(* all dropped alive forever (reliable_message_timeout_ms defaults to 0),  *)
(* which is the hang NoHang names.                                         *)
(*                                                                         *)
(*   ClientSendProbe        a probe of the current candidate's size        *)
(*   ClientExpire           the probe window ends: next candidate, or -14  *)
(*   ClientRecvReply(r)     a matching reply (a size at an index <= idx)   *)
(*                          fixes the packet size and moves to sending the *)
(*                          response, with one early send                  *)
(*   ClientSendResponse     the response, carrying the packet size         *)
(*   ClientTimeout          the response phase ends: -4                    *)
(*   ClientRecvKeepAlive(r) connected                                      *)
(*   ServerRecvProbe(p)     answer (proven mapping, half-open, a size on   *)
(*                          the server's own list, the update's allowance  *)
(*                          unspent, a reply of the same size) or ignore   *)
(*   ServerRecvResponse(p)  half-open and the size on its list: the slot,  *)
(*                          with that size, and a keep-alive; connected:   *)
(*                          a keep-alive again                             *)
(*   ClientSendData, ServerSendData, ClientRecvData, ServerRecvData        *)
(*   Tick, ServerUpdate     the clocks                                     *)
(*   LoseUp, LoseDown, Replay, AttackProbe   the environment               *)
(*   Done                   the terminal stutter                           *)
(*                                                                         *)
(* Broken selects a reversed witness:                                      *)
(*   "today"       no probe: the client sends its response at once with   *)
(*                 the ceiling, the code before the design; on a path     *)
(*                 that cannot carry it the client connects on the small  *)
(*                 keep-alive and full-size payload never arrives          *)
(*                 (CarriedBothWays, and NoHang and FailsWhenNoneFits as   *)
(*                 temporal properties);                                   *)
(*   "unproven"    the server answers a probe from an address with no     *)
(*                 mapping whose probe did not open (ReplyOnlyProven);     *)
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
(*                 (ConnectsWhenFits).  "today" also connects a client    *)
(*                 whose path carries none of its candidates               *)
(*                 (FailsWhenNoneFits).                                    *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Ceiling,        \* the packet ceiling: sizes are 1 .. Ceiling
    ClientList,     \* the client's candidates, strictly descending
    ServerList,     \* the server's own candidate set
    CapsUp,         \* the path capacities client -> server to explore
    CapsDown,       \* the path capacities server -> client to explore
    W,              \* sends in one probe window, and in the response phase
    MaxLoss,        \* the most packets that fit and are lost, in a behaviour
    AttackBudget,   \* the most attacker sends (replays and probes from X)
    LateExpire,     \* TRUE: a window may expire with packets in flight
    Broken          \* "none", or the reversed witness

Sizes == 1..Ceiling
Range(f) == {f[i] : i \in DOMAIN f}

ASSUME /\ Ceiling \in Nat \ {0}
       /\ ClientList \in Seq(Sizes) /\ Len(ClientList) >= 1
       /\ \A i \in 1..Len(ClientList) - 1 : ClientList[i] > ClientList[i + 1]
       /\ ServerList \subseteq Sizes /\ ServerList # {}
       /\ CapsUp \subseteq 0..Ceiling /\ CapsUp # {}
       /\ CapsDown \subseteq 0..Ceiling /\ CapsDown # {}
       /\ W \in Nat \ {0} /\ MaxLoss \in Nat /\ AttackBudget \in Nat
       /\ LateExpire \in BOOLEAN
       /\ Broken \in {"none", "today", "unproven", "larger", "offlist",
                      "noallowance", "slotceiling", "noexpire", "skiplast"}

Probing == Len(ClientList) > 1
Cand(i) == ClientList[i]
Max(S) == CHOOSE m \in S : \A x \in S : x <= m

\* packets: one record shape for both directions
Pkt(k, src, s, p, n) == [k |-> k, src |-> src, s |-> s, p |-> p, n |-> n]
UpPackets ==
    {Pkt("probe", a, s, 0, n) : a \in {"C", "X"}, s \in Sizes, n \in {0, 1}}
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
    idx,            \* the candidate being probed
    size,           \* the client's packet size, 0 until chosen
    sends,          \* sends of this window (probe) or of the response phase
    maySend,        \* the client may send this interval
    mapping,        \* the server's mapping of C: "halfopen" or "connected"
    slot,           \* the slot's packet size, 0 until assigned
    answered,       \* the mapping's probe reply allowance is spent this update
    up,             \* packets in flight client -> server
    down,           \* packets in flight server -> client
    losses,         \* packets lost so far (environment)
    attacks,        \* attacker sends so far (environment)
    proof,          \* ghost: the reply the client took: [up, down] sizes that crossed
    upGot,          \* ghost: a payload reached the server
    downGot,        \* ghost: a payload reached the client
    badUnproven,    \* ghost: a reply went to an address that never opened
    badLarger,      \* ghost: a reply larger than the probe it answers
    badOffList,     \* ghost: a reply of a size not on the server's list
    badTwice        \* ghost: a second reply to one mapping in one update

vars == << capUp, capDown, phase, idx, size, sends, maySend, mapping, slot,
           answered, up, down, losses, attacks, proof, upGot, downGot,
           badUnproven, badLarger, badOffList, badTwice >>

ghosts == << proof, upGot, downGot, badUnproven, badLarger, badOffList, badTwice >>

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
    /\ maySend = TRUE                \* the challenge's early send
    /\ mapping = "halfopen"
    /\ slot = 0
    /\ answered = FALSE
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

ClientSendProbe ==
    /\ phase = "probe"
    /\ maySend
    /\ sends < W \/ Broken = "noexpire"
    /\ ~ProbeInFlight /\ ~ReplyInFlight
    /\ up' = SendUp(Pkt("probe", "C", Cand(idx), 0, 0))
    /\ sends' = IF sends < W THEN sends + 1 ELSE sends
    /\ maySend' = FALSE
    /\ UNCHANGED << capUp, capDown, phase, idx, size, mapping, slot, answered,
                    down, losses, attacks >>
    /\ UNCHANGED ghosts

ClientExpire ==
    /\ phase = "probe"
    /\ Broken # "noexpire"
    /\ IF LateExpire
       THEN sends >= 1
       ELSE sends = W /\ ~ProbeInFlight /\ ~ReplyInFlight
    /\ IF idx < Len(ClientList) - (IF Broken = "skiplast" THEN 1 ELSE 0)
       THEN /\ idx' = idx + 1
            /\ sends' = 0
            /\ UNCHANGED phase
       ELSE /\ phase' = "failed"
            /\ UNCHANGED << idx, sends >>
    /\ UNCHANGED << capUp, capDown, size, maySend, mapping, slot, answered,
                    up, down, losses, attacks >>
    /\ UNCHANGED ghosts

\* A reply opens under the handshake server-to-client key (every reply here
\* is the server's to C); it matches when its size is a candidate at an index
\* at or below the one being probed. Anything else is ignored.
ClientRecvReply(r) ==
    /\ r \in down
    /\ r.k = "reply"
    /\ down' = down \ {r}
    /\ IF phase = "probe" /\ r.s \in {Cand(j) : j \in 1..idx}
       THEN /\ phase' = "response"
            /\ size' = r.s
            /\ sends' = 0
            /\ maySend' = TRUE        \* one early send for this advance
            /\ proof' = [up |-> r.p, down |-> r.s]
       ELSE /\ UNCHANGED << phase, size, sends, maySend >>
            /\ UNCHANGED proof
    /\ UNCHANGED << capUp, capDown, idx, mapping, slot, answered, up, losses,
                    attacks, upGot, downGot, badUnproven, badLarger,
                    badOffList, badTwice >>

ClientSendResponse ==
    /\ phase = "response"
    /\ maySend
    /\ sends < W
    /\ ~ResponseInFlight /\ ~KeepAliveInFlight
    /\ up' = SendUp(Pkt("resp", "C", size, 0, 0))
    /\ sends' = sends + 1
    /\ maySend' = FALSE
    /\ UNCHANGED << capUp, capDown, phase, idx, size, mapping, slot, answered,
                    down, losses, attacks >>
    /\ UNCHANGED ghosts

ClientTimeout ==
    /\ phase = "response"
    /\ sends = W
    /\ ~ResponseInFlight /\ ~KeepAliveInFlight
    /\ phase' = "timedout"
    /\ UNCHANGED << capUp, capDown, idx, size, sends, maySend, mapping, slot,
                    answered, up, down, losses, attacks >>
    /\ UNCHANGED ghosts

ClientRecvKeepAlive(r) ==
    /\ r \in down
    /\ r.k = "ka"
    /\ down' = down \ {r}
    /\ phase' = IF phase = "response" THEN "connected" ELSE phase
    /\ UNCHANGED << capUp, capDown, idx, size, sends, maySend, mapping, slot,
                    answered, up, losses, attacks >>
    /\ UNCHANGED ghosts

ClientSendData ==
    /\ phase = "connected"
    /\ ~\E p \in up : p.k = "data"
    /\ up' = SendUp(Pkt("data", "C", size, 0, 0))
    /\ up' # up
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, answered, down, losses, attacks >>
    /\ UNCHANGED ghosts

ClientRecvData(r) ==
    /\ r \in down
    /\ r.k = "data"
    /\ down' = down \ {r}
    /\ IF phase = "connected" THEN downGot' = TRUE ELSE UNCHANGED downGot
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, answered, up, losses, attacks >>
    /\ UNCHANGED << proof, upGot, badUnproven, badLarger, badOffList, badTwice >>

-----------------------------------------------------------------------------
(* The server *)

\* the probe opens under the handshake client-to-server key: only C's do
Opens(p) == p.src = "C"

\* the checks, each of which a witness drops
Proven(p) == Broken = "unproven" \/ (Opens(p) /\ mapping = "halfopen")
OnList(p) == Broken = "offlist" \/ p.s \in ServerList
Allowed == Broken = "noallowance" \/ ~answered
ReplySize(p) == IF Broken = "larger" THEN Max(ServerList) ELSE p.s

\* An honest probe the allowance withholds: the client's probe arrived in the
\* same server update as an earlier one (delay bunched them). To the client
\* that is a lost attempt, so it spends the loss budget.
Withheld(p) == Proven(p) /\ OnList(p) /\ ~Allowed /\ Opens(p) /\ p.n = 0

ServerRecvProbe(p) ==
    /\ p \in up
    /\ p.k = "probe"
    /\ up' = up \ {p}
    /\ Withheld(p) => losses < MaxLoss
    /\ losses' = IF Withheld(p) THEN losses + 1 ELSE losses
    /\ IF Proven(p) /\ OnList(p) /\ Allowed
       THEN /\ answered' = TRUE
            /\ down' = IF Opens(p)
                       THEN SendDown(Pkt("reply", "S", ReplySize(p), p.s, 0))
                       ELSE down     \* a reply to X: never reaches C
            /\ IF ~Opens(p) THEN badUnproven' = TRUE ELSE UNCHANGED badUnproven
            /\ IF ReplySize(p) > p.s THEN badLarger' = TRUE ELSE UNCHANGED badLarger
            /\ IF ReplySize(p) \notin ServerList THEN badOffList' = TRUE ELSE UNCHANGED badOffList
            /\ IF answered THEN badTwice' = TRUE ELSE UNCHANGED badTwice
       ELSE /\ UNCHANGED << answered, down >>
            /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice >>
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, attacks, proof, upGot, downGot >>

\* the response opens; the size it carries must be on the server's own list
ServerRecvResponse(p) ==
    /\ p \in up
    /\ p.k = "resp"
    /\ up' = up \ {p}
    /\ IF mapping = "halfopen" /\ p.s \in ServerList
       THEN /\ mapping' = "connected"
            /\ slot' = IF Broken = "slotceiling" THEN Ceiling ELSE p.s
            /\ down' = SendDown(Pkt("ka", "S", 0, 0, 0))
       ELSE IF mapping = "connected"
            THEN /\ down' = SendDown(Pkt("ka", "S", 0, 0, 0))
                 /\ UNCHANGED << mapping, slot >>
            ELSE UNCHANGED << mapping, slot, down >>
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, answered,
                    losses, attacks >>
    /\ UNCHANGED ghosts

ServerSendData ==
    /\ mapping = "connected"
    /\ ~\E r \in down : r.k = "data"
    /\ down' = SendDown(Pkt("data", "S", slot, 0, 0))
    /\ down' # down
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, answered, up, losses, attacks >>
    /\ UNCHANGED ghosts

ServerRecvData(p) ==
    /\ p \in up
    /\ p.k = "data"
    /\ up' = up \ {p}
    /\ IF mapping = "connected" THEN upGot' = TRUE ELSE UNCHANGED upGot
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, answered, down, losses, attacks >>
    /\ UNCHANGED << proof, downGot, badUnproven, badLarger, badOffList, badTwice >>

-----------------------------------------------------------------------------
(* Time and the environment *)

Tick ==
    /\ ~maySend \/ answered
    /\ maySend' = TRUE
    /\ answered' = FALSE
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, mapping, slot, up,
                    down, losses, attacks >>
    /\ UNCHANGED ghosts

ServerUpdate ==
    /\ answered
    /\ answered' = FALSE
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, up, down, losses, attacks >>
    /\ UNCHANGED ghosts

\* a packet that fits is lost on the way (an attacker's needs no loss)
LoseUp(p) ==
    /\ losses < MaxLoss
    /\ p \in up
    /\ p.src = "C" /\ p.n = 0
    /\ up' = up \ {p}
    /\ losses' = losses + 1
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, answered, down, attacks >>
    /\ UNCHANGED ghosts

LoseDown(r) ==
    /\ losses < MaxLoss
    /\ r \in down
    /\ down' = down \ {r}
    /\ losses' = losses + 1
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, answered, up, attacks >>
    /\ UNCHANGED ghosts

\* an on-path copy of a client probe in flight, the client's address its source
Replay(p) ==
    /\ attacks < AttackBudget
    /\ p \in up
    /\ p.k = "probe" /\ p.src = "C" /\ p.n = 0
    /\ up' = up \cup {[p EXCEPT !.n = 1]}
    /\ up' # up
    /\ attacks' = attacks + 1
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, answered, down, losses >>
    /\ UNCHANGED ghosts

\* a probe of any size from an address that never opened
AttackProbe(s) ==
    /\ attacks < AttackBudget
    /\ up' = up \cup {Pkt("probe", "X", s, 0, 0)}
    /\ up' # up
    /\ attacks' = attacks + 1
    /\ UNCHANGED << capUp, capDown, phase, idx, size, sends, maySend, mapping,
                    slot, answered, down, losses >>
    /\ UNCHANGED ghosts

\* the attempt is over: failed and timed out end it, and a connected client
\* whose packets are all delivered or dropped has nothing left to do
Done ==
    /\ phase \in {"connected", "failed", "timedout"}
    /\ UNCHANGED vars

Next ==
    \/ ClientSendProbe
    \/ ClientExpire
    \/ \E r \in down : ClientRecvReply(r)
    \/ ClientSendResponse
    \/ ClientTimeout
    \/ \E r \in down : ClientRecvKeepAlive(r)
    \/ ClientSendData
    \/ \E r \in down : ClientRecvData(r)
    \/ \E p \in up : ServerRecvProbe(p)
    \/ \E p \in up : ServerRecvResponse(p)
    \/ ServerSendData
    \/ \E p \in up : ServerRecvData(p)
    \/ Tick
    \/ ServerUpdate
    \/ \E p \in up : LoseUp(p)
    \/ \E r \in down : LoseDown(r)
    \/ \E p \in up : Replay(p)
    \/ \E s \in Sizes : AttackProbe(s)
    \/ Done

\* Every step of the two ends and every delivery is weakly fair; the clocks
\* too. Loss, the attacker and the extra server update are not.
Fairness ==
    /\ WF_vars(ClientSendProbe)
    /\ WF_vars(ClientExpire)
    /\ WF_vars(ClientSendResponse)
    /\ WF_vars(ClientTimeout)
    /\ WF_vars(ClientSendData)
    /\ WF_vars(ServerSendData)
    /\ WF_vars(Tick)
    /\ \A r \in DownPackets : WF_vars(ClientRecvReply(r))
    /\ \A r \in DownPackets : WF_vars(ClientRecvKeepAlive(r))
    /\ \A r \in DownPackets : WF_vars(ClientRecvData(r))
    /\ \A p \in UpPackets : WF_vars(ServerRecvProbe(p))
    /\ \A p \in UpPackets : WF_vars(ServerRecvResponse(p))
    /\ \A p \in UpPackets : WF_vars(ServerRecvData(p))

Spec == Init /\ [][Next]_vars
FairSpec == Spec /\ Fairness

-----------------------------------------------------------------------------
(* Safety *)

TypeOK ==
    /\ capUp \in 0..Ceiling /\ capDown \in 0..Ceiling
    /\ phase \in {"probe", "response", "connected", "failed", "timedout"}
    /\ idx \in 1..Len(ClientList)
    /\ size \in 0..Ceiling
    /\ sends \in 0..W
    /\ maySend \in BOOLEAN
    /\ mapping \in {"halfopen", "connected"}
    /\ slot \in 0..Ceiling
    /\ answered \in BOOLEAN
    /\ up \subseteq UpPackets
    /\ down \subseteq DownPackets
    /\ losses \in 0..MaxLoss
    /\ attacks \in 0..AttackBudget
    /\ proof \in [up : 0..Ceiling, down : 0..Ceiling]
    /\ {upGot, downGot, badUnproven, badLarger, badOffList, badTwice} \subseteq BOOLEAN

\* (1) both ends hold the same packet size: when the client is connected the
\* server's slot is assigned, with the client's size
SlotAgrees == phase = "connected" => mapping = "connected" /\ slot = size

\* the chosen size is one of the client's own candidates
ChosenIsCandidate == phase \in {"response", "connected"} => size \in Range(ClientList)

\* (2) a probing client is connected only at a size the path carries both
\* ways, and a packet of that size has crossed each way: the probe the reply
\* answers up, the reply down. A one-entry list does no probing.
CarriedBothWays ==
    (phase = "connected" /\ Probing) => size <= capUp /\ size <= capDown
CrossedBothWays ==
    (phase = "connected" /\ Probing) => proof.up >= size /\ proof.down >= size

\* (3) the server never answers an address whose probe did not open under the
\* handshake keys of a proven mapping, never with a packet larger than the one
\* it answers, and only with sizes on its own list
ReplyOnlyProven == ~badUnproven
ReplyNoLarger == ~badLarger
ReplyOwnList == ~badOffList

\* (4) at most one probe reply per mapping per update
OneReplyPerUpdate == ~badTwice

\* Reachability, not a rule: a probing client never takes a reply from an
\* earlier window than the one it is in. MCRNPathProbeReachLateReply.cfg
\* must violate it, so the late-reply path of LateExpire is explored.
NoLateReplyTaken ==
    (phase \in {"response", "connected"} /\ Probing) => size = Cand(idx)

-----------------------------------------------------------------------------
(* Liveness, under FairSpec *)

Fits(c) == c \in ServerList /\ c <= capUp /\ c <= capDown
SomeFits == IF Probing THEN \E i \in 1..Len(ClientList) : Fits(Cand(i))
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

=============================================================================
