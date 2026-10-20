-------------------------- MODULE RNServerSendFair --------------------------
(***************************************************************************)
(* The server's send pass under a frame shortage, as                       *)
(* sdk/rocketnet_message_server.cpp builds it: rn_message_object_start's   *)
(* in-place starts (InPlace), then rn_message_object_send_packets, whose    *)
(* first round offers one frame per connection in rotation order from      *)
(* send_first and whose second round takes the rest, and the transport's   *)
(* per-update capacity of K frames (payload_ring_capacity in               *)
(* sdk/rocketnet_server_transport.cpp).                                     *)
(*                                                                         *)
(* One step is one update. Connections 0..N-1 are connected throughout;    *)
(* any of them may find a send waiting at the start of an update           *)
(* (arrive), and a waiting connection keeps waiting until it is served,    *)
(* that is, until it is handed at least one frame. The application's       *)
(* in-place starts take frames in its own call order, modelled as index    *)
(* order, the worst case for rotation. A connection served in place may    *)
(* still have work for the walk (more). In the second round any served     *)
(* connection may be the first refused (second): an over-approximation    *)
(* of which pass is still open.                                            *)
(*                                                                         *)
(* age[c] counts the consecutive updates c waited without a frame. With    *)
(* no in-place starts (InPlaceStarts = FALSE) every waiting connection is  *)
(* served within ceil(N / K) updates (FairWalk). With them, the update in  *)
(* which they first run out of frames is spent in call order, and c is     *)
(* served within ceil(N / K) + 1 updates (Fair); after that update         *)
(* constrained holds them back and the walk alone hands out frames.        *)
(* Two constants flip one rule each for the reversed witnesses:            *)
(*   RotateStart = FALSE       the walk starts at connection 0 every       *)
(*                             update (the base before this model);        *)
(*   InPlaceWhenConstrained = TRUE  in-place starts keep taking frames     *)
(*                             in call order under a shortage.             *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    N,                          \* connections
    K,                          \* frames per update
    InPlaceStarts,              \* the application sends unreliable messages, which start in place
    RotateStart,                \* TRUE in the code
    InPlaceWhenConstrained      \* FALSE in the code

ASSUME N \in Nat /\ N > 0 /\ K \in Nat /\ K > 0

Conns == 0..(N - 1)
NONE == N

CeilDiv(a, b) == (a + b - 1) \div b

\* the updates a waiting connection may go without a frame
Bound == CeilDiv(N, K)

VARIABLES first, constrained, pending, age

vars == <<first, constrained, pending, age>>

\* position of c in the walk that starts at f
Rank(c, f) == (c - f + N) % N
Ahead(S, f, c) == {d \in S : Rank(d, f) < Rank(c, f)}
\* the first r of S in walk order from f, and the one after them
Take(S, f, r) == {c \in S : Cardinality(Ahead(S, f, c)) < r}
Next(S, f, r) == {c \in S : Cardinality(Ahead(S, f, c)) = r}

TypeOK ==
    /\ first \in Conns
    /\ constrained \in BOOLEAN
    /\ pending \subseteq Conns
    /\ age \in [Conns -> 0..(Bound + 1)]

Init ==
    /\ first = 0
    /\ constrained = FALSE
    /\ pending = {}
    /\ age = [c \in Conns |-> 0]

\* the in-place choices, none when the application makes no in-place starts
InPlaceSets == IF InPlaceStarts THEN SUBSET Conns ELSE {{}}

Update ==
    \E arrive \in SUBSET Conns, inplace \in InPlaceSets, more \in InPlaceSets, second \in Conns \cup {NONE} :
        LET waiting    == pending \cup arrive
            \* rn_message_object_start: in-place starts while not constrained, in call order
            ip         == IF ~InPlaceStarts \/ ( constrained /\ ~InPlaceWhenConstrained ) THEN {} ELSE inplace \cap waiting
            servedIP   == Take(ip, 0, K)
            ipRefused  == ip \ servedIP # {}
            \* rn_message_object_send_packets, first round: one plan each from start
            start      == IF RotateStart THEN first ELSE 0
            walk       == (waiting \ servedIP) \cup (servedIP \cap more)
            frames     == K - Cardinality(servedIP)
            servedWalk == Take(walk, start, frames)
            refused1   == Next(walk, start, frames)
            served     == servedIP \cup servedWalk
            \* the second round: with no first-round refusal, a served connection may be refused a second frame
            refused2   == IF refused1 = {} /\ second \in servedWalk THEN {second} ELSE {}
        IN
            /\ first' = IF refused1 # {} THEN CHOOSE c \in refused1 : TRUE
                        ELSE IF refused2 # {} THEN second
                        ELSE first
            /\ constrained' = (ipRefused \/ refused1 # {} \/ refused2 # {})
            /\ pending' = waiting \ served
            /\ age' = [c \in Conns |-> IF c \in waiting \ served THEN age[c] + 1 ELSE 0]

Spec == Init /\ [][Update]_vars

\* the walk alone: every waiting connection is handed a frame within Bound updates
FairWalk == \A c \in Conns : age[c] <= Bound - 1

\* with in-place starts: within Bound + 1 updates
Fair == \A c \in Conns : age[c] <= Bound

=============================================================================
