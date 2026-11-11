------------------------- MODULE RNServerSendFair -------------------------
(***************************************************************************)
(* The server's per-update send rotation (sdk/rocketnet_message_server.cpp, *)
(* rn_message_object_send_packets): the transport offers K payload frames   *)
(* per update (send_capacity, sdk/rocketnet_server_transport.cpp), and the  *)
(* send pass starts at the connection after the last one served, so the     *)
(* clients served first rotate across updates. The rule this model states   *)
(* is bounded service: with N connected clients and a cap of K frames per   *)
(* update, every client is served within ceil(N / K) updates.               *)
(*                                                                         *)
(* Tick serves the next K clients in cyclic order and moves the cursor     *)
(* after the last of them. Mode = "fromzero" is the reversed witness: the  *)
(* pass starts at index 1 every update, so with K < N the low indices      *)
(* consume the capacity and a higher index starves. Mode = "inplace" is    *)
(* the second reversed witness: the first client keeps the cursor and      *)
(* consumes the whole cap, so every later client starves.                  *)
(*                                                                         *)
(* Per-client scope (MESSAGES.md m1.8, revision 1.1): Dark is a set of     *)
(* clients whose path is dark or that never acknowledge, so they always    *)
(* have resends due.  The rule gives each connection one frame per round   *)
(* (rn_message_object_send_packets), so a dark client takes no more of the *)
(* cap than any other (OtherClientUnaffected).  Mode = "darkhogs" is the   *)
(* reversed witness: a dark client with resends due takes the whole cap   *)
(* each update, a per-client allowance the one-frame-per-round rule rules *)
(* out.                                                                    *)
(*                                                                         *)
(* The model covers only the service order; it does not model queueing,    *)
(* retransmission or the payload content. A client is served when a frame  *)
(* is started for it, and a full ring defers that client's sends to the    *)
(* next update rather than dropping its reliability state.                 *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS N,          \* connected clients, 1..N
          K,          \* payload frames the transport starts per update
          Mode,       \* "fair", "fromzero", "inplace" or "darkhogs"
          MaxTicks,   \* the bounded exploration's last update
          Dark        \* the clients that never acknowledge: resends always due

ASSUME N \in Nat \ {0}
ASSUME K \in 1..N
ASSUME Mode \in {"fair", "fromzero", "inplace", "darkhogs"}
ASSUME MaxTicks \in Nat \ {0}
ASSUME Dark \subseteq 1..N
ASSUME Mode = "darkhogs" => Dark # {}

Clients == 1..N

Window == (N + K - 1) \div K

VARIABLES now,       \* the update number, from 0
          cursor,    \* the first client the next fair pass offers a frame
          last       \* the last update each client was served, 0 before the first

vars == <<now, cursor, last>>

Constraint == now <= MaxTicks

First == IF Mode = "fromzero" THEN 1 ELSE cursor

Count == IF Mode = "inplace" THEN 1 ELSE K

Served == IF Mode = "darkhogs"
          THEN { CHOOSE d \in Dark : TRUE }
          ELSE { ((First + j - 1) % N) + 1 : j \in 0..Count-1 }

NextCursor ==
    IF Mode = "fair"
    THEN ((First + K - 1) % N) + 1
    ELSE First

Init ==
    /\ now = 0
    /\ cursor = 1
    /\ last = [ c \in Clients |-> 0 ]

\* one update: the pass serves Served, then the cursor names the next first
Tick ==
    /\ now < MaxTicks
    /\ now' = now + 1
    /\ last' = [ c \in Clients |-> IF c \in Served THEN now' ELSE last[c] ]
    /\ cursor' = NextCursor

Next == Tick \/ UNCHANGED vars

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ now \in 0..MaxTicks
    /\ cursor \in Clients
    /\ last \in [ Clients -> 0..MaxTicks ]

\* under a cap of K frames per update and N clients, every client is served
\* within ceil(N / K) updates
FairService ==
    \A c \in Clients : now - last[c] < Window

\* a client that is dark or never acknowledges leaves every other client's
\* service as it was: each is still served within ceil(N / K) updates
OtherClientUnaffected ==
    \A c \in Clients \ Dark : now - last[c] < Window

=============================================================================
