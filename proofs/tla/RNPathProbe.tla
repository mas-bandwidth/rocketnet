----------------------------- MODULE RNPathProbe -----------------------------
(***************************************************************************)
(* The path packet size probe, v2: connect small, probe upward.  Both ends *)
(* start the connection at P0, the smallest candidate the client's list   *)
(* gives the family (the connection response carries it; t12 rule 16).    *)
(* After CONNECTED, a client whose list has more than one candidate walks  *)
(* up a ladder: the next candidate above P, smallest first, with pairs of *)
(* probes; an authenticated large reply raises P, confirms the raise to   *)
(* the server and goes on to the next size up; K strikes rule the size    *)
(* out and the walk waits, then re-tries the same size, for as long as    *)
(* the connection lasts (sizes above it are never tried before it is      *)
(* raised to); when the first step is ruled out for the first time, P0    *)
(* itself is checked first, and K strikes there are -14;                  *)
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
(* probe is at the next candidate above P (or at P0 itself, when the      *)
(* first step was ruled out).                                              *)
(*                                                                         *)
(* The path.  A capacity each way, chosen at Init from CapsUp and CapsDown *)
(* and fixed for the connection (a reused slot's connection has its own,  *)
(* from ReuseCapsUp and ReuseCapsDown): a packet larger than its          *)
(* direction's capacity is dropped when sent, always.  Loss is bursts     *)
(* (LossBurst): a burst of kind "all" drops every packet, of kind         *)
(* "large" every packet                                                    *)
(* above the control size (the large probe and its reply), in flight      *)
(* when it starts and sent while it lasts, both ways; at most MaxBursts   *)
(* bursts, and every burst ends.  The loss assumption (LossBound TRUE):   *)
(* loss of large packets alone never covers K consecutive pairs whose     *)
(* small probes came back.  LossBound FALSE drops it: heavy random loss,  *)
(* which rules out a size the path carries now and then, and the re-try   *)
(* is what then reaches it.  burstStrikes counts the strikes since the candidate's strikes   *)
(* last went to zero taken on a pair a burst met (pairInBurst); a burst   *)
(* may meet a pair that can still strike only while burstStrikes < K - 1, *)
(* so loss alone never makes K strikes in a row.  A pair whose small      *)
(* probe is lost breaks the row: the code sets strikes to zero, and the   *)
(* row starts again.  Packets in flight are a set, delivered in any       *)
(* order.  The server's list is part of what the path carries:            *)
(* Carried(S) is S on ServerList and S within both capacities.            *)
(* An on-path attacker may replay a client packet in flight (a second     *)
(* copy, n = 1), send packets from X or F, or a forged large reply,       *)
(* at most AttackBudget times in all, of the kinds in AttackKinds (with   *)
(* a budget of one, a case per kind covers every behaviour of all three); *)
(* with Forger, F sends without limit.                                    *)
(*                                                                         *)
(* Time.  Tick is one pair interval (FIRST, 0.1 s or twice the handshake  *)
(* round trip, doubled after a pair whose small probe was lost, up to     *)
(* MAX, 1 s; back to FIRST at a rule-out and a re-try), or the wait of a  *)
(* ruled-out size for its re-try (10 s, then doubling to 300 s: the       *)
(* arithmetic is RNPathProbeRetryLayout.tla's, not tracked here): the     *)
(* next pair (or the re-try) is due, and the server runs an update, which *)
(* renews the mapping's two answer allowances; ServerUpdate is a further  *)
(* server update inside it.  Every pair whose small came back and whose   *)
(* large did not is a strike, and every strike counts: K in a row rule a  *)
(* candidate out and the walk waits, so the walk takes one pair interval  *)
(* per size the path carries and K at the first it does not.  A raise     *)
(* makes the next pair due at once.                                       *)
(* The confirm is resent whenever none is in flight (its                  *)
(* 1 s interval is not modelled).  Timed TRUE: the interval covers a      *)
(* round trip, so a Tick waits until nothing of the                       *)
(* pair is in flight (the liveness and loss-only cases).  Timed FALSE is  *)
(* the code's untimed view: a pair's packets may arrive after it resolved *)
(* (late), until the pair after next reuses the pair number's parity,     *)
(* when what is left of it is dropped (the code's u32 pair number keeps   *)
(* them distinct; a packet two intervals late is taken as lost here).     *)
(* Timed TRUE also takes a re-try's wait to cover the confirm's exchange  *)
(* (ClientRetry).                                                          *)
(*                                                                         *)
(*   ClientSendPair          the small probe and the large at Cur, one new *)
(*                           pair number; not while a large burst is on   *)
(*                           and burstStrikes = K - 1 (the loss assumption) *)
(*   ClientRecvSmall(r)      the small reply of the outstanding pair        *)
(*   ClientRecvLarge(r)      a large reply that opens, of any pair, taken  *)
(*                           only while probing at the walk's candidate:   *)
(*                           above P it raises P, sets the confirm and     *)
(*                           goes on to the next size up (strikes zero, no *)
(*                           pair outstanding, the next pair due), or ends *)
(*                           the walk at the largest candidate; at P (the  *)
(*                           P0 check) it ends the walk; else ignored      *)
(*   ClientRecvForged(r)     a forged reply: the open fails, ignored        *)
(*   ClientResolvePair       when the next pair is due: large seen,         *)
(*                           success; small seen and large not, a strike;   *)
(*                           small not seen, evidence of loss: the          *)
(*                           candidate's strikes go back to zero            *)
(*   ClientRuleOut           K strikes in a row at a candidate above P rule *)
(*                           it out and the walk waits for its re-try; at  *)
(*                           its first rule-out at the first step (P still *)
(*                           P0) the walk goes on to P0's check first      *)
(*   ClientFail              K strikes in a row at P0 itself: -14           *)
(*   ClientRetry             the wait is over: the ruled-out size is probed *)
(*                           again, its strikes at zero, its pair due now   *)
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
(*   Reuse(cu, cd)           the connection ends and the slot is given to *)
(*                           a new one, at most MaxReuses times: the      *)
(*                           same client object connecting again, or a    *)
(*                           new client in the same server slot, over a   *)
(*                           path of capacities cu and cd; both ends start *)
(*                           as at Init, at P0 with nothing ruled out, no *)
(*                           strikes, no confirm, no wait, the walk's     *)
(*                           first pair due; what was in flight was      *)
(*                           sealed under the old keys and is dropped    *)
(*                           (ReuseStartsFresh)                           *)
(*                                                                         *)
(* Broken selects a reversed witness:                                      *)
(*   "reusekeep"    the server slot keeps its P across a reuse, so a new *)
(*                  connection starts above P0 (ReuseStartsFresh, and    *)
(*                  RaisedWithinPath on a smaller path);                 *)
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
(*   "noretry"      a rule-out ends the walk for good, as before the       *)
(*                  re-try: under heavy loss a carried size ruled out is   *)
(*                  never reached (RetryReaches);                          *)
(*   "aboveruled"   a rule-out does not stop the walk: it goes on to the  *)
(*                  sizes above the one ruled out (NeverAboveRuled);       *)
(*   "skipstep"     the walk skips a step: its candidate is the largest   *)
(*                  above P, not the next one up (NoSkippedStep);          *)
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
    AttackKinds,    \* the attacker sends a case explores: "replay", "probe" (from X or F), "reply" (forged)
    Forger,         \* TRUE: packets from F without limit
    Timed,          \* TRUE: the pair interval covers a round trip
    LossBound,      \* TRUE: the loss assumption; FALSE: heavy loss may rule out a carried size
    MaxReuses,      \* the most times the connection ends and its slot is reused
    ReuseCapsUp,    \* the path capacities client -> server a connection in a reused slot may have
    ReuseCapsDown,  \* the path capacities server -> client a connection in a reused slot may have
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
       /\ AttackKinds \subseteq {"replay", "probe", "reply"}
       /\ Forger \in BOOLEAN /\ Timed \in BOOLEAN /\ LossBound \in BOOLEAN
       /\ MaxReuses \in Nat
       /\ ReuseCapsUp \subseteq Nat /\ ReuseCapsDown \subseteq Nat
       /\ Broken \in {"none", "lower", "strikeonloss", "raiseonprobe",
                      "forgedraise", "probeaftermax", "unproven", "larger",
                      "offlist", "noallowance", "skiplast", "forgedspends",
                      "noreset", "unboundedburst", "aboveruled", "skipstep",
                      "noretry", "reusekeep"}

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
    phase,          \* the client: "probing" (a step or its re-try), "check" (P0's own check),
                    \* "waiting" (for a re-try), "done" (the walk has ended), "failed" (-14)
    ruled,          \* the candidates ruled out and not since raised to
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
    badTwice,       \* ghost: a second answer of one kind to one mapping in one update
    reuses,         \* reuses of the slot so far (environment)
    badStale        \* ghost: a reuse left a probe variable of the old connection

vars == << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo, pairS,
           smallSeen, largeSeen, due, confirmS, ansSmall, ansLarge, burst, bursts, burstStrikes, pairInBurst,
           attacks, up, down, badUnproven, badLarger, badOffList,
           badTwice, reuses, badStale >>

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
\* only while the row it has made is under K - 1; "unboundedburst" and
\* LossBound FALSE drop it
BurstMayMeet == burstStrikes < K - 1 \/ Broken = "unboundedburst" \/ ~LossBound

\* the walk, a ladder: the next candidate above P, smallest first. A
\* ruled-out size stays the walk's candidate, re-tried after a wait, so
\* nothing above it is tried before it is raised to; in P0's own check the
\* candidate is P0 itself, P. "aboveruled" goes on past a rule-out to the
\* sizes above it; "skipstep" takes the largest above P, not the next.
Above == {c \in ClientSet : c > cP}
Ladder == IF Broken = "aboveruled" THEN Above \ ruled ELSE Above
Cur == IF phase = "check" \/ Ladder = {} THEN cP
       ELSE IF Broken = "skipstep" THEN Max(Ladder) ELSE Min(Ladder)

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
    /\ reuses = 0
    /\ badStale = FALSE

-----------------------------------------------------------------------------
(* The client *)

\* the pair: the small probe and the large at the walk's candidate, one new
\* pair number; what is left of the pair two back (the same parity) is
\* dropped. "probeaftermax" keeps sending after the walk has ended. A pair
\* sent into a burst is met by it; into a large burst only while the loss
\* assumption allows (a pair sent into a burst of every packet loses its
\* small probe, so it cannot strike). When Timed, the pair a raise makes
\* due at once keeps the interval's promise: nothing of the last pair (a
\* replayed copy included) is still in flight, as for Tick, and it reaches
\* the server in a later update than the large reply it follows, so no
\* earlier large probe spends the large allowance its own needs. Untimed,
\* both can happen: a strike without loss (NoWrongRuleOut is not claimed).
ClientSendPair ==
    /\ phase \in {"probing", "check"} \/ (Broken = "probeaftermax" /\ phase = "done")
    /\ due
    /\ ~out
    /\ strikes < K
    /\ burst = "large" => BurstMayMeet
    /\ Timed => ~PairInFlight /\ ~ansLarge
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
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice, reuses, badStale >>

ClientRecvSmall(r) ==
    /\ r \in down
    /\ r.k = "small" /\ r.src = "S"
    /\ phase # "failed"
    /\ down' = down \ {r}
    /\ smallSeen' = IF out /\ r.pn = pairNo THEN TRUE ELSE smallSeen
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, largeSeen, due, confirmS, ansSmall, ansLarge, burst, bursts, burstStrikes, pairInBurst,
                    attacks, up >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice, reuses, badStale >>

\* A large reply taken: only while probing (or in P0's check), not while
\* the walk waits, and only at the walk's current candidate, from any pair
\* so far (rocketnet_client_process_probe). Above P it raises P at once
\* and sets the confirm, which goes at the next probe update
\* (ClientSendConfirm), the size is no longer ruled out, and the walk goes
\* on to the next size up: no pair outstanding, the strikes and the loss
\* assumption's row at zero, the next pair due at once; at the largest
\* candidate the walk ends ("probeaftermax" does not end it). At P itself
\* (P0's check, the first step ruled out) it raises nothing and the walk
\* waits for the first step's re-try, which is not due yet ("noretry": the
\* walk ends at P0). "lower" takes any large reply of a candidate, in any
\* phase.
Takes(r) == IF Broken = "lower" THEN r.s \in ClientSet
            ELSE phase \in {"probing", "check"} /\ r.s = Cur

Take(r) ==
    /\ cP' = r.s
    /\ confirmS' = IF r.s > cP THEN r.s ELSE confirmS
    /\ ruled' = ruled \ {r.s}
    /\ out' = FALSE
    /\ smallSeen' = FALSE
    /\ largeSeen' = FALSE
    /\ IF r.s > cP /\ (r.s < MaxC \/ Broken = "probeaftermax")
       THEN /\ phase' = "probing"
            /\ strikes' = 0
            /\ burstStrikes' = 0
            /\ due' = TRUE
       ELSE IF r.s = cP /\ phase = "check" /\ Broken # "noretry"
       THEN /\ phase' = "waiting"
            /\ strikes' = 0
            /\ burstStrikes' = 0
            /\ due' = FALSE
       ELSE /\ phase' = "done"
            /\ UNCHANGED << strikes, burstStrikes, due >>

\* a large reply that opens under the session server-to-client key; any
\* other (not probing, or not the walk's candidate) is consumed and ignored
ClientRecvLarge(r) ==
    /\ r \in down
    /\ r.k = "large" /\ r.src = "S"
    /\ phase # "failed"
    /\ down' = down \ {r}
    /\ IF Takes(r)
       THEN Take(r)
       ELSE UNCHANGED << cP, confirmS, ruled, phase, out, smallSeen, largeSeen,
                         strikes, burstStrikes, due >>
    /\ UNCHANGED << capUp, capDown, sP, pairNo, pairS,
                    ansSmall, ansLarge, burst, bursts, pairInBurst,
                    attacks, up >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice, reuses, badStale >>

\* a forged reply fails the open and is ignored; "forgedraise" takes it
\* as an authenticated one above P
ClientRecvForged(r) ==
    /\ r \in down
    /\ r.src = "F"
    /\ phase # "failed"
    /\ down' = down \ {r}
    /\ IF Broken = "forgedraise" /\ r.s \in ClientSet /\ r.s > cP
       THEN Take(r)
       ELSE UNCHANGED << cP, confirmS, ruled, phase, out, smallSeen, largeSeen,
                         strikes, burstStrikes, due >>
    /\ UNCHANGED << capUp, capDown, sP, pairNo, pairS,
                    ansSmall, ansLarge, burst, bursts, pairInBurst,
                    attacks, up >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice, reuses, badStale >>

\* a strike: small seen and large not, at the walk's candidate
Strike ==
    /\ phase \in {"probing", "check"} /\ ~largeSeen /\ pairS = Cur
    /\ smallSeen \/ Broken = "strikeonloss"

\* when the next pair is due: large seen, success (taken at arrival, where
\* the pair stopped being outstanding, so none resolves here), strikes unchanged;
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
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice, reuses, badStale >>

\* K strikes at a candidate above P rule it out, and the walk waits for its
\* re-try (the next time of the size's schedule, not yet due): sizes above
\* it are never tried before it is raised to. At the size's first rule-out
\* at the first step (P still P0, no raise, the size not ruled out before)
\* the walk goes on to P0's own check first, fresh (no strikes, a new row,
\* the pair due now). A re-try ruled out again waits for the next.
\* "skiplast" waits without P0's own check; "aboveruled" goes on to the
\* sizes above the one ruled out; "noretry" ends the walk for good.
ClientRuleOut ==
    /\ phase = "probing"
    /\ strikes = K
    /\ Cur # cP
    /\ ruled' = ruled \cup {Cur}
    /\ strikes' = 0
    /\ burstStrikes' = 0
    /\ IF \/ cP = P0 /\ Cur \notin ruled /\ Broken # "skiplast"
          \/ Broken = "aboveruled" /\ Ladder \ {Cur} # {}
       THEN /\ phase' = IF Broken = "aboveruled" /\ Ladder \ {Cur} # {}
                        THEN "probing" ELSE "check"
            /\ UNCHANGED due
       ELSE /\ phase' = IF Broken = "noretry" THEN "done" ELSE "waiting"
            /\ due' = FALSE
    /\ UNCHANGED << capUp, capDown, cP, sP, out, pairNo, pairS, smallSeen,
                    largeSeen, confirmS, ansSmall, ansLarge, burst, bursts, pairInBurst,
                    attacks, up, down >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice, reuses, badStale >>

\* K strikes at P0 itself (P0's check): the path does not carry P0, -14
ClientFail ==
    /\ phase = "check"
    /\ strikes = K
    /\ phase' = "failed"
    /\ UNCHANGED << capUp, capDown, cP, sP, ruled, strikes, out, pairNo, pairS,
                    smallSeen, largeSeen, due, confirmS, ansSmall, ansLarge,
                    burst, bursts, burstStrikes, pairInBurst, attacks, up, down >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice, reuses, badStale >>

\* the wait is over (its time is due): the walk probes its ruled-out
\* candidate again as a fresh step, its strikes at zero, its first pair due
\* now (rocketnet_client_protocol_probe_retry); the size stays ruled out
\* until a large reply raises P to it. When Timed, the wait (10 s at least,
\* ROCKETNET_LINK_MTU_RETRY_FIRST_SECONDS) covers the confirm's exchange (a
\* confirm a second, ROCKETNET_LINK_MTU_CONFIRM_INTERVAL_SECONDS, with no
\* probe to spend the server's allowance first): the re-try starts after the
\* echo of a raise before it.  Without it the model's one allowance a pair
\* interval lets each re-try's small probe beat the confirm for ever.
ClientRetry ==
    /\ phase = "waiting"
    /\ due
    /\ ~out
    /\ Timed => confirmS = 0
    /\ phase' = "probing"
    /\ strikes' = 0
    /\ UNCHANGED << capUp, capDown, cP, sP, ruled, out, pairNo, pairS,
                    smallSeen, largeSeen, due, confirmS, ansSmall, ansLarge,
                    burst, bursts, burstStrikes, pairInBurst, attacks, up, down >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice, reuses, badStale >>

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
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice, reuses, badStale >>

ClientRecvConfirmEcho(r) ==
    /\ r \in down
    /\ r.k = "echo"
    /\ phase # "failed"
    /\ down' = down \ {r}
    /\ confirmS' = IF confirmS # 0 /\ r.s >= confirmS THEN 0 ELSE confirmS
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, smallSeen, largeSeen, due, ansSmall, ansLarge, burst, bursts, burstStrikes, pairInBurst,
                    attacks, up >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice, reuses, badStale >>

\* a payload at the client's P: it crosses when P is within the path up,
\* which RaisedWithinPath states for every P above P0; no property waits
\* on payload, so it leaves no state here
ClientSendData ==
    /\ phase # "failed"
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, pairInBurst, smallSeen, largeSeen, due,
                    confirmS, ansSmall, ansLarge, burst, bursts, burstStrikes, attacks,
                    up, down >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice, reuses, badStale >>

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
                    attacks, reuses, badStale >>

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
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice, reuses, badStale >>

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
    /\ UNCHANGED << badUnproven, badLarger, badOffList, reuses, badStale >>

\* a payload at the slot's P, down: as ClientSendData
ServerSendData ==
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, pairInBurst, smallSeen, largeSeen, due,
                    confirmS, ansSmall, ansLarge, burst, bursts, burstStrikes, attacks,
                    up, down >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice, reuses, badStale >>

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
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice, reuses, badStale >>

ServerUpdate ==
    /\ ansSmall \/ ansLarge
    /\ ansSmall' = FALSE
    /\ ansLarge' = FALSE
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, smallSeen, largeSeen, due, confirmS, burst, bursts, burstStrikes, pairInBurst,
                    attacks, up, down >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice, reuses, badStale >>

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
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice, reuses, badStale >>

\* an on-path copy of a client packet in flight, the client's address its source
Replay(p) ==
    /\ "replay" \in AttackKinds
    /\ attacks < AttackBudget
    /\ p \in up
    /\ p.src = "C" /\ p.n = 0 /\ p.k \in {"small", "large", "confirm"}
    /\ up' = up \cup {[p EXCEPT !.n = 1]}
    /\ up' # up
    /\ attacks' = attacks + 1
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, smallSeen, largeSeen, due, confirmS, ansSmall,
                    ansLarge, burst, bursts, burstStrikes, pairInBurst, down >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice, reuses, badStale >>

\* a packet from X (a probe under X's own half-open mapping) or from F (any
\* kind, at the client's address, failing the open); with Forger, F's are
\* not counted against the budget, one in flight at a time (that is enough
\* to reach the server before the genuine probe of every update)
AttackPackets ==
    {Pkt("small", a, 0, 0, 0) : a \in {"X", "F"}}
    \cup {Pkt("large", a, s, 0, 0) : a \in {"X", "F"}, s \in Sizes}
    \cup {Pkt("confirm", "F", s, 0, 0) : s \in Sizes}

AttackProbe(p) ==
    /\ \/ "probe" \in AttackKinds /\ attacks < AttackBudget
       \/ Forger /\ p.src = "F" /\ ~\E q \in up : q.src = "F"
    /\ up' = up \cup {p}
    /\ up' # up
    /\ attacks' = IF Forger /\ p.src = "F" THEN attacks ELSE attacks + 1
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, smallSeen, largeSeen, due, confirmS, ansSmall,
                    ansLarge, burst, bursts, burstStrikes, pairInBurst, down >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice, reuses, badStale >>

\* a forged large reply at any size, sent to the client
AttackReply(s) ==
    /\ "reply" \in AttackKinds
    /\ attacks < AttackBudget
    /\ down' = down \cup {Pkt("large", "F", s, 0, 0)}
    /\ down' # down
    /\ attacks' = attacks + 1
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, smallSeen, largeSeen, due, confirmS, ansSmall,
                    ansLarge, burst, bursts, burstStrikes, pairInBurst, up >>
    /\ UNCHANGED << badUnproven, badLarger, badOffList, badTwice, reuses, badStale >>

\* the walk has ended (or -14) and nothing is left to do
Done ==
    /\ phase \in {"done", "failed"}
    /\ UNCHANGED << capUp, capDown, cP, sP, phase, ruled, strikes, out, pairNo,
                    pairS, smallSeen, largeSeen, due, confirmS, ansSmall,
                    ansLarge, burst, bursts, burstStrikes, pairInBurst, attacks, up, down, badUnproven,
                    badLarger, badOffList, badTwice, reuses, badStale >>

\* a connection's probe state as it starts (Init): both ends at P0, nothing
\* ruled out, no strikes, no pair out, no confirm, no answer spent, the
\* walk's first pair due
FreshProbe ==
    /\ cP = P0 /\ sP = P0
    /\ phase = IF Probing THEN "probing" ELSE "done"
    /\ ruled = {} /\ strikes = 0 /\ out = FALSE /\ pairNo = 0 /\ pairS = P0
    /\ smallSeen = FALSE /\ largeSeen = FALSE /\ due = TRUE /\ confirmS = 0
    /\ ansSmall = FALSE /\ ansLarge = FALSE /\ pairInBurst = FALSE /\ burstStrikes = 0

\* the connection ends, in any state, and its slot is given to a new one:
\* the same client object connecting again or a new client, over a path of
\* capacities cu and cd (rocketnet_client_connection_reset at the
\* disconnect and again at the connect, rocketnet_server_connection_reset
\* when the slot is freed, and the handshake, which sets both ends' P to
\* P0), the new path from ReuseCapsUp and ReuseCapsDown (kept apart
\* from CapsUp and CapsDown so that a trace's Init has the one path its
\* first connection had). The packets in flight were sealed under the old connection's keys
\* and are dropped. Outside a burst, so the new connection's first pair is
\* not met by one it did not start in; the budgets of bursts and attacks
\* are the behaviour's. "reusekeep" keeps the server slot's P.
Reuse(cu, cd) ==
    /\ reuses < MaxReuses
    /\ burst = "off"
    /\ capUp' = cu
    /\ capDown' = cd
    /\ cP' = P0
    /\ sP' = IF Broken = "reusekeep" THEN sP ELSE P0
    /\ phase' = IF Probing THEN "probing" ELSE "done"
    /\ ruled' = {}
    /\ strikes' = 0
    /\ out' = FALSE
    /\ pairNo' = 0
    /\ pairS' = P0
    /\ pairInBurst' = FALSE
    /\ smallSeen' = FALSE
    /\ largeSeen' = FALSE
    /\ due' = TRUE
    /\ confirmS' = 0
    /\ ansSmall' = FALSE
    /\ ansLarge' = FALSE
    /\ burstStrikes' = 0
    /\ up' = {}
    /\ down' = {}
    /\ reuses' = reuses + 1
    /\ badStale' = ~FreshProbe'
    /\ UNCHANGED << burst, bursts, attacks, badUnproven, badLarger, badOffList, badTwice >>

Next ==
    \/ ClientSendPair
    \/ \E r \in down : ClientRecvSmall(r)
    \/ \E r \in down : ClientRecvLarge(r)
    \/ \E r \in down : ClientRecvForged(r)
    \/ ClientResolvePair
    \/ ClientRuleOut
    \/ ClientFail
    \/ ClientRetry
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
    \/ \E cu \in ReuseCapsUp, cd \in ReuseCapsDown : Reuse(cu, cd)
    \/ Done

\* Every step of the two ends and every delivery is weakly fair; the clocks
\* too, and every burst ends (eventual delivery). Starting a burst and the
\* attacker are not; nor is data, which no property here waits on.
Fairness ==
    /\ WF_vars(ClientSendPair)
    /\ WF_vars(ClientResolvePair)
    /\ WF_vars(ClientRuleOut)
    /\ WF_vars(ClientFail)
    /\ WF_vars(ClientRetry)
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
    /\ capUp \in CapsUp \cup ReuseCapsUp /\ capDown \in CapsDown \cup ReuseCapsDown
    /\ cP \in ClientSet /\ sP \in ServerList
    /\ phase \in {"probing", "check", "waiting", "done", "failed"}
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
    /\ {badUnproven, badLarger, badOffList, badTwice, badStale} \subseteq BOOLEAN
    /\ reuses \in 0..MaxReuses

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
FailOnlyAtP0 == phase = "failed" => cP = P0 /\ strikes = K /\ ruled # {}

\* the ladder holds at a rule-out: no pair, and no large probe of the
\* client's in flight, at a size above one ruled out (and not since raised
\* to)
NeverAboveRuled ==
    \A s \in ruled :
        /\ pairS <= s
        /\ \A p \in up : (p.k = "large" /\ p.src = "C") => p.s <= s

\* (4) the server answers a probe only for a connected mapping, never with a
\* packet larger than the probe, only at sizes on its own list, and at most
\* one of each kind per mapping per update
ReplyOnlyConnected == ~badUnproven
ReplyNoLarger == ~badLarger
ReplyOwnList == ~badOffList
OneReplyPerUpdate == ~badTwice

\* (4b) a reused slot starts fresh: every probe variable of a connection
\* that follows a reuse is its Init value, so nothing of the old
\* connection's walk (its P, a rule-out, strikes, a wait, a confirm, an
\* answer spent) reaches the new one
ReuseStartsFresh == ~badStale

\* P is never a candidate ruled out: a size is ruled out only above P, and
\* a raise to it ends its rule-out
NeverAtRuledOut == cP \notin ruled

\* at most one size is ruled out at a time, the walk's candidate: the next
\* size up from P
RuledIsTheNextStep == \A s \in ruled : Above # {} /\ s = Min(Above)

(* Action properties *)

\* (5) P never decreases, on either side, within a connection (a reuse
\* starts the next at P0); (9) and (10) are within a connection too
PNeverDecreases == [][reuses' = reuses => cP' >= cP /\ sP' >= sP]_vars

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

\* (9) probing ends for good at the largest candidate: no pair once P is the
\* largest candidate or the walk has ended, and none while it waits for a
\* re-try (a new pair number is a pair sent)
NoProbeAfterDone ==
    [][reuses' = reuses /\ pairNo' # pairNo => phase \in {"probing", "check"} /\ cP < MaxC]_vars

\* (10) the ladder skips no step: every pair is at the smallest candidate
\* above P not ruled out, or at P itself for P0's check once the first
\* step was ruled out
NoSkippedStep ==
    [][reuses' = reuses /\ pairNo' # pairNo =>
           \/ Above # {} /\ pairS' = Min(Above)
           \/ ruled # {} /\ pairS' = cP]_vars

\* Reachability, not a rule: when untimed, a large reply of an earlier pair
\* (its parity not the last pair's) raises P. MCRNPathProbeReachLateReply.cfg
\* must violate it, so the late path of the code is explored.
NoLateRaise ==
    [][~(cP' > cP /\ \E r \in down : r.k = "large" /\ r.src = "S"
                                    /\ r.pn # pairNo /\ r \notin down')]_vars

\* Reachability, not a rule: a re-try gets through, raising P to a size that
\* was ruled out. MCRNPathProbeReachRetryRaise.cfg must violate it, so the
\* re-try's success is explored.
NoRetryRaise == [][~(cP' > cP /\ cP' \in ruled)]_vars

-----------------------------------------------------------------------------
(* Liveness, under FairSpec with Timed *)

\* the top of the ladder: the largest candidate s such that the path
\* carries every client candidate above P0 up to s (a size it does not
\* carry, off the server's list or above a capacity, stops the walk there)
Best == Max({s \in ClientSet : \A c \in ClientSet : (c > P0 /\ c <= s) => Carried(c)})

\* both ends hold the same P from some point on (the walk may go on
\* re-trying a size the path does not carry, at no change to P)
Converges == <>[](cP = sP)

\* a wait for a re-try always ends in the re-try
WaitEnds == phase = "waiting" ~> phase = "probing"

\* a size the path carries that was ruled out (heavy loss, LossBound FALSE)
\* is reached once the loss ends: past P0's check, which a re-try never
\* repeats, P comes to it ("noretry" ends the walk at the rule-out). ruled
\* holds one size at most, the next step up (RuledIsTheNextStep), and only a
\* raise to that size takes it out of ruled (ClientRecvLarge), so ruled
\* empty again is P at the size: one leads-to, not one for each size, which
\* TLC checks as one branch of the liveness check instead of three
RetryReaches ==
    (\E s \in ruled : Carried(s)) /\ phase \notin {"check", "failed"} ~> ruled = {}

\* both ends reach the top of the ladder the path carries
ReachesBest == Carried(P0) => <>[](cP = Best /\ sP = Best)

\* a path that does not carry P0 ends in -14
FailsWhenP0NotCarried == (Probing /\ ~Carried(P0)) => <>(phase = "failed")

\* a forger at the client's address, sending without limit, cannot starve the
\* walk: its packets spend nothing, so the genuine probes are answered
ForgedNeverStarves == (Forger /\ Carried(P0)) => <>(cP = Best /\ sP = Best)

=============================================================================
