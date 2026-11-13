---------------------------- MODULE LoopbackHub ----------------------------
(***************************************************************************)
(* The registration lifecycle of the loopback hub in                      *)
(* sdk/rocketnet_driver_loopback.cpp.  The application creates one hub    *)
(* (rocketnet_loopback_hub_create) holding one server record, index 0,    *)
(* and MaxClients client records after it.  Server start registers a      *)
(* server in record 0; client connect registers a client in the first     *)
(* free record after it; a full hub, or a second server, is refused.      *)
(* Stop, disconnect and destroy unregister the endpoint and leave its     *)
(* record for the next one.  rocketnet_loopback_hub_destroy refuses, and  *)
(* keeps the hub, while any endpoint is registered; otherwise it frees    *)
(* the hub.                                                                *)
(*                                                                         *)
(* Destroy is two steps in the code, and here: it reads registered under  *)
(* the hub mutex and releases it (the check), then destroys the mutex,    *)
(* zeroes the header and frees the block outside it (the free).  Between  *)
(* them the hub is "retiring": its header is still intact, so a create on *)
(* another thread passes the header check, takes the mutex and registers  *)
(* into a hub about to be freed.  That is caller misuse (rocketnet.h:     *)
(* destroy the hub after every server and client that uses it is          *)
(* destroyed), and the code does not catch it.  CallerSerializesDestroy   *)
(* is that contract: TRUE, no create runs while a destroy is in progress; *)
(* FALSE lets one in, and MCLoopbackHubBrokenDestroyRace.cfg shows the     *)
(* freed hub with an endpoint registered.  The roadmap item               *)
(* loopback-hub-destroy-check-and-free is the code fix.                   *)
(*                                                                         *)
(* Each endpoint is built into a copy of the library.  The endpoints in   *)
(* Foreign come from another build: the hub's magic, version and layout   *)
(* id differ from theirs, and every use they make of it is refused.      *)
(* Destroy is called from a copy of either build.                         *)
(*                                                                         *)
(* Two switches are the fixes and one is the caller's contract; the       *)
(* reversed witnesses turn one off:                                       *)
(*   DestroyChecksRegistered  destroy refuses while an endpoint is        *)
(*                            registered (NoEndpointOutlivesHub)         *)
(*   UseChecksHeader          every use checks the header (NoForeignUse) *)
(*   CallerSerializesDestroy  no create between destroy's check and its   *)
(*                            free (NoEndpointOutlivesHub)               *)
(*                                                                         *)
(* The code cites RegisterServer, RegisterClient, Unregister, Create and  *)
(* Destroy where they happen.  Done is the terminal stutter after the     *)
(* hub is freed.                                                           *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    MaxClients,                 \* the hub's client records, fixed at create
    Servers,                    \* server objects that may try to register
    Clients,                    \* client objects that may try to register
    Foreign,                    \* endpoints built by another build of the library
    NoEndpoint,                 \* a free record's holder: no endpoint
    DestroyChecksRegistered,    \* the fix: destroy refuses while any endpoint is registered
    UseChecksHeader,            \* the fix: every use checks magic, version and layout
    CallerSerializesDestroy     \* the caller's contract: no create while a destroy runs

VARIABLES
    hub,            \* "none" before create, "live", "retiring" between destroy's
                    \* check and its free, "freed" after destroy
    record,         \* record index -> the endpoint registered there, or NoEndpoint
    at,             \* endpoint -> its record index, or -1 when not registered
    registered,     \* the hub's own count of registered endpoints
    lastDestroy,    \* the last destroy's result: "none", "ok", "in_use" or "mismatch"
    foreignUse      \* ghost: an endpoint of another build registered with the hub

vars == <<hub, record, at, registered, lastDestroy, foreignUse>>

Endpoints == Servers \cup Clients
Records == 0..MaxClients

ASSUME /\ MaxClients \in Nat \ {0}
       /\ Servers \cap Clients = {}
       /\ Foreign \subseteq Endpoints
       /\ NoEndpoint \notin Endpoints

Init ==
    /\ hub = "none"
    /\ record = [ r \in Records |-> NoEndpoint ]
    /\ at = [ e \in Endpoints |-> -1 ]
    /\ registered = 0
    /\ lastDestroy = "none"
    /\ foreignUse = FALSE

\* the header check at every use: a hub made by another build is refused.
\* A retiring hub's header is intact, so the check passes; only the
\* caller's contract keeps a create out of that window.
Usable(e) ==
    /\ \/ hub = "live"
       \/ hub = "retiring" /\ ~CallerSerializesDestroy
    /\ (e \in Foreign => ~UseChecksHeader)

Register(e, r) ==
    /\ record' = [ record EXCEPT ![r] = e ]
    /\ at' = [ at EXCEPT ![e] = r ]
    /\ registered' = registered + 1
    /\ foreignUse' = (foreignUse \/ e \in Foreign)
    /\ UNCHANGED <<hub, lastDestroy>>

\* rocketnet_loopback_hub_create: one block, every record free
Create ==
    /\ hub = "none"
    /\ hub' = "live"
    /\ UNCHANGED <<record, at, registered, lastDestroy, foreignUse>>

\* server start: record 0, or refused when another server holds it
RegisterServer(s) ==
    /\ at[s] = -1
    /\ Usable(s)
    /\ record[0] = NoEndpoint
    /\ Register(s, 0)

\* client connect: the first free record after the server's; a full hub refuses
RegisterClient(c) ==
    /\ at[c] = -1
    /\ Usable(c)
    /\ \E r \in 1..MaxClients :
         /\ record[r] = NoEndpoint
         /\ \A q \in 1..r-1 : record[q] # NoEndpoint
         /\ Register(c, r)

\* stop, disconnect or destroy: the record stays in the hub for the next endpoint
Unregister(e) ==
    /\ at[e] # -1
    /\ record' = [ record EXCEPT ![at[e]] = NoEndpoint ]
    /\ at' = [ at EXCEPT ![e] = -1 ]
    /\ registered' = registered - 1
    /\ UNCHANGED <<hub, lastDestroy, foreignUse>>

\* rocketnet_loopback_hub_destroy from a copy of this build (foreign = FALSE)
\* or of another build (foreign = TRUE), in the code's two steps.  The
\* check: a mismatch, or registered read under the mutex above zero, refuses
\* and keeps the hub; otherwise the mutex is released and the hub retires.
\* The free: the mutex destroyed, the header zeroed, the block freed,
\* whatever registered later.  The free is the same call's second step, so
\* it takes the build of the call that passed the check (either value of
\* foreign enables it; the hub state alone gates it).
Destroy(foreign) ==
    /\ \/ /\ hub = "live"
          /\ IF foreign /\ UseChecksHeader
               THEN /\ lastDestroy' = "mismatch"
                    /\ UNCHANGED hub
               ELSE IF registered > 0 /\ DestroyChecksRegistered
                 THEN /\ lastDestroy' = "in_use"
                      /\ UNCHANGED hub
                 ELSE /\ lastDestroy' = "ok"
                      /\ hub' = "retiring"
       \/ /\ hub = "retiring"
          /\ hub' = "freed"
          /\ UNCHANGED lastDestroy
    /\ UNCHANGED <<record, at, registered, foreignUse>>

Done ==
    /\ hub = "freed"
    /\ UNCHANGED vars

Next ==
    \/ Create
    \/ \E s \in Servers : RegisterServer(s)
    \/ \E c \in Clients : RegisterClient(c)
    \/ \E e \in Endpoints : Unregister(e)
    \/ \E foreign \in BOOLEAN : Destroy(foreign)
    \/ Done

Spec == Init /\ [][Next]_vars

RegisteredSet == { e \in Endpoints : at[e] # -1 }

TypeOK ==
    /\ hub \in {"none", "live", "retiring", "freed"}
    /\ record \in [ Records -> Endpoints \cup {NoEndpoint} ]
    /\ at \in [ Endpoints -> Records \cup {-1} ]
    /\ registered \in 0..MaxClients+1
    /\ lastDestroy \in {"none", "ok", "in_use", "mismatch"}
    /\ foreignUse \in BOOLEAN

\* the record table and the endpoints agree, one endpoint per record
RecordsMatch ==
    /\ \A e \in Endpoints : at[e] # -1 => record[at[e]] = e
    /\ \A r \in Records : record[r] # NoEndpoint => at[record[r]] = r

\* the server is in record 0 and only a server is
ServerAtZero ==
    /\ \A s \in Servers : at[s] \in {-1, 0}
    /\ \A c \in Clients : at[c] # 0

\* the hub's count is the number of registered endpoints
CountIsRegistered ==
    registered = Cardinality(RegisteredSet)

\* never more clients than the hub holds
WithinCapacity ==
    Cardinality({ c \in Clients : at[c] # -1 }) <= MaxClients

\* no endpoint is registered with a hub that does not exist: destroy never
\* frees a hub an endpoint still reads
NoEndpointOutlivesHub ==
    hub \in {"none", "freed"} => RegisteredSet = {}

\* no endpoint of another build ever registers with the hub
NoForeignUse ==
    ~foreignUse

\* a refused destroy keeps the hub
RefusedKeepsHub ==
    lastDestroy \in {"in_use", "mismatch"} => hub = "live"

=============================================================================
