------------------------- MODULE RNReceiveAllowance -------------------------
(***************************************************************************)
(* rocketnet: the server's per-client receive allowance (the code is      *)
(* rocketnet_server_receive_allowance_take and its callers in             *)
(* sdk/rocketnet_server_protocol.cpp; the setting is                      *)
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
(* A correct client sends through its own send budget: a bucket of depth  *)
(* MaxPkt refilled at B per tick. Its packets reach the server up to T-1  *)
(* ticks late (an outage shorter than the timeout) and may arrive all at  *)
(* once. A flooder sends anything, FloodPerTick packets a tick. The       *)
(* server's time argument follows the clock but may step back            *)
(* (MaxBack times), and a connection may end and the slot be reused       *)
(* (MaxReconnect times).                                                  *)
(*                                                                         *)
(* The control traffic a client sends outside its budget (keep-alives,    *)
(* standalone acks) is not modelled: it is what the second B of R is      *)
(* there for, and README-RNReceiveAllowance.md records where it can       *)
(* exceed it.                                                             *)
(*                                                                         *)
(* Four constants flip the guarding rules, one per reversed witness:      *)
(*   Refill = FALSE     the allowance never refills: a client under 1x is *)
(*                      refused. Fails UnderRateNeverRefused.             *)
(*   CapFull = FALSE    the cap is one tick of R: the burst after a       *)
(*                      silence is refused. Fails UnderRateNeverRefused.  *)
(*   PerClient = FALSE  one allowance for every slot: refusing the        *)
(*                      flooder changes the correct client. Fails         *)
(*                      Isolated.                                         *)
(*   Monotone = FALSE   a step back in time refills by its distance:      *)
(*                      time going back mints bytes. Fails NoMint.        *)
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
    Refill,         \* TRUE: refill from time (FALSE: BrokenNoRefill)
    CapFull,        \* TRUE: Cap = R x T + MaxPkt (FALSE: BrokenSmallCap)
    PerClient,      \* TRUE: one allowance per slot (FALSE: BrokenShared)
    Monotone        \* TRUE: only time past the last refill adds (FALSE: BrokenTimeBack)

ASSUME Correct \cap Flooders = {}
ASSUME B \in Nat \ {0} /\ Factor \in Nat \ {0} /\ MaxPkt \in Nat \ {0} /\ T \in Nat \ {0}

Clients == Correct \cup Flooders
R == Factor * B
Cap == IF CapFull THEN R * T + MaxPkt ELSE R
Keys == IF PerClient THEN Clients ELSE {"shared"}
Key(c) == IF PerClient THEN c ELSE "shared"
Min(a, b) == IF a < b THEN a ELSE b
Dist(a, b) == IF a > b THEN a - b ELSE b - a

\* the refill rule of rocketnet_server_receive_allowance_take
Adds(t, l) == IF Monotone THEN t > l ELSE t # l
Refilled(a, l, t) == IF Refill /\ Adds(t, l) THEN Min(Cap, a + R * Dist(t, l)) ELSE a
Moved(l, t) == IF Adds(t, l) THEN t ELSE l

VARIABLES
    now,        \* the clock, in ticks
    srv,        \* the server's time argument
    hi,         \* the highest time argument so far
    backs,      \* steps back taken
    reconnects, \* slot reuses taken
    fl,         \* flooder packets this tick
    sb,         \* each correct client's send bucket
    net,        \* each correct client's packets in flight: <<[n, st]>>
    allow,      \* the allowance per key
    last,       \* its last refill time
    shadow,     \* each client's allowance computed from its own packets only
    refused,    \* refused packets per client, this connection
    acc,        \* accepted bytes per client, this connection
    start       \* the time argument at which the connection started

vars == << now, srv, hi, backs, reconnects, fl, sb, net, allow, last, shadow, refused, acc, start >>

TypeOK ==
    /\ now \in 0..Horizon /\ srv \in 0..Horizon /\ hi \in 0..Horizon
    /\ backs \in 0..MaxBack /\ reconnects \in 0..MaxReconnect /\ fl \in 0..FloodPerTick
    /\ sb \in [Correct -> 0..MaxPkt]
    /\ allow \in [Keys -> 0..Cap] /\ last \in [Keys -> 0..Horizon]
    /\ shadow \in [Clients -> [a : 0..Cap, l : 0..Horizon]]
    /\ refused \in [Clients -> Nat] /\ acc \in [Clients -> Nat] /\ start \in [Clients -> 0..Horizon]

Init ==
    /\ now = 0 /\ srv = 0 /\ hi = 0 /\ backs = 0 /\ reconnects = 0 /\ fl = 0
    /\ sb = [c \in Correct |-> MaxPkt]
    /\ net = [c \in Correct |-> << >>]
    /\ allow = [k \in Keys |-> Cap]
    /\ last = [k \in Keys |-> 0]
    /\ shadow = [c \in Clients |-> [a |-> Cap, l |-> 0]]
    /\ refused = [c \in Clients |-> 0]
    /\ acc = [c \in Clients |-> 0]
    /\ start = [c \in Clients |-> 0]

\* Receive: one authenticated packet of n bytes from c at the time argument
Take(c, n) ==
    LET k == Key(c)
        a == Refilled(allow[k], last[k], srv)
        sa == Refilled(shadow[c].a, shadow[c].l, srv)
    IN  /\ last' = [last EXCEPT ![k] = Moved(last[k], srv)]
        /\ shadow' = [shadow EXCEPT ![c] = [a |-> IF n <= sa THEN sa - n ELSE sa, l |-> Moved(shadow[c].l, srv)]]
        /\ IF n <= a
           THEN /\ allow' = [allow EXCEPT ![k] = a - n]
                /\ acc' = [acc EXCEPT ![c] = acc[c] + n]
                /\ UNCHANGED refused
           ELSE /\ allow' = [allow EXCEPT ![k] = a]
                /\ refused' = [refused EXCEPT ![c] = refused[c] + 1]
                /\ UNCHANGED acc

\* the clock moves only when no packet in flight would pass its delivery bound
Tick ==
    /\ now < Horizon
    /\ \A c \in Correct : \A i \in 1..Len(net[c]) : now + 1 - net[c][i].st <= T - 1
    /\ now' = now + 1 /\ srv' = now + 1 /\ hi' = now + 1 /\ fl' = 0
    /\ sb' = [c \in Correct |-> Min(MaxPkt, sb[c] + B)]
    /\ UNCHANGED << backs, reconnects, net, allow, last, shadow, refused, acc, start >>

StepBack ==
    /\ backs < MaxBack /\ srv > 0
    /\ srv' = srv - 1 /\ backs' = backs + 1
    /\ UNCHANGED << now, hi, reconnects, fl, sb, net, allow, last, shadow, refused, acc, start >>

Send(c, n) ==
    /\ n <= sb[c] /\ Len(net[c]) < MaxNet
    /\ sb' = [sb EXCEPT ![c] = sb[c] - n]
    /\ net' = [net EXCEPT ![c] = Append(net[c], [n |-> n, st |-> now])]
    /\ UNCHANGED << now, srv, hi, backs, reconnects, fl, allow, last, shadow, refused, acc, start >>

Deliver(c) ==
    /\ net[c] # << >>
    /\ Take(c, Head(net[c]).n)
    /\ net' = [net EXCEPT ![c] = Tail(net[c])]
    /\ UNCHANGED << now, srv, hi, backs, reconnects, fl, sb, start >>

Flood(c, n) ==
    /\ fl < FloodPerTick
    /\ Take(c, n)
    /\ fl' = fl + 1
    /\ UNCHANGED << now, srv, hi, backs, reconnects, sb, net, start >>

\* the connection ends and a new one takes the slot: the slot's allowance is
\* zeroed by the reset and filled at the connection's start; the old
\* connection's packets in flight no longer open
Reconnect(c) ==
    /\ PerClient /\ reconnects < MaxReconnect
    /\ reconnects' = reconnects + 1
    /\ allow' = [allow EXCEPT ![c] = Cap]
    /\ last' = [last EXCEPT ![c] = srv]
    /\ shadow' = [shadow EXCEPT ![c] = [a |-> Cap, l |-> srv]]
    /\ refused' = [refused EXCEPT ![c] = 0]
    /\ acc' = [acc EXCEPT ![c] = 0]
    /\ start' = [start EXCEPT ![c] = srv]
    /\ sb' = IF c \in Correct THEN [sb EXCEPT ![c] = MaxPkt] ELSE sb
    /\ net' = IF c \in Correct THEN [net EXCEPT ![c] = << >>] ELSE net
    /\ UNCHANGED << now, srv, hi, backs, fl >>

\* the run ends at the horizon: a terminal stutter
Done == now = Horizon /\ UNCHANGED vars

Next ==
    \/ Tick
    \/ Done
    \/ StepBack
    \/ \E c \in Correct, n \in 1..MaxPkt : Send(c, n)
    \/ \E c \in Correct : Deliver(c)
    \/ \E c \in Flooders, n \in 1..MaxPkt : Flood(c, n)
    \/ \E c \in Clients : Reconnect(c)

Spec == Init /\ [][Next]_vars

\* a client within its budget, delivered up to T-1 late, is never refused
UnderRateNeverRefused == \A c \in Correct : refused[c] = 0

\* a client's allowance is what its own packets made it
Isolated == \A c \in Correct : allow[Key(c)] = shadow[c].a

\* nothing is accepted beyond the capacity and the rate times the time seen
NoMint == \A c \in Clients : acc[c] <= Cap + R * (hi - start[c])

=============================================================================
