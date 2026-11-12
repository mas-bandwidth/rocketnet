------------------------- MODULE RNReceiveAllowance -------------------------
(***************************************************************************)
(* rocketnet: the server's per-client receive allowance (the code is      *)
(* rocketnet_server_receive_allowance_take, _take_packet and _start and   *)
(* their callers in sdk/rocketnet_server_protocol.cpp; the setting is     *)
(* rocketnet_server_config_t per_client_receive_kbps).                    *)
(*                                                                         *)
(* Each client slot holds a byte allowance. It is refilled from the time  *)
(* argument of the packet's update at R bytes per tick, R = Factor x B    *)
(* where B is the setting, capped at Cap = R x T + MaxPkt (T the          *)
(* connection's timeout in ticks, MaxPkt one maximum packet), and full    *)
(* when the connection starts. An authenticated packet that fits spends   *)
(* its bytes; one that does not is refused, counted, and spends nothing.  *)
(* Only time past the last refill adds, so a time argument that steps     *)
(* back credits nothing twice.                                            *)
(*                                                                         *)
(* Two kinds of packet the client sends outside its send budget pay first *)
(* from a credit of their own, and from the bytes only when it is empty:  *)
(*   a standalone ack ("ack") spends an ack credit: the server's session  *)
(*     sequences taken this connection less its own standalone acks       *)
(*     (seq - spent, spent counting from the connection's first sequence  *)
(*     and up by each server ack and each credited client ack);           *)
(*   a keep-alive ("ka") or a disconnect ("dc") spends a control token,   *)
(*     refilled like the bytes at Factor per tick (one keep-alive a tick  *)
(*     is the client's send rate) up to CtlCap = Factor x T + 2.          *)
(* A payload ("pay") pays bytes only.                                     *)
(*                                                                         *)
(* A correct client sends payload through its own send budget: a bucket   *)
(* of depth MaxPkt refilled at B per tick. It owes an ack for each        *)
(* eliciting packet of the server's it receives (some are lost), and      *)
(* sends a standalone ack only while it owes one; any send pays the debt. *)
(* It sends at most one keep-alive a tick, and one disconnect, its last   *)
(* packet. Its control packets are MaxPkt bytes, the worst for the bytes. *)
(* Its packets reach the server up to T-1 ticks late (an outage shorter   *)
(* than the timeout) and may arrive all at once. A flooder sends          *)
(* anything, FloodPerTick packets a tick. The server's time argument      *)
(* follows the clock but may step back (MaxBack times), and a connection  *)
(* may end and the slot be reused (MaxReconnect times).                   *)
(*                                                                         *)
(* Constants flip the guarding rules, one per reversed witness:           *)
(*   Refill = FALSE     the allowance never refills: a client under 1x is *)
(*                      refused. Fails UnderRateNeverRefused.             *)
(*   CapFull = FALSE    the cap is one tick of R: the burst after a       *)
(*                      silence is refused. Fails UnderRateNeverRefused.  *)
(*   PerClient = FALSE  one allowance for every slot: refusing the        *)
(*                      flooder changes the correct client. Fails         *)
(*                      Isolated.                                         *)
(*   Monotone = FALSE   a step back in time refills by its distance:      *)
(*                      time going back mints bytes. Fails NoMint.        *)
(*   AckCredit = FALSE  standalone acks pay bytes: the acks of a lossy    *)
(*                      stream refuse a correct client. Fails             *)
(*                      UnderRateNeverRefused.                            *)
(*   Control = FALSE    keep-alives and disconnects pay bytes: a correct  *)
(*                      client's keep-alives are refused. Fails           *)
(*                      UnderRateNeverRefused.                            *)
(*   CreditBound = FALSE  an ack is credited whatever the server sent: a  *)
(*                      flooder's acks are never refused. Fails           *)
(*                      AcksBounded.                                      *)
(*   ServerAckSpends = FALSE  the server's own standalone acks give       *)
(*                      credit. Fails AcksBounded.                        *)
(* With MaxElicit = 0 and Kinds = {"pay"} the instance is the byte        *)
(* allowance alone, as its first four witnesses check it.                 *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    Correct,        \* clients that send through their budget
    Flooders,       \* clients that send anything
    B,              \* the setting: the correct client's budget per tick
    Factor,         \* ROCKETNET_RECEIVE_ALLOWANCE_FACTOR
    MaxPkt,         \* one maximum packet, the least depth of a client's bucket
    T,              \* the connection's timeout in ticks; delivery is at most T-1 late
    Horizon,        \* the last tick
    FloodPerTick,   \* flooder packets per tick
    MaxNet,         \* correct packets in flight at once
    MaxBack,        \* steps back of the server's time argument
    MaxReconnect,   \* slot reuses
    MaxElicit,      \* server session packets per client per connection (eliciting and acks)
    Kinds,          \* the packet kinds clients send: {"pay"} or all four
    Refill,         \* TRUE: refill from time (FALSE: BrokenNoRefill)
    CapFull,        \* TRUE: Cap = R x T + MaxPkt (FALSE: BrokenSmallCap)
    PerClient,      \* TRUE: one allowance per slot (FALSE: BrokenShared)
    Monotone,       \* TRUE: only time past the last refill adds (FALSE: BrokenTimeBack)
    AckCredit,      \* TRUE: a standalone ack spends an ack credit first (FALSE: BrokenNoAckCredit)
    Control,        \* TRUE: a keep-alive or disconnect spends a token first (FALSE: BrokenNoControl)
    CreditBound,    \* TRUE: only while seq > spent (FALSE: BrokenFreeAcks)
    ServerAckSpends \* TRUE: the server's acks move spent (FALSE: BrokenServerAckCredits)

ASSUME Correct \cap Flooders = {}
ASSUME B \in Nat \ {0} /\ Factor \in Nat \ {0} /\ MaxPkt \in Nat \ {0} /\ T \in Nat \ {0}
ASSUME Kinds \subseteq {"pay", "ack", "ka", "dc"} /\ "pay" \in Kinds

Clients == Correct \cup Flooders
R == Factor * B
Cap == IF CapFull THEN R * T + MaxPkt ELSE R
CtlCap == Factor * T + 2
Keys == IF PerClient THEN Clients ELSE {"shared"}
Key(c) == IF PerClient THEN c ELSE "shared"
Min(a, b) == IF a < b THEN a ELSE b
Dist(a, b) == IF a > b THEN a - b ELSE b - a

\* the refill rule of rocketnet_server_receive_allowance_take (and of the
\* control tokens in _take_packet), at rate r up to cap
Adds(t, l) == IF Monotone THEN t > l ELSE t # l
Refilled(a, l, t, r, cap) == IF Refill /\ Adds(t, l) THEN Min(cap, a + r * Dist(t, l)) ELSE a
Moved(l, t) == IF Adds(t, l) THEN t ELSE l

VARIABLES
    now,        \* the clock, in ticks (one tick is the client's send tick)
    srv,        \* the server's time argument
    hi,         \* the highest time argument so far
    backs,      \* steps back taken
    reconnects, \* slot reuses taken
    fl,         \* flooder packets this tick
    sb,         \* each correct client's send bucket
    net,        \* each correct client's packets in flight: <<[n, st, k]>>
    allow,      \* the byte allowance per key
    last,       \* its last refill time
    ctl,        \* the control tokens per key
    ctlLast,    \* their last refill time
    spent,      \* the ack credit mark per key
    seq,        \* the server's session sequences taken per client (its next)
    pend,       \* each correct client's eliciting packets in flight to it
    debt,       \* each correct client's ack debt
    kaAt,       \* one past the tick of each correct client's last keep-alive (0: none)
    bye,        \* each correct client has sent its disconnect
    elicited,   \* eliciting packets the server sent each client, this connection
    credited,   \* each client's acks paid by the ack credit, this connection
    tokens,     \* each client's packets paid by control tokens, this connection
    shadow,     \* each client's allowance computed from its own packets only
    refused,    \* refused packets per client, this connection
    acc,        \* accepted bytes per client, this connection
    start       \* the time argument at which the connection started

vars == << now, srv, hi, backs, reconnects, fl, sb, net, allow, last, ctl, ctlLast, spent, seq,
          pend, debt, kaAt, bye, elicited, credited, tokens, shadow, refused, acc, start >>

TypeOK ==
    /\ now \in 0..Horizon /\ srv \in 0..Horizon /\ hi \in 0..Horizon
    /\ backs \in 0..MaxBack /\ reconnects \in 0..MaxReconnect /\ fl \in 0..FloodPerTick
    /\ sb \in [Correct -> 0..MaxPkt]
    /\ allow \in [Keys -> 0..Cap] /\ last \in [Keys -> 0..Horizon]
    /\ ctl \in [Keys -> 0..CtlCap] /\ ctlLast \in [Keys -> 0..Horizon]
    /\ spent \in [Keys -> Nat] /\ seq \in [Clients -> Nat]
    /\ pend \in [Correct -> Nat] /\ debt \in [Correct -> Nat]
    /\ kaAt \in [Correct -> 0..Horizon + 1] /\ bye \in [Correct -> BOOLEAN]
    /\ elicited \in [Clients -> Nat] /\ credited \in [Clients -> Nat] /\ tokens \in [Clients -> Nat]
    /\ shadow \in [Clients -> [a : 0..Cap, l : 0..Horizon, k : 0..CtlCap, kl : 0..Horizon, s : Nat]]
    /\ refused \in [Clients -> Nat] /\ acc \in [Clients -> Nat] /\ start \in [Clients -> 0..Horizon]

Init ==
    /\ now = 0 /\ srv = 0 /\ hi = 0 /\ backs = 0 /\ reconnects = 0 /\ fl = 0
    /\ sb = [c \in Correct |-> MaxPkt]
    /\ net = [c \in Correct |-> << >>]
    /\ allow = [k \in Keys |-> Cap]
    /\ last = [k \in Keys |-> 0]
    /\ ctl = [k \in Keys |-> CtlCap]
    /\ ctlLast = [k \in Keys |-> 0]
    /\ spent = [k \in Keys |-> 1]
    /\ seq = [c \in Clients |-> 1]
    /\ pend = [c \in Correct |-> 0]
    /\ debt = [c \in Correct |-> 0]
    /\ kaAt = [c \in Correct |-> 0]
    /\ bye = [c \in Correct |-> FALSE]
    /\ elicited = [c \in Clients |-> 0]
    /\ credited = [c \in Clients |-> 0]
    /\ tokens = [c \in Clients |-> 0]
    /\ shadow = [c \in Clients |-> [a |-> Cap, l |-> 0, k |-> CtlCap, kl |-> 0, s |-> 1]]
    /\ refused = [c \in Clients |-> 0]
    /\ acc = [c \in Clients |-> 0]
    /\ start = [c \in Clients |-> 0]

\* which credit pays a packet of kind k, given the credit state (a, ctl tokens
\* after refill, ack mark s) and the sequence counter q of its client
AckPays(k, s, q) == AckCredit /\ k = "ack" /\ (~CreditBound \/ s < q)
CtlPays(k, t) == Control /\ k \in {"ka", "dc"} /\ t >= 1

\* Receive: one authenticated packet of n bytes and kind k from c at the time
\* argument: rocketnet_server_receive_allowance_take_packet
Take(c, n, k) ==
    LET y == Key(c)
        a == Refilled(allow[y], last[y], srv, R, Cap)
        t == Refilled(ctl[y], ctlLast[y], srv, Factor, CtlCap)
        sa == Refilled(shadow[c].a, shadow[c].l, srv, R, Cap)
        st == Refilled(shadow[c].k, shadow[c].kl, srv, Factor, CtlCap)
        ack == AckPays(k, spent[y], seq[c])
        ka == ~ack /\ CtlPays(k, t)
        sack == AckPays(k, shadow[c].s, seq[c])
        ska == ~sack /\ CtlPays(k, st)
    IN  /\ ctlLast' = IF k \in {"ka", "dc"} /\ ~ack THEN [ctlLast EXCEPT ![y] = Moved(ctlLast[y], srv)] ELSE ctlLast
        /\ ctl' = IF ka THEN [ctl EXCEPT ![y] = t - 1]
                  ELSE IF k \in {"ka", "dc"} THEN [ctl EXCEPT ![y] = t]
                  ELSE ctl
        /\ spent' = IF ack THEN [spent EXCEPT ![y] = spent[y] + 1] ELSE spent
        /\ credited' = IF ack THEN [credited EXCEPT ![c] = credited[c] + 1] ELSE credited
        /\ tokens' = IF ka THEN [tokens EXCEPT ![c] = tokens[c] + 1] ELSE tokens
        /\ shadow' = [shadow EXCEPT ![c] =
               [a |-> IF ~sack /\ ~ska THEN (IF n <= sa THEN sa - n ELSE sa) ELSE shadow[c].a,
                l |-> IF ~sack /\ ~ska THEN Moved(shadow[c].l, srv) ELSE shadow[c].l,
                k |-> IF ska THEN st - 1 ELSE IF k \in {"ka", "dc"} /\ ~sack THEN st ELSE shadow[c].k,
                kl |-> IF k \in {"ka", "dc"} /\ ~sack THEN Moved(shadow[c].kl, srv) ELSE shadow[c].kl,
                s |-> IF sack THEN shadow[c].s + 1 ELSE shadow[c].s]]
        /\ IF ack \/ ka
           THEN /\ UNCHANGED << allow, last, refused, acc >>
           ELSE /\ last' = [last EXCEPT ![y] = Moved(last[y], srv)]
                /\ IF n <= a
                   THEN /\ allow' = [allow EXCEPT ![y] = a - n]
                        /\ acc' = [acc EXCEPT ![c] = acc[c] + n]
                        /\ UNCHANGED refused
                   ELSE /\ allow' = [allow EXCEPT ![y] = a]
                        /\ refused' = [refused EXCEPT ![c] = refused[c] + 1]
                        /\ UNCHANGED acc

\* the clock moves only when no packet in flight would pass its delivery bound
Tick ==
    /\ now < Horizon
    /\ \A c \in Correct : \A i \in 1..Len(net[c]) : now + 1 - net[c][i].st <= T - 1
    /\ now' = now + 1 /\ srv' = now + 1 /\ hi' = now + 1 /\ fl' = 0
    /\ sb' = [c \in Correct |-> Min(MaxPkt, sb[c] + B)]
    /\ UNCHANGED << backs, reconnects, net, allow, last, ctl, ctlLast, spent, seq, pend, debt, kaAt, bye,
                    elicited, credited, tokens, shadow, refused, acc, start >>

StepBack ==
    /\ backs < MaxBack /\ srv > 0
    /\ srv' = srv - 1 /\ backs' = backs + 1
    /\ UNCHANGED << now, hi, reconnects, fl, sb, net, allow, last, ctl, ctlLast, spent, seq, pend, debt,
                    kaAt, bye, elicited, credited, tokens, shadow, refused, acc, start >>

\* the server sends c an eliciting packet (keep-alive, payload, disconnect):
\* one session sequence; a correct client receives it or loses it later
Elicit(c) ==
    /\ seq[c] - 1 < MaxElicit
    /\ seq' = [seq EXCEPT ![c] = seq[c] + 1]
    /\ elicited' = [elicited EXCEPT ![c] = elicited[c] + 1]
    /\ pend' = IF c \in Correct THEN [pend EXCEPT ![c] = pend[c] + 1] ELSE pend
    /\ UNCHANGED << now, srv, hi, backs, reconnects, fl, sb, net, allow, last, ctl, ctlLast, spent, debt,
                    kaAt, bye, credited, tokens, shadow, refused, acc, start >>

\* the server's own standalone ack: a sequence that elicits nothing
\* (rocketnet_server_protocol_write_ack_if_due)
ServerAck(c) ==
    /\ seq[c] - 1 < MaxElicit
    /\ seq' = [seq EXCEPT ![c] = seq[c] + 1]
    /\ spent' = IF ServerAckSpends THEN [spent EXCEPT ![Key(c)] = spent[Key(c)] + 1] ELSE spent
    /\ shadow' = IF ServerAckSpends THEN [shadow EXCEPT ![c].s = shadow[c].s + 1] ELSE shadow
    /\ UNCHANGED << now, srv, hi, backs, reconnects, fl, sb, net, allow, last, ctl, ctlLast, pend, debt,
                    kaAt, bye, elicited, credited, tokens, refused, acc, start >>

\* a correct client receives (or loses) one eliciting packet: its debt grows
Receive(c, got) ==
    /\ pend[c] > 0
    /\ pend' = [pend EXCEPT ![c] = pend[c] - 1]
    /\ debt' = IF got THEN [debt EXCEPT ![c] = debt[c] + 1] ELSE debt
    /\ UNCHANGED << now, srv, hi, backs, reconnects, fl, sb, net, allow, last, ctl, ctlLast, spent, seq,
                    kaAt, bye, elicited, credited, tokens, shadow, refused, acc, start >>

\* a correct client's send of kind k and n bytes: every send pays its debt
CanSend(c) == ~bye[c] /\ Len(net[c]) < MaxNet
Sent(c, n, k) ==
    /\ net' = [net EXCEPT ![c] = Append(net[c], [n |-> n, st |-> now, k |-> k])]
    /\ debt' = [debt EXCEPT ![c] = 0]

Send(c, n) ==
    /\ CanSend(c) /\ n <= sb[c]
    /\ sb' = [sb EXCEPT ![c] = sb[c] - n]
    /\ Sent(c, n, "pay")
    /\ UNCHANGED << now, srv, hi, backs, reconnects, fl, allow, last, ctl, ctlLast, spent, seq, pend,
                    kaAt, bye, elicited, credited, tokens, shadow, refused, acc, start >>

SendAck(c) ==
    /\ "ack" \in Kinds /\ CanSend(c) /\ debt[c] >= 1
    /\ Sent(c, MaxPkt, "ack")
    /\ UNCHANGED << now, srv, hi, backs, reconnects, fl, sb, allow, last, ctl, ctlLast, spent, seq, pend,
                    kaAt, bye, elicited, credited, tokens, shadow, refused, acc, start >>

SendKeepAlive(c) ==
    /\ "ka" \in Kinds /\ CanSend(c) /\ kaAt[c] # now + 1
    /\ kaAt' = [kaAt EXCEPT ![c] = now + 1]
    /\ Sent(c, MaxPkt, "ka")
    /\ UNCHANGED << now, srv, hi, backs, reconnects, fl, sb, allow, last, ctl, ctlLast, spent, seq, pend,
                    bye, elicited, credited, tokens, shadow, refused, acc, start >>

SendDisconnect(c) ==
    /\ "dc" \in Kinds /\ CanSend(c)
    /\ bye' = [bye EXCEPT ![c] = TRUE]
    /\ Sent(c, MaxPkt, "dc")
    /\ UNCHANGED << now, srv, hi, backs, reconnects, fl, sb, allow, last, ctl, ctlLast, spent, seq, pend,
                    kaAt, elicited, credited, tokens, shadow, refused, acc, start >>

Deliver(c) ==
    /\ net[c] # << >>
    /\ Take(c, Head(net[c]).n, Head(net[c]).k)
    /\ net' = [net EXCEPT ![c] = Tail(net[c])]
    /\ UNCHANGED << now, srv, hi, backs, reconnects, fl, sb, seq, pend, debt, kaAt, bye, elicited, start >>

Flood(c, n, k) ==
    /\ fl < FloodPerTick
    /\ Take(c, n, k)
    /\ fl' = fl + 1
    /\ UNCHANGED << now, srv, hi, backs, reconnects, sb, net, seq, pend, debt, kaAt, bye, elicited, start >>

\* the connection ends and a new one takes the slot: the slot's allowance is
\* zeroed by the reset and started full at the connection's start
\* (rocketnet_server_receive_allowance_start), its ack mark at the
\* connection's next sequence; the old connection's packets in flight no
\* longer open
Reconnect(c) ==
    /\ PerClient /\ reconnects < MaxReconnect
    /\ reconnects' = reconnects + 1
    /\ allow' = [allow EXCEPT ![c] = Cap]
    /\ last' = [last EXCEPT ![c] = srv]
    /\ ctl' = [ctl EXCEPT ![c] = CtlCap]
    /\ ctlLast' = [ctlLast EXCEPT ![c] = srv]
    /\ spent' = [spent EXCEPT ![c] = seq[c]]
    /\ shadow' = [shadow EXCEPT ![c] = [a |-> Cap, l |-> srv, k |-> CtlCap, kl |-> srv, s |-> seq[c]]]
    /\ refused' = [refused EXCEPT ![c] = 0]
    /\ acc' = [acc EXCEPT ![c] = 0]
    /\ elicited' = [elicited EXCEPT ![c] = 0]
    /\ credited' = [credited EXCEPT ![c] = 0]
    /\ tokens' = [tokens EXCEPT ![c] = 0]
    /\ start' = [start EXCEPT ![c] = srv]
    /\ sb' = IF c \in Correct THEN [sb EXCEPT ![c] = MaxPkt] ELSE sb
    /\ net' = IF c \in Correct THEN [net EXCEPT ![c] = << >>] ELSE net
    /\ pend' = IF c \in Correct THEN [pend EXCEPT ![c] = 0] ELSE pend
    /\ debt' = IF c \in Correct THEN [debt EXCEPT ![c] = 0] ELSE debt
    /\ kaAt' = IF c \in Correct THEN [kaAt EXCEPT ![c] = 0] ELSE kaAt
    /\ bye' = IF c \in Correct THEN [bye EXCEPT ![c] = FALSE] ELSE bye
    /\ UNCHANGED << now, srv, hi, backs, fl, seq >>

\* the run ends at the horizon: a terminal stutter
Done == now = Horizon /\ UNCHANGED vars

Next ==
    \/ Tick
    \/ Done
    \/ StepBack
    \/ \E c \in Clients : Elicit(c) \/ ServerAck(c)
    \/ \E c \in Correct, got \in BOOLEAN : Receive(c, got)
    \/ \E c \in Correct, n \in 1..MaxPkt : Send(c, n)
    \/ \E c \in Correct : SendAck(c) \/ SendKeepAlive(c) \/ SendDisconnect(c)
    \/ \E c \in Correct : Deliver(c)
    \/ \E c \in Flooders, n \in 1..MaxPkt, k \in Kinds : Flood(c, n, k)
    \/ \E c \in Clients : Reconnect(c)

Spec == Init /\ [][Next]_vars

\* a client within its budget, delivered up to T-1 late, is never refused:
\* not its payload, not its acks of what the server sent, not its
\* keep-alives, not its disconnect
UnderRateNeverRefused == \A c \in Correct : refused[c] = 0

\* a client's allowance is what its own packets made it
Isolated == \A c \in Correct :
    /\ allow[Key(c)] = shadow[c].a
    /\ ctl[Key(c)] = shadow[c].k
    /\ spent[Key(c)] = shadow[c].s

\* nothing is accepted beyond the capacity and the rate times the time seen:
\* in bytes, and in control tokens
NoMint == \A c \in Clients :
    /\ acc[c] <= Cap + R * (hi - start[c])
    /\ tokens[c] <= CtlCap + Factor * (hi - start[c])

\* the ack credit pays for no more standalone acks than the eliciting packets
\* the server sent that client this connection
AcksBounded == \A c \in Clients : credited[c] <= elicited[c]

=============================================================================
