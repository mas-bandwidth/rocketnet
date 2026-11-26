----------------------------- MODULE RNPathProbe -----------------------------
(***************************************************************************)
(* The path packet size probe, v2: connect small, probe upward.  Both ends *)
(* start the connection at P0, the smallest candidate the client's list   *)
(* gives the family (the connection response carries it; t12 rule 16).    *)
(* After CONNECTED, a client whose list has more than one candidate walks  *)
(* the candidates above P, largest first, with pairs of probes, raises P   *)
(* on an authenticated large reply and confirms the raise to the server;  *)
(* the server answers probes from a connected mapping and raises its P    *)
(* only on a confirm (sdk/rocketnet_client_protocol.cpp,                   *)
(* sdk/rocketnet_server_protocol.cpp; standard/TRANSPORT.md t11 and t12).  *)
(*                                                                         *)
(* The handshake is not modelled: it carries no probe.  The model starts  *)
(* CONNECTED at P0 on both ends.  Probe packets (type 9) are sealed under *)
(* the session keys on the session sequences.  Sources: "C", the client;  *)
(* "F", a forger at the client's address, whose packets find C's mapping  *)
(* and fail to open; "X", an address whose mapping is half-open, so its   *)
(* probes open under its own session keys but come from no connection.    *)
(*                                                                         *)
(* Sizes are wire sizes.  The small probe, the confirm and its echo are   *)
(* 34 bytes in the code, wire size 0 here, and fit every path.  ClientSet *)
(* is the client's candidate set for the family (P of each link_mtus      *)
(* entry, equal sizes collapsed); ServerList is the server's own, which   *)
(* may differ; P0 = the smallest of ClientSet, on both lists.  A large    *)
(* probe is at a candidate S above P (or at P0 itself, last).             *)
(*                                                                         *)
(* The path.  A capacity each way, chosen at Init from CapsUp and CapsDown *)
(* and fixed for the behaviour: a packet larger than its direction's      *)
(* capacity is dropped when sent, always.  Loss is bursts (LossBurst): a  *)
(* burst of kind "all" drops every packet, of kind "large" every packet   *)
(* above the control size (the large probe and its reply), in flight      *)
(* when it starts and sent while it lasts, both ways; at most MaxBursts   *)
(* bursts, and every burst ends.  The loss assumption: loss of large      *)
(* packets alone never covers K consecutive pairs whose small probes came *)
(* back.  burstStrikes counts the strikes since the candidate's strikes   *)
(* last went to zero taken on a pair a burst met (pairInBurst); a burst   *)
(* may meet a pair that can still strike only while burstStrikes < K - 1, *)
(* so loss alone never makes K strikes in a row.  A pair whose small      *)
(* probe is lost breaks the row: the code sets strikes to zero, and the   *)
(* row starts again.  Packets in flight are a set, delivered in any       *)
(* order.  The server's list is part of what the path carries:            *)
(* Carried(S) is S on ServerList and S within both capacities.            *)
(* An on-path attacker may replay a client packet in flight (a second     *)
(* copy, n = 1), send packets from X or F, or a forged large reply,       *)
(* at most AttackBudget times in all; with Forger, F sends without limit.  *)
(*                                                                         *)
(* Time.  Tick is one pair interval (FIRST, 0.25 s or twice the handshake *)
(* round trip, doubled after a pair whose small probe was lost, up to     *)
(* MAX, 1 s; back to FIRST at a rule-out): the next pair is due, and the  *)
(* server runs an update, which renews the mapping's two answer           *)
(* allowances; ServerUpdate is a further server update inside it.  Every  *)
(* pair whose small came back and whose large did not is a strike, and    *)
(* every strike counts: K in a row rule a candidate out, so a path that   *)
(* carries a smaller candidate settles in K pair intervals per size it    *)
(* does not carry.  The confirm is resent whenever none is in flight (its *)
(* 1 s interval is not modelled).  Timed TRUE: the interval covers a      *)
(* round trip, so a Tick waits until nothing of the                       *)
(* pair is in flight (the liveness and loss-only cases).  Timed FALSE is  *)
(* the code's untimed view: a pair's packets may arrive after it resolved *)
(* (late), until the pair after next reuses the pair number's parity,     *)
(* when what is left of it is dropped (the code's u32 pair number keeps   *)
(* them distinct; a packet two intervals late is taken as lost here).     *)
(*                                                                         *)
(*   ClientSendPair          the small probe and the large at Cur, one new *)
(*                           pair number; not while a large burst is on   *)
(*                           and burstStrikes = K - 1 (the loss assumption) *)
(*   ClientRecvSmall(r)      the small reply of the outstanding pair        *)
(*   ClientRecvLarge(r)      a large reply that opens, of any pair, taken  *)
(*                           only while probing at the walk's candidate:   *)
(*                           above P it raises P and sets the confirm; the  *)
(*                           walk ends, no pair outstanding; else ignored   *)
(*   ClientRecvForged(r)     a forged reply: the open fails, ignored        *)
(*   ClientResolvePair       when the next pair is due: large seen,         *)
(*                           success; small seen and large not, a strike;   *)
(*                           small not seen, evidence of loss: the          *)
(*                           candidate's strikes go back to zero            *)
(*   ClientRuleOut           K strikes in a row at a candidate above P rule *)
(*                           it out; the next starts fresh                  *)
(*   ClientFail              K strikes in a row at P0 itself: -14           *)
(*   ClientSendConfirm       the confirm of the raised P, at each probe     *)
(*                           update until its echo                         *)
(*   ClientRecvConfirmEcho(r) an echo carrying the confirmed size or larger *)
(*   ServerRecvProbe(p)      a probe from C or X: small or a size on the    *)
(*                           server's list, a connected mapping, the        *)
(*                           update's allowance for its kind; then the      *)
(*                           open, the allowance spent, the echo of the     *)
(*                           same kind and size                             *)
(*   ServerRecvForged(p)     any packet from F: the same checks, an open    *)
(*                           that fails, which spends nothing               *)
(*   ServerRecvConfirm(p)    a confirm from C: the small allowance; a size *)
(*                           on the list above the slot's P raises it, and *)
(*                           is echoed; off the list, ignored               *)
(*   ClientSendData, ServerSendData   a payload at P (no state here)         *)
(*   Tick, ServerUpdate      the clocks                                     *)
(*   LossBurst(k), Replay, AttackProbe, AttackReply   the environment       *)
(*   Done                    the terminal stutter                           *)
(*                                                                         *)
(* Broken selects a reversed witness:                                      *)
(*   "lower"        the client takes any large reply of a candidate, in   *)
(*                  any phase: a late reply below P lowers it             *)
(*                  (PNeverDecreases; untimed);                            *)
(*   "strikeonloss" a pair whose small reply was lost too counts a strike *)
(*                  (StrikeNeedsSmall);                                    *)
(*   "noreset"      a pair whose small probe was lost leaves the strikes *)
(*                  as they are, so two bursts of large-only loss, each    *)
(*                  under K strikes and with a loss of every packet        *)
(*                  between them, add up to K and rule a carried size out  *)
(*                  (NoWrongRuleOut);                                      *)
(*   "unboundedburst" not the code: the loss assumption dropped, so one   *)
(*                  burst of large-only loss meets K pairs in a row and    *)
(*                  rules a carried size out (NoWrongRuleOut): the bound   *)
(*                  is what the property needs;                            *)
(*   "raiseonprobe" the server raises its P when it answers a large probe, *)
(*                  without a confirm (ServerRaisesOnConfirm);             *)
(*   "forgedraise"  the client raises on a forged large reply that does   *)
(*                  not open (ClientRaisesOnAuthLarge);                    *)
(*   "probeaftermax" the walk does not end when P reaches the largest     *)
(*                  candidate, so the client sends another pair            *)
(*                  (NoProbeAfterDone);                                    *)
(*   "unproven"     the server answers a probe for a mapping that is not  *)
(*                  connected (ReplyOnlyConnected);                        *)
(*   "larger"       the server answers a large probe with its largest     *)
(*                  size, not the probe's (ReplyNoLarger);                 *)
(*   "offlist"      the server answers a size not on its own list         *)
(*                  (ReplyOwnList);                                        *)
(*   "noallowance"  the server answers every probe of a mapping in one    *)
(*                  update (OneReplyPerUpdate);                            *)
(*   "skiplast"     the walk ends without probing P0 itself, so a client  *)
(*                  whose path does not carry P0 never fails -14           *)
(*                  (FailsWhenP0NotCarried);                               *)
(*   "forgedspends" the allowance is spent before the open, so a forger   *)
(*                  at the client's address withholds every genuine probe  *)
(*                  (ForgedNeverStarves).                                  *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    ClientSet,      \* the client's candidate packet sizes for the family
    ServerList,     \* the server's own candidate set for the family
    CapsUp,         \* the path capacities client -> server to explore
    CapsDown,       \* the path capacities server -> client to explore
    K,              \* strikes that rule a candidate out (ROCKETNET_LINK_MTU_PROBE_STRIKES)
    MaxBursts,      \* the most loss bursts in a behaviour
    AttackBudget,   \* the most attacker sends (replays, packets from X or F, forged replies)
    Forger,         \* TRUE: packets from F without limit
    Timed,          \* TRUE: the pair interval covers a round trip
    Broken          \* "none", or the reversed witness

Max(S) == CHOOSE m \in S : \A x \in S : x <= m
Min(S) == CHOOSE m \in S : \A x \in S : x >= m

P0 == Min(ClientSet)
MaxC == Max(ClientSet)
N == Cardinality(ClientSet)
Probing == N > 1

ASSUME /\ ClientSet \subseteq Nat \ {0} /\ ClientSet # {}
       /\ ServerList \subseteq Nat \ {0} /\ ServerList # {}
       \* t12 rule 16: the server accepts P0 only from its own list
       /\ P0 \in ServerList
       /\ CapsUp \subseteq Nat /\ CapsUp # {}
       /\ CapsDown \subseteq Nat /\ CapsDown # {}
       /\ K \in Nat \ {0} /\ MaxBursts \in Nat /\ AttackBudget \in Nat
       /\ Forger \in BOOLEAN /\ Timed \in BOOLEAN
       /\ Broken \in {"none", "lower", "strikeonloss", "raiseonprobe",
                      "forgedraise", "probeaftermax", "unproven", "larger",
                      "offlist", "noallowance", "skiplast", "forgedspends",
                      "noreset", "unboundedburst"}

\* every size a packet can carry here; 0 is the 34-byte control size
Sizes == ClientSet \cup ServerList

\* packets: one record shape for both directions; pn is the pair number's
\* parity, n marks a replayed copy
Pkt(k, src, s, pn, n) == [k |-> k, src |-> src, s |-> s, pn |-> pn, n |-> n]
UpPackets ==
    {Pkt("small", a, 0, b, n) : a \in {"C", "F", "X"}, b \in {0, 1}, n \in {0, 1}}
    \cup {Pkt("large", a, s, b, n) : a \in {"C", "F", "X"}, s \in Sizes, b \in {0, 1}, n \in {0, 1}}
    \cup {Pkt("confirm", a, s, 0, n) : a \in {"C", "F"}, s \in Sizes, n \in {0, 1}}
DownPackets ==
    {Pkt("small", "S", 0, b, 0) : b \in {0, 1}}
    \cup {Pkt("large", a, s, b, 0) : a \in {"S", "F"}, s \in Sizes, b \in {0, 1}}
    \cup {Pkt("echo", "S", s, 0, 0) : s \in Sizes}

VARIABLES
    capUp,          \* the path's capacity client -> server (environment)
    capDown,        \* the path's capacity server -> client (environment)
    cP,             \* the client's P: its packet_bytes
    sP,             \* the server slot's P
    phase,          \* the client: "probing", "done" (the walk has ended), "failed" (-14)
    ruled,          \* the candidates ruled out
    strikes,        \* strikes against the walk's current candidate
    out,            \* a pair is outstanding (sent, not yet resolved)
    pairNo,         \* the outstanding pair's number (its parity)
    pairS,          \* the outstanding pair's large size
    pairInBurst,    \* a loss burst met the outstanding pair: on when it was sent, or started while it was out (environment)
    smallSeen,      \* the outstanding pair's small reply arrived
    largeSeen,      \* the outstanding pair's large reply arrived
    due,            \* the next pair is due (the interval has passed)
    confirmS,       \* the size the client confirms until its echo, 0 for none
    ansSmall,       \* the mapping answered a small probe or confirm this update
    ansLarge,       \* the mapping answered a large probe this update
    burst,          \* the loss burst on: "off", "all" or "large" (environment)
    bursts,         \* loss bursts so far (environment)
    burstStrikes,   \* strikes in the current row taken on a pair a burst met (environment)
    attacks,        \* attacker sends so far (environment)
    up,             \* packets in flight client -> server (environment)
    down,           \* packets in flight server -> client (environment)
    badUnproven,    \* ghost: a probe answered for a mapping that is not connected
    badLarger,      \* ghost: a reply larger than the probe it answers
    badOffList,     \* ghost: a large reply of a size not on the server's list
    badTwice        \* ghost: a second answer of one kind to one mapping in one update

vars == << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo, pairS,
           smallSeen, largeSeen, due, confirmS, ansSmall, ansLarge, burst, bursts, burstStrikes, pairInBurst,
           attacks, up, down, badUnproven, badLarger, badOffList,
           badTwice >>

\* the small probe, the confirm and its echo are 34 bytes: size 0 here
Wire(p) == IF p.k \in {"small", "confirm", "echo"} THEN 0 ELSE p.s

\* send: a packet sent during a burst, or larger than the direction's
\* capacity, never arrives
Lost(p) == burst = "all" \/ (burst = "large" /\ Wire(p) > 0)
SendUp(p, u) == IF Lost(p) \/ Wire(p) > capUp THEN u ELSE u \cup {p}
SendDown(p, d) == IF Lost(p) \/ Wire(p) > capDown THEN d ELSE d \cup {p}

Carried(s) == s \in ServerList /\ s <= capUp /\ s <= capDown

\* the loss assumption: loss alone never covers K consecutive pairs whose
\* small probes came back, so a burst may meet a pair that can still strike
\* only while the row it has made is under K - 1; "unboundedburst" drops it
BurstMayMeet == burstStrikes < K - 1 \/ Broken = "unboundedburst"

\* the walk: the candidates above P not ruled out, largest first; when none
\* is left and P is still P0, P0 itself
Above == {c \in ClientSet : c > cP /\ c \notin ruled}
Cur == IF Above # {} THEN Max(Above) ELSE cP

\* a probe packet or reply of the client's pairs (not an attacker's)
PairPkt(p) == p.k \in {"small", "large"} /\ p.src \in {"C", "S"}
PairInFlight == (\E p \in up : PairPkt(p)) \/ (\E r \in down : PairPkt(r))
ConfirmInFlight == (\E p \in up : p.k = "confirm" /\ p.src = "C")
                   \/ (\E r \in down : r.k = "echo")

Init ==
    /\ capUp \in CapsUp
    /\ capDown \in CapsDown
    /\ cP = P0
    /\ sP = P0
    /\ phase = IF Probing THEN "probing" ELSE "done"
    /\ ruled = {}
    /\ strikes = 0
    /\ out = FALSE
    /\ pairNo = 0
    /\ pairS = P0
    /\ pairInBurst = FALSE
    /\ smallSeen = FALSE
    /\ largeSeen = FALSE
    /\ due = TRUE                    \* the first pair goes at connect
    /\ confirmS = 0
    /\ ansSmall = FALSE
    /\ ansLarge = FALSE
    /\ burst = "off"
    /\ bursts = 0
    /\ burstStrikes = 0
    /\ attacks = 0
    /\ up = {}
    /\ down = {}
    /\ badUnproven = FALSE
    /\ badLarger = FALSE
    /\ badOffList = FALSE
    /\ badTwice = FALSE

-----------------------------------------------------------------------------
(* The client *)

\* the pair: the small probe and the large at the walk's candidate, one new
\* pair number; what is left of the pair two back (the same parity) is
\* dropped. "probeaftermax" keeps sending after the walk has ended. A pair
\* sent into a burst is met by it; into a large burst only while the loss
\* assumption allows (a pair sent into a burst of every packet loses its
\* small probe, so it cannot strike).
ClientSendPair ==
    /\ phase = "probing" \/ (Broken = "probeaftermax" /\ phase = "done")
    /\ due
    /\ ~out
    /\ strikes < K
    /\ burst = "large" => BurstMayMeet
    /\ LET b == 1 - pairNo
           old == {p \in up : PairPkt(p) /\ p.pn = b}
           u0 == up \ old
       IN /\ up' = SendUp(Pkt("large", "C", Cur, b, 0), SendUp(Pkt("small", "C", 0, b, 0), u0))
          /\ down' = down \ {r \in down : PairPkt(r) /\ r.pn = b}
          /\ pairNo' = b
    /\ out' = TRUE
    /\ pairS' = Cur
    /\ pairInBurst' = (burst # "off")
    /\ due' = FALSE
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, smallSeen,
                    largeSeen, confirmS, ansSmall, ansLarge, burst, bursts, burstStrikes, attacks >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice >>

ClientRecvSmall(r) ==
    /\ r \in down
    /\ r.k = "small" /\ r.src = "S"
    /\ phase # "failed"
    /\ down' = down \ {r}
    /\ smallSeen' = IF out /\ r.pn = pairNo THEN TRUE ELSE smallSeen
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, largeSeen, due, confirmS, ansSmall, ansLarge, burst, bursts, burstStrikes, pairInBurst,
                    attacks, up >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice >>

\* A large reply taken: only while probing and only at the walk's current
\* candidate, from any pair so far (rocketnet_client_process_probe). Above
\* P it raises P at once and sets the confirm, which goes at the next probe
\* update (ClientSendConfirm); at P itself (P0, every candidate above ruled
\* out) it raises nothing. Either way the walk ends, which leaves no pair
\* outstanding ("probeaftermax" does not end it: the pair is marked).
\* "lower" takes any large reply of a candidate, in any phase.
Takes(r) == IF Broken = "lower" THEN r.s \in ClientSet
            ELSE phase = "probing" /\ r.s = Cur

Take(r) ==
    /\ cP' = r.s
    /\ confirmS' = IF r.s > cP THEN r.s ELSE confirmS
    /\ IF Broken = "probeaftermax"
       THEN /\ largeSeen' = IF out /\ r.pn = pairNo THEN TRUE ELSE largeSeen
            /\ UNCHANGED << phase, out, smallSeen >>
       ELSE /\ phase' = "done"
            /\ out' = FALSE
            /\ smallSeen' = FALSE
            /\ largeSeen' = FALSE

\* a large reply that opens under the session server-to-client key; any
\* other (not probing, or not the walk's candidate) is consumed and ignored
ClientRecvLarge(r) ==
    /\ r \in down
    /\ r.k = "large" /\ r.src = "S"
    /\ phase # "failed"
    /\ down' = down \ {r}
    /\ IF Takes(r)
       THEN Take(r)
       ELSE UNCHANGED << cP, confirmS, phase, out, smallSeen, largeSeen >>
    /\ UNCHANGED << capUp, capDown, sP, ruled, strikes, pairNo, pairS, due,
                    ansSmall, ansLarge, burst, bursts, burstStrikes, pairInBurst,
                    attacks, up >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice >>

\* a forged reply fails the open and is ignored; "forgedraise" takes it
\* as an authenticated one above P
ClientRecvForged(r) ==
    /\ r \in down
    /\ r.src = "F"
    /\ phase # "failed"
    /\ down' = down \ {r}
    /\ IF Broken = "forgedraise" /\ r.s \in ClientSet /\ r.s > cP
       THEN Take(r)
       ELSE UNCHANGED << cP, confirmS, phase, out, smallSeen, largeSeen >>
    /\ UNCHANGED << capUp, capDown, sP, ruled, strikes, pairNo, pairS, due,
                    ansSmall, ansLarge, burst, bursts, burstStrikes, pairInBurst,
                    attacks, up >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice >>

\* a strike: small seen and large not, at the walk's candidate
Strike ==
    /\ phase = "probing" /\ ~largeSeen /\ pairS = Cur
    /\ smallSeen \/ Broken = "strikeonloss"

\* when the next pair is due: large seen, success (taken at arrival, where
\* the walk ended; only "probeaftermax" resolves one), strikes unchanged;
\* small seen and large not, a strike against the pair's candidate, always;
\* small not seen, evidence of loss: the candidate's strikes go back to
\* zero (the code also doubles its pair interval, not tracked here), and so
\* does the loss assumption's row. "noreset" leaves the strikes.
ClientResolvePair ==
    /\ phase # "failed"
    /\ due
    /\ out
    /\ out' = FALSE
    /\ smallSeen' = FALSE
    /\ largeSeen' = FALSE
    /\ IF Strike
       THEN /\ strikes' = strikes + 1
            /\ burstStrikes' = IF pairInBurst THEN burstStrikes + 1 ELSE burstStrikes
       ELSE IF ~smallSeen
       THEN /\ strikes' = IF Broken = "noreset" THEN strikes ELSE 0
            /\ burstStrikes' = 0
       ELSE UNCHANGED << strikes, burstStrikes >>
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, pairNo, pairS, due,
                    confirmS, ansSmall, ansLarge, burst, bursts, pairInBurst, attacks, up, down >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice >>

\* K strikes at a candidate above P rule it out; the walk moves to the next
\* one, fresh (no strikes, a new row), or to P0 itself; the walk ends when P is above P0 and none is left.
\* "skiplast" ends the walk without P0's own check.
ClientRuleOut ==
    /\ phase = "probing"
    /\ strikes = K
    /\ Above # {}
    /\ ruled' = ruled \cup {Cur}
    /\ strikes' = 0
    /\ burstStrikes' = 0
    /\ phase' = IF Above \ {Cur} = {} /\ (cP > P0 \/ Broken = "skiplast")
                THEN "done" ELSE "probing"
    /\ UNCHANGED << capUp, capDown, cP, sP, out, pairNo, pairS, smallSeen,
                    largeSeen, due, confirmS, ansSmall, ansLarge, burst, bursts, pairInBurst,
                    attacks, up, down >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice >>

\* K strikes at P0 itself: the path does not carry P0, -14
ClientFail ==
    /\ phase = "probing"
    /\ strikes = K
    /\ Above = {}
    /\ phase' = "failed"
    /\ UNCHANGED << capUp, capDown, cP, sP, ruled, strikes, out, pairNo, pairS,
                    smallSeen, largeSeen, due, confirmS, ansSmall, ansLarge,
                    burst, bursts, burstStrikes, pairInBurst, attacks, up, down >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice >>

\* the confirm goes at the next probe update after the raise and is resent
\* until its echo, whether or not the path loses it or a copy is in flight;
\* when Timed, one at a time
ClientSendConfirm ==
    /\ confirmS # 0
    /\ phase # "failed"
    /\ Timed => ~ConfirmInFlight
    /\ up' = SendUp(Pkt("confirm", "C", confirmS, 0, 0), up)
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, smallSeen, largeSeen, due, confirmS, ansSmall,
                    ansLarge, burst, bursts, burstStrikes, pairInBurst, attacks, down >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice >>

ClientRecvConfirmEcho(r) ==
    /\ r \in down
    /\ r.k = "echo"
    /\ phase # "failed"
    /\ down' = down \ {r}
    /\ confirmS' = IF confirmS # 0 /\ r.s >= confirmS THEN 0 ELSE confirmS
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, smallSeen, largeSeen, due, ansSmall, ansLarge, burst, bursts, burstStrikes, pairInBurst,
                    attacks, up >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice >>

\* a payload at the client's P: it crosses when P is within the path up,
\* which RaisedWithinPath states for every P above P0; no property waits
\* on payload, so it leaves no state here
ClientSendData ==
    /\ phase # "failed"
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, pairInBurst, smallSeen, largeSeen, due,
                    confirmS, ansSmall, ansLarge, burst, bursts, burstStrikes, attacks,
                    up, down >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice >>

-----------------------------------------------------------------------------
(* The server *)

\* the code's checks, in its order; each witness drops one
SizeOK(p) == p.k = "small" \/ p.s \in ServerList \/ Broken = "offlist"
Connected(p) == p.src \in {"C", "F"} \/ Broken = "unproven"   \* C's mapping is the slot
Allowed(p) ==
    \/ Broken = "noallowance"
    \/ p.src = "X"                       \* X's own mapping, its own allowance
    \/ p.k \in {"small", "confirm"} /\ ~ansSmall
    \/ p.k = "large" /\ ~ansLarge
ReplySize(p) == IF p.k = "large" /\ Broken = "larger" THEN Max(ServerList) ELSE p.s

\* the checks before the open pass
Reaches(p) == SizeOK(p) /\ Connected(p) /\ Allowed(p)

\* the allowance of the probe's kind, spent
SpendSmall(p) == IF p.k \in {"small", "confirm"} THEN TRUE ELSE ansSmall
SpendLarge(p) == IF p.k = "large" THEN TRUE ELSE ansLarge

\* a probe from C (or a replayed copy) or from X: X's probe opens under its
\* half-open mapping's session keys, and is not answered: no connection
ServerRecvProbe(p) ==
    /\ p \in up
    /\ p.k \in {"small", "large"}
    /\ p.src # "F"
    /\ up' = up \ {p}
    /\ IF Reaches(p)
       THEN /\ IF p.src = "C"
               THEN /\ ansSmall' = SpendSmall(p)
                    /\ ansLarge' = SpendLarge(p)
                    /\ down' = SendDown(Pkt(p.k, "S", ReplySize(p), p.pn, 0), down)
                    /\ IF (p.k = "small" /\ ansSmall) \/ (p.k = "large" /\ ansLarge)
                       THEN badTwice' = TRUE ELSE UNCHANGED badTwice
                    /\ UNCHANGED badUnproven
               ELSE /\ badUnproven' = TRUE      \* "unproven": the reply goes to X
                    /\ UNCHANGED << ansSmall, ansLarge, down, badTwice >>
            /\ IF ReplySize(p) > p.s THEN badLarger' = TRUE ELSE UNCHANGED badLarger
            /\ IF p.k = "large" /\ ReplySize(p) \notin ServerList
               THEN badOffList' = TRUE ELSE UNCHANGED badOffList
            /\ sP' = IF /\ Broken = "raiseonprobe" /\ p.src = "C"
                        /\ p.k = "large" /\ p.s \in ServerList /\ p.s > sP
                     THEN p.s ELSE sP
       ELSE UNCHANGED << ansSmall, ansLarge, down, sP, badUnproven, badLarger,
                         badOffList, badTwice >>
    /\ UNCHANGED << capUp, capDown, cP, phase, ruled, strikes, out, pairNo,
                    pairS, smallSeen, largeSeen, due, confirmS, burst, bursts, burstStrikes, pairInBurst,
                    attacks >>

\* any packet from F, at the client's address: it reaches the open and
\* fails, and spends nothing, so the genuine probe of the update is still
\* answered; "forgedspends" spends the allowance before the open
ServerRecvForged(p) ==
    /\ p \in up
    /\ p.src = "F"
    /\ up' = up \ {p}
    /\ IF Broken = "forgedspends" /\ Reaches(p)
       THEN /\ ansSmall' = SpendSmall(p)
            /\ ansLarge' = SpendLarge(p)
       ELSE UNCHANGED << ansSmall, ansLarge >>
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, smallSeen, largeSeen, due, confirmS, burst, bursts, burstStrikes, pairInBurst,
                    attacks, down >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice >>

\* a confirm from C: the small allowance; a size on the server's list above
\* the slot's P raises it (the server sends at it from then on), and any
\* confirm of a size on the list is echoed; one off the list spends the
\* allowance (the open came first) and is ignored: no echo, no raise
ServerRecvConfirm(p) ==
    /\ p \in up
    /\ p.k = "confirm" /\ p.src = "C"
    /\ up' = up \ {p}
    /\ IF Allowed(p)
       THEN /\ ansSmall' = TRUE
            /\ IF ansSmall THEN badTwice' = TRUE ELSE UNCHANGED badTwice
            /\ sP' = IF p.s \in ServerList /\ p.s > sP THEN p.s ELSE sP
            /\ down' = IF p.s \in ServerList
                       THEN SendDown(Pkt("echo", "S", p.s, 0, 0), down)
                       ELSE down
       ELSE UNCHANGED << ansSmall, badTwice, sP, down >>
    /\ UNCHANGED << capUp, capDown, cP, phase, ruled, strikes, out, pairNo,
                    pairS, smallSeen, largeSeen, due, confirmS, ansLarge, burst, bursts, burstStrikes, pairInBurst,
                    attacks >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList >>

\* a payload at the slot's P, down: as ClientSendData
ServerSendData ==
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, pairInBurst, smallSeen, largeSeen, due,
                    confirmS, ansSmall, ansLarge, burst, bursts, burstStrikes, attacks,
                    up, down >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice >>

-----------------------------------------------------------------------------
(* Time and the environment *)

\* one pair interval and the server's update in it: the next pair is due
\* and the allowances are renewed; when Timed, the interval covers the
\* pair's round trip
Tick ==
    /\ ~due
    /\ Timed => ~PairInFlight
    /\ due' = TRUE
    /\ ansSmall' = FALSE
    /\ ansLarge' = FALSE
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, smallSeen, largeSeen, confirmS, burst, bursts, burstStrikes, pairInBurst,
                    attacks, up, down >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice >>

ServerUpdate ==
    /\ ansSmall \/ ansLarge
    /\ ansSmall' = FALSE
    /\ ansLarge' = FALSE
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, smallSeen, largeSeen, due, confirmS, burst, bursts, burstStrikes, pairInBurst,
                    attacks, up, down >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice >>

\* a loss burst starts (everything in flight is dropped, both ways) or ends.
\* A burst of kind "all" drops every packet, "large" every packet above the
\* 34-byte control size (the large probe and its reply) while the small
\* partners cross. A burst that starts while a pair is out meets it; one
\* that can make it strike (large, or every packet once the small reply
\* is in) starts only while the loss assumption allows.
LossBurst(k) ==
    /\ IF k # "off"
       THEN /\ burst = "off"
            /\ bursts < MaxBursts
            /\ out /\ (k = "large" \/ smallSeen) => BurstMayMeet
            /\ burst' = k
            /\ bursts' = bursts + 1
            /\ pairInBurst' = (out \/ pairInBurst)
            /\ up' = {p \in up : k = "large" /\ Wire(p) = 0}
            /\ down' = {r \in down : k = "large" /\ Wire(r) = 0}
       ELSE /\ burst # "off"
            /\ burst' = "off"
            /\ UNCHANGED << bursts, pairInBurst, up, down >>
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, smallSeen, largeSeen, due,
                    confirmS, ansSmall, ansLarge, burstStrikes, attacks >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice >>

\* an on-path copy of a client packet in flight, the client's address its source
Replay(p) ==
    /\ attacks < AttackBudget
    /\ p \in up
    /\ p.src = "C" /\ p.n = 0 /\ p.k \in {"small", "large", "confirm"}
    /\ up' = up \cup {[p EXCEPT !.n = 1]}
    /\ up' # up
    /\ attacks' = attacks + 1
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, smallSeen, largeSeen, due, confirmS, ansSmall,
                    ansLarge, burst, bursts, burstStrikes, pairInBurst, down >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice >>

\* a packet from X (a probe under X's own half-open mapping) or from F (any
\* kind, at the client's address, failing the open); with Forger, F's are
\* not counted against the budget, one in flight at a time (that is enough
\* to reach the server before the genuine probe of every update)
AttackPackets ==
    {Pkt("small", a, 0, 0, 0) : a \in {"X", "F"}}
    \cup {Pkt("large", a, s, 0, 0) : a \in {"X", "F"}, s \in Sizes}
    \cup {Pkt("confirm", "F", s, 0, 0) : s \in Sizes}

AttackProbe(p) ==
    /\ \/ attacks < AttackBudget
       \/ Forger /\ p.src = "F" /\ ~\E q \in up : q.src = "F"
    /\ up' = up \cup {p}
    /\ up' # up
    /\ attacks' = IF Forger /\ p.src = "F" THEN attacks ELSE attacks + 1
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, smallSeen, largeSeen, due, confirmS, ansSmall,
                    ansLarge, burst, bursts, burstStrikes, pairInBurst, down >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice >>

\* a forged large reply at any size, sent to the client
AttackReply(s) ==
    /\ attacks < AttackBudget
    /\ down' = down \cup {Pkt("large", "F", s, 0, 0)}
    /\ down' # down
    /\ attacks' = attacks + 1
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, smallSeen, largeSeen, due, confirmS, ansSmall,
                    ansLarge, burst, bursts, burstStrikes, pairInBurst, up >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice >>

\* the walk has ended (or -14) and nothing is left to do
Done ==
    /\ phase \in {"done", "failed"}
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, smallSeen, largeSeen, due, confirmS, ansSmall,
                    ansLarge, burst, bursts, burstStrikes, pairInBurst, attacks, up, down, badUnproven,
                    badLarger, badOffList, badTwice >>

Next ==
    \/ ClientSendPair
    \/ \E r \in down : ClientRecvSmall(r)
    \/ \E r \in down : ClientRecvLarge(r)
    \/ \E r \in down : ClientRecvForged(r)
    \/ ClientResolvePair
    \/ ClientRuleOut
    \/ ClientFail
    \/ ClientSendConfirm
    \/ \E r \in down : ClientRecvConfirmEcho(r)
    \/ ClientSendData
    \/ \E p \in up : ServerRecvProbe(p)
    \/ \E p \in up : ServerRecvForged(p)
    \/ \E p \in up : ServerRecvConfirm(p)
    \/ ServerSendData
    \/ Tick
    \/ ServerUpdate
    \/ \E k \in {"off", "all", "large"} : LossBurst(k)
    \/ \E p \in up : Replay(p)
    \/ \E p \in AttackPackets : AttackProbe(p)
    \/ \E s \in Sizes : AttackReply(s)
    \/ Done

\* Every step of the two ends and every delivery is weakly fair; the clocks
\* too, and every burst ends (eventual delivery). Starting a burst and the
\* attacker are not; nor is data, which no property here waits on.
Fairness ==
    /\ WF_vars(ClientSendPair)
    /\ WF_vars(ClientResolvePair)
    /\ WF_vars(ClientRuleOut)
    /\ WF_vars(ClientFail)
    /\ WF_vars(ClientSendConfirm)
    /\ WF_vars(Tick)
    /\ WF_vars(ServerUpdate)
    /\ WF_vars(LossBurst("off"))
    /\ \A r \in DownPackets : WF_vars(ClientRecvSmall(r))
    /\ \A r \in DownPackets : WF_vars(ClientRecvLarge(r))
    /\ \A r \in DownPackets : WF_vars(ClientRecvForged(r))
    /\ \A r \in DownPackets : WF_vars(ClientRecvConfirmEcho(r))
    /\ \A p \in UpPackets : WF_vars(ServerRecvProbe(p))
    /\ \A p \in UpPackets : WF_vars(ServerRecvForged(p))
    /\ \A p \in UpPackets : WF_vars(ServerRecvConfirm(p))

Spec == Init /\ [][Next]_vars
FairSpec == Spec /\ Fairness

-----------------------------------------------------------------------------
(* Safety *)

TypeOK ==
    /\ capUp \in CapsUp /\ capDown \in CapsDown
    /\ cP \in ClientSet /\ sP \in ServerList
    /\ phase \in {"probing", "done", "failed"}
    /\ ruled \subseteq ClientSet
    /\ strikes \in 0..K
    /\ out \in BOOLEAN /\ pairNo \in {0, 1} /\ pairS \in ClientSet
    /\ {pairInBurst, smallSeen, largeSeen, due, ansSmall, ansLarge} \subseteq BOOLEAN
    /\ burst \in {"off", "all", "large"}
    /\ burstStrikes \in 0..K
    /\ confirmS \in {0} \cup ClientSet
    /\ bursts \in 0..MaxBursts
    /\ attacks \in 0..AttackBudget
    /\ up \subseteq UpPackets
    /\ down \subseteq DownPackets
    /\ {badUnproven, badLarger, badOffList, badTwice} \subseteq BOOLEAN

\* (1) P is never above what the path carries both ways: P0 is where both
\* ends start, and every raise above it is a size that crossed both ways
RaisedWithinPath ==
    /\ cP > P0 => Carried(cP)
    /\ sP > P0 => Carried(sP)

\* the server is never ahead of the client: it raises only to a size the
\* client confirmed
ServerNeverAhead == sP <= cP

\* (2) under loss alone (bursts and the path's capacities), nothing is ruled
\* out that the path carries; holds when Timed and under the loss
\* assumption (BurstMayMeet): K strikes in a row need loss of large packets
\* alone over K consecutive pairs whose small probes came back
NoWrongRuleOut == \A s \in ruled : ~Carried(s)

\* (3) -14 only when P0 itself collected K strikes
FailOnlyAtP0 == phase = "failed" => cP = P0 /\ strikes = K /\ Above = {}

\* (4) the server answers a probe only for a connected mapping, never with a
\* packet larger than the probe, only at sizes on its own list, and at most
\* one of each kind per mapping per update
ReplyOnlyConnected == ~badUnproven
ReplyNoLarger == ~badLarger
ReplyOwnList == ~badOffList
OneReplyPerUpdate == ~badTwice

\* P is never a candidate ruled out: a large reply is taken only at the
\* walk's current candidate, so a late reply of a candidate ruled out since
\* is ignored
NeverAtRuledOut == cP \notin ruled

(* Action properties *)

\* (5) P never decreases, on either side
PNeverDecreases == [][cP' >= cP /\ sP' >= sP]_vars

\* (6) a strike is counted only for a pair whose small reply came back
StrikeNeedsSmall == [][strikes' = strikes + 1 => smallSeen]_vars

\* (7) the server raises only on a confirm from the client carrying the size
ServerRaisesOnConfirm ==
    [][sP' > sP => \E p \in up : p.k = "confirm" /\ p.src = "C" /\ p.s = sP'
                                 /\ p \notin up']_vars

\* (8) the client raises only on an authenticated large reply of that size
ClientRaisesOnAuthLarge ==
    [][cP' > cP => \E r \in down : r.k = "large" /\ r.src = "S" /\ r.s = cP'
                                   /\ r \notin down']_vars

\* (9) probing ends for good: no pair once P is the largest candidate or the
\* walk has ended (a new pair number is a pair sent)
NoProbeAfterDone ==
    [][pairNo' # pairNo => phase = "probing" /\ cP < MaxC]_vars

\* Reachability, not a rule: when untimed, a large reply of an earlier pair
\* (its parity not the last pair's) raises P. MCRNPathProbeReachLateReply.cfg
\* must violate it, so the late path of the code is explored.
NoLateRaise ==
    [][~(cP' > cP /\ \E r \in down : r.k = "large" /\ r.src = "S"
                                    /\ r.pn # pairNo /\ r \notin down')]_vars

-----------------------------------------------------------------------------
(* Liveness, under FairSpec with Timed *)

Best == IF \E c \in ClientSet : Carried(c)
        THEN Max({c \in ClientSet : Carried(c)}) ELSE P0

\* the walk ends and both ends hold the same P from then on
Converges == <>[](phase # "probing" /\ cP = sP)

\* both ends reach the largest candidate the path carries
ReachesBest == Carried(P0) => <>[](cP = Best /\ sP = Best)

\* a path that does not carry P0 ends in -14
FailsWhenP0NotCarried == (Probing /\ ~Carried(P0)) => <>(phase = "failed")

\* a forger at the client's address, sending without limit, cannot starve the
\* walk: its packets spend nothing, so the genuine probes are answered
ForgedNeverStarves == (Forger /\ Carried(P0)) => <>(cP = Best /\ sP = Best)

=============================================================================
