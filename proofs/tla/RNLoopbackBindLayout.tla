------------------------ MODULE RNLoopbackBindLayout ------------------------
(***************************************************************************)
(* The loopback hub's bound table and its ephemeral port search, as laid   *)
(* out in sdk/rocketnet_driver_loopback.cpp, with the code's arithmetic.   *)
(*                                                                         *)
(* The hub has R endpoint records: the server at 0, the clients after it.  *)
(* hub->bound[family][port] holds the record index plus one of the live    *)
(* endpoint bound on that family and port, 0 for none, so a datagram's     *)
(* destination is one lookup, not a walk of the records.  loopback_create  *)
(* refuses a bind whose family's entry for its port is nonzero, takes the  *)
(* server's record or the first free client record, and for a bind to      *)
(* port 0 searches from hub->next_port for a port neither family holds,    *)
(* stepping with loopback_next_port, which wraps from the range's last     *)
(* port (65535) to its first (49152); then it writes the entry.            *)
(* loopback_destroy clears the entry only when it names the destroyed      *)
(* record.  loopback_find_destination reads the datagram family's entry    *)
(* (a live endpoint bound to the wildcard or to the exact address), and    *)
(* for IPv4 falls back to the IPv6 entry when that endpoint is a           *)
(* dual-stack IPv6 wildcard.                                               *)
(*                                                                         *)
(* The abstract contract is the walk the table replaces: WalkFind scans    *)
(* the live records.  LookupIsWalk states the table lookup returns the     *)
(* same endpoint for every destination.  Ports run 0..Last; the ephemeral  *)
(* range is First..Last and Last stands for 65535, so the code's uint16    *)
(* port + 1 at Last is (Last + 1) mod (Last + 1) = 0.                      *)
(*                                                                         *)
(* Reversed witnesses (each flag FALSE is a real bug class):               *)
(*   PlusOne        the entry stores the record index without the + 1     *)
(*   BindCheck      create without the refusal of a held family port       *)
(*   WrapToFirst    loopback_next_port as port + 1, wrapping to 0          *)
(*   BothFamilies   the search tests only the bind's own family            *)
(* A destroy that clears the entry without testing that it names the      *)
(* record is not a witness: DestroyFindsOwnEntry shows the test is always  *)
(* true under the code's create, so the unguarded clear is equivalent.     *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Records,        \* 1 + max_clients endpoint records
    First,          \* ROCKETNET_LOOPBACK_FIRST_PORT
    Last,           \* ROCKETNET_LOOPBACK_LAST_PORT, the largest port
    BindPorts,      \* the ports an explicit bind names (0 asks for an ephemeral port)
    PlusOne,        \* the entry is the record index plus one, 0 for none
    BindCheck,      \* loopback_create refuses a family port already held
    WrapToFirst,    \* loopback_next_port wraps Last to First
    BothFamilies    \* the search skips a port either family holds

ASSUME Records \in Nat \ {0}
ASSUME First \in Nat \ {0} /\ Last \in Nat /\ First <= Last
ASSUME Records < Last - First + 1          \* assumed, not enforced by the code: a free ephemeral port exists
ASSUME BindPorts \subseteq 0..Last /\ 0 \in BindPorts
ASSUME \A f \in {PlusOne, BindCheck, WrapToFirst, BothFamilies} : f \in BOOLEAN

Recs == 0..(Records - 1)
Ports == 0..Last
Families == {4, 6}
BindAddrs == {"wild", "a"}          \* a bind's address: the family's wildcard or a specific one
ToAddrs == {"a", "b"}               \* a datagram's destination address
NONE == Records

VARIABLES
    live,           \* live[r]: endpoints[r].live
    fam,            \* fam[r]: endpoints[r].bind.type
    addr,           \* addr[r]: endpoints[r].bind's address
    port,           \* port[r]: endpoints[r].bind.port
    bound,          \* bound[f][p]: hub->bound[f == 6][p]
    nextPort,       \* hub->next_port
    registered,     \* hub->registered
    pickedOut,      \* ghost: a search picked a port outside First..Last
    pickedHeld,     \* ghost: a search picked a port some live endpoint holds
    searchStuck,    \* ghost: a search found no port within its bound
    otherCleared    \* ghost: a destroy met an entry naming another record

vars == <<live, fam, addr, port, bound, nextPort, registered, pickedOut, pickedHeld, searchStuck, otherCleared>>

\* the entry written for record r, and the record an entry names (NONE for none)
Encode(r) == IF PlusOne THEN r + 1 ELSE r
Decode(e) == IF e = 0 THEN NONE ELSE IF PlusOne THEN e - 1 ELSE e

\* loopback_next_port
NextPort(p) == IF WrapToFirst
               THEN IF p = Last THEN First ELSE p + 1
               ELSE (p + 1) % (Last + 1)

Held(f, p) == bound[f][p] # 0
PortFree(f, p) == IF BothFamilies THEN ~Held(4, p) /\ ~Held(6, p) ELSE ~Held(f, p)

\* the while loop of loopback_create from p, at most Last + 1 steps
RECURSIVE SearchFrom(_, _, _)
SearchFrom(f, p, fuel) ==
    IF fuel = 0 THEN -1
    ELSE IF PortFree(f, p) THEN p
    ELSE SearchFrom(f, NextPort(p), fuel - 1)

\* the record create takes: the server's, or the first free client record
FreeRecord(server) ==
    LET cands == IF server THEN {0} \cap {r \in Recs : ~live[r]}
                 ELSE {r \in 1..(Records - 1) : ~live[r]}
    IN IF cands = {} THEN NONE
       ELSE CHOOSE r \in cands : \A s \in cands : r <= s

HeldByAny(p) == \E r \in Recs : live[r] /\ port[r] = p

Init ==
    /\ live = [r \in Recs |-> FALSE]
    /\ fam = [r \in Recs |-> 4]
    /\ addr = [r \in Recs |-> "wild"]
    /\ port = [r \in Recs |-> 0]
    /\ bound = [f \in Families |-> [p \in Ports |-> 0]]
    /\ nextPort = First
    /\ registered = 0
    /\ pickedOut = FALSE
    /\ pickedHeld = FALSE
    /\ searchStuck = FALSE
    /\ otherCleared = FALSE

\* loopback_create: the held-port refusal, the record, the port search for a
\* bind to port 0, then the record and its entry written
Create(server, f, a, bp) ==
    /\ ~(BindCheck /\ Held(f, bp))
    /\ LET r == FreeRecord(server)
           found == SearchFrom(f, nextPort, Last + 1)
           p == IF bp = 0 THEN found ELSE bp
       IN /\ r # NONE
          /\ IF bp = 0 /\ found = -1
             THEN /\ searchStuck' = TRUE
                  /\ UNCHANGED <<live, fam, addr, port, bound, nextPort, registered, pickedOut, pickedHeld>>
             ELSE /\ live' = [live EXCEPT ![r] = TRUE]
                  /\ fam' = [fam EXCEPT ![r] = f]
                  /\ addr' = [addr EXCEPT ![r] = a]
                  /\ port' = [port EXCEPT ![r] = p]
                  /\ bound' = [bound EXCEPT ![f][p] = Encode(r)]
                  /\ nextPort' = IF bp = 0 THEN NextPort(p) ELSE nextPort
                  /\ registered' = registered + 1
                  /\ pickedOut' = (pickedOut \/ (bp = 0 /\ (p < First \/ p > Last)))
                  /\ pickedHeld' = (pickedHeld \/ (bp = 0 /\ HeldByAny(p)))
                  /\ UNCHANGED searchStuck
    /\ UNCHANGED otherCleared

\* loopback_destroy: the entry cleared only when it names this record
Destroy(r) ==
    /\ live[r]
    /\ LET e == bound[fam[r]][port[r]]
       IN /\ bound' = IF e = Encode(r)
                      THEN [bound EXCEPT ![fam[r]][port[r]] = 0]
                      ELSE bound
          /\ otherCleared' = (otherCleared \/ e # Encode(r))
    /\ live' = [live EXCEPT ![r] = FALSE]
    /\ fam' = [fam EXCEPT ![r] = 4]
    /\ addr' = [addr EXCEPT ![r] = "wild"]
    /\ port' = [port EXCEPT ![r] = 0]
    /\ registered' = registered - 1
    /\ UNCHANGED <<nextPort, pickedOut, pickedHeld, searchStuck>>

Next ==
    \/ \E server \in BOOLEAN, f \in Families, a \in BindAddrs, bp \in BindPorts : Create(server, f, a, bp)
    \/ \E r \in Recs : Destroy(r)

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------

\* loopback_find_destination, read off the table
Accepts(r, a) == live[r] /\ (addr[r] = "wild" \/ addr[r] = a)
TableFind(f, a, p) ==
    LET same == Decode(bound[f][p])
        dual == Decode(bound[6][p])
    IN IF same # NONE /\ Accepts(same, a) THEN same
       ELSE IF f = 4 /\ dual # NONE /\ live[dual] /\ addr[dual] = "wild" THEN dual
       ELSE NONE

\* the same answer by a walk of the records
WalkFind(f, a, p) ==
    LET sameSet == {r \in Recs : live[r] /\ fam[r] = f /\ port[r] = p /\ Accepts(r, a)}
        dualSet == {r \in Recs : live[r] /\ fam[r] = 6 /\ port[r] = p /\ addr[r] = "wild"}
    IN IF sameSet # {} THEN CHOOSE r \in sameSet : TRUE
       ELSE IF f = 4 /\ dualSet # {} THEN CHOOSE r \in dualSet : TRUE
       ELSE NONE

TypeOK ==
    /\ live \in [Recs -> BOOLEAN]
    /\ fam \in [Recs -> Families]
    /\ addr \in [Recs -> BindAddrs]
    /\ port \in [Recs -> Ports]
    /\ bound \in [Families -> [Ports -> 0..Records]]
    /\ nextPort \in Ports
    /\ registered \in 0..Records

\* one live endpoint per family and port
OneEndpointPerFamilyPort ==
    \A r, s \in Recs : r # s /\ live[r] /\ live[s] => fam[r] # fam[s] \/ port[r] # port[s]

\* an entry is nonzero exactly when a live endpoint holds its family port, and names it
BoundExact ==
    \A f \in Families, p \in Ports :
        LET holders == {r \in Recs : live[r] /\ fam[r] = f /\ port[r] = p}
        IN IF holders = {} THEN bound[f][p] = 0
           ELSE bound[f][p] # 0 /\ Decode(bound[f][p]) \in holders

\* the one-lookup destination is the walk's
LookupIsWalk ==
    \A f \in Families, a \in ToAddrs, p \in 1..Last : TableFind(f, a, p) = WalkFind(f, a, p)

\* an ephemeral port is in the range, free in both families when picked, and always found
EphemeralInRange == ~pickedOut
EphemeralPortFree == ~pickedHeld
SearchEnds == ~searchStuck

\* no live endpoint holds port 0, and next_port stays in the range
NoPortZero == \A r \in Recs : live[r] => port[r] # 0
NextPortInRange == nextPort \in First..Last

\* destroy's test always finds its own entry
DestroyFindsOwnEntry == ~otherCleared

RegisteredCount == registered = Cardinality({r \in Recs : live[r]})

=============================================================================
