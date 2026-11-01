------------------------- MODULE RNClientSlotsLayout -------------------------
(***************************************************************************)
(* The client slots' free stack and owner generations, as laid out in      *)
(* sdk/rocketnet_client_slots.cpp, with the code's arithmetic.             *)
(*                                                                         *)
(* N slots, each with connected, client_id and slot_generation (the owner  *)
(* generation the slot last held, 0 before its first assign), and the      *)
(* free stack: the array free_stack of N entries and free_stack_count,     *)
(* filled at create highest index first so the first assigns pop 0, 1, 2.  *)
(* rocketnet_client_slots_assign reads the top entry free_stack[count-1],  *)
(* refuses when the slot's last generation equals the owner generation it *)
(* is given (stale ownership, refused before any state changes), then      *)
(* pops it (--count), marks the slot connected under that generation.     *)
(* rocketnet_client_slots_release_exact releases only the exact tenure:    *)
(* the slot connected, its client id and its generation equal to the       *)
(* handle's; it then pushes the index at free_stack[count++].              *)
(* rocketnet_client_slots_release is release_exact of the slot's current   *)
(* identity, after refusing a slot that is not connected.  The full-id     *)
(* index (open addressing) is RNOpenAddress.tla's and is abstracted here   *)
(* to a lookup by client id over the connected slots.                      *)
(*                                                                         *)
(* Owner generations come from the caller (the encryption mapping's        *)
(* generation).  The header's contract: a generation names one ownership   *)
(* instance and differs from the generation the reused slot last held.     *)
(* The environment offers each assign a fresh generation or the top slot's *)
(* last one, which the code must refuse.  Handles are the development      *)
(* ownership witness (rocketnet_client_slots_handle): every tenure's       *)
(* (client id, index, generation) is kept and may be released at any      *)
(* later time.                                                             *)
(*                                                                         *)
(* Reversed witnesses (each flag FALSE is a real bug class):               *)
(*   ReleaseGuard   rocketnet_client_slots_release without its connected   *)
(*                  refusal, and release_exact without its exactness test  *)
(*   PopDecrements  assign reads the top without --free_stack_count        *)
(*   ReleaseGenCheck release_exact without the generation compare          *)
(*   AssignGenCheck assign without the stale-generation refusal            *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    N,              \* max_clients
    Ids,            \* client ids
    MaxGen,         \* fresh generations run 1..MaxGen
    ReleaseGuard,
    PopDecrements,
    ReleaseGenCheck,
    AssignGenCheck

ASSUME N \in Nat \ {0} /\ MaxGen \in Nat \ {0}
ASSUME \A f \in {ReleaseGuard, PopDecrements, ReleaseGenCheck, AssignGenCheck} : f \in BOOLEAN

Slots == 0..(N - 1)
Gens == 0..MaxGen
NOID == 0

VARIABLES
    connected,      \* connected[i]
    cid,            \* client_id[i], 0 when free
    gen,            \* slot_generation[i]
    stack,          \* free_stack[k]
    top,            \* free_stack_count
    numConnected,   \* num_connected
    nextGen,        \* the caller's next fresh generation
    tenure,         \* ghost: tenure[i], the tenure the slot holds (0 for none)
    tenures,        \* ghost: tenures begun so far
    handles,        \* ghost: every tenure's handle [id, idx, gen, ten]
    pastGens,       \* ghost: pastGens[i], the generations slot i has held
    overflow,       \* ghost: a push past the array's end
    wrongRelease,   \* ghost: a handle released a tenure that was not its own
    staleAssign,    \* ghost: an assign took a generation the slot held before
    doubleTake      \* ghost: an assign took a slot that was connected

vars == <<connected, cid, gen, stack, top, numConnected, nextGen, tenure, tenures, handles, pastGens, overflow, wrongRelease, staleAssign, doubleTake>>

Init ==
    /\ connected = [i \in Slots |-> FALSE]
    /\ cid = [i \in Slots |-> NOID]
    /\ gen = [i \in Slots |-> 0]
    /\ stack = [k \in Slots |-> N - 1 - k]
    /\ top = N
    /\ numConnected = 0
    /\ nextGen = 1
    /\ tenure = [i \in Slots |-> 0]
    /\ tenures = 0
    /\ handles = {}
    /\ pastGens = [i \in Slots |-> {}]
    /\ overflow = FALSE
    /\ wrongRelease = FALSE
    /\ staleAssign = FALSE
    /\ doubleTake = FALSE

IdConnected(id) == \E i \in Slots : connected[i] /\ cid[i] = id

\* rocketnet_client_slots_assign: the caller has checked the id is not
\* connected; the top of the free stack, the stale-generation refusal, the
\* pop and the slot written.  g is the caller's generation: fresh, or the
\* top slot's last generation.
Assign(id, g) ==
    /\ ~IdConnected(id)
    /\ tenures < 2 * MaxGen
    /\ top > 0
    /\ LET i == stack[top - 1]
       IN /\ ~(AssignGenCheck /\ gen[i] = g)
          /\ top' = IF PopDecrements THEN top - 1 ELSE top
          /\ connected' = [connected EXCEPT ![i] = TRUE]
          /\ cid' = [cid EXCEPT ![i] = id]
          /\ gen' = [gen EXCEPT ![i] = g]
          /\ numConnected' = numConnected + 1
          /\ tenure' = [tenure EXCEPT ![i] = tenures + 1]
          /\ tenures' = tenures + 1
          /\ handles' = handles \cup {[id |-> id, idx |-> i, gen |-> g, ten |-> tenures + 1]}
          /\ pastGens' = [pastGens EXCEPT ![i] = @ \cup {g}]
          /\ staleAssign' = (staleAssign \/ g \in pastGens[i])
          /\ doubleTake' = (doubleTake \/ connected[i])
    /\ nextGen' = IF g = nextGen THEN nextGen + 1 ELSE nextGen
    /\ UNCHANGED <<stack, overflow, wrongRelease>>

AssignFresh(id) == nextGen <= MaxGen /\ Assign(id, nextGen)
AssignStale(id) == top > 0 /\ gen[stack[top - 1]] # 0 /\ Assign(id, gen[stack[top - 1]])

\* rocketnet_client_slots_release_exact's tests: index, connected, client id, generation
Exact(i, id, g) ==
    IF ReleaseGuard
    THEN connected[i] /\ cid[i] = id /\ (~ReleaseGenCheck \/ gen[i] = g)
    ELSE TRUE

\* the release itself: the slot cleared and its index pushed at free_stack[count++]
ReleaseSteps(i) ==
    /\ connected' = [connected EXCEPT ![i] = FALSE]
    /\ cid' = [cid EXCEPT ![i] = NOID]
    /\ stack' = IF top < N THEN [stack EXCEPT ![top] = i] ELSE stack
    /\ overflow' = (overflow \/ top >= N)
    /\ top' = IF top < N THEN top + 1 ELSE top
    /\ numConnected' = numConnected - 1
    /\ tenure' = [tenure EXCEPT ![i] = 0]
    /\ UNCHANGED <<gen, nextGen, tenures, handles, pastGens, staleAssign, doubleTake>>

\* rocketnet_client_slots_release_handle: a kept handle, at any later time
ReleaseHandle(h) ==
    /\ Exact(h.idx, h.id, h.gen)
    /\ ReleaseSteps(h.idx)
    /\ wrongRelease' = (wrongRelease \/ tenure[h.idx] # h.ten)

\* rocketnet_client_slots_release: the slot's current identity; a slot not
\* connected is refused before anything is touched
Release(i) ==
    /\ ~ReleaseGuard \/ connected[i]
    /\ Exact(i, cid[i], gen[i])
    /\ ReleaseSteps(i)
    /\ UNCHANGED wrongRelease

Next ==
    \/ \E id \in Ids : AssignFresh(id) \/ AssignStale(id)
    \/ \E h \in handles : ReleaseHandle(h)
    \/ \E i \in Slots : Release(i)

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------

TypeOK ==
    /\ connected \in [Slots -> BOOLEAN]
    /\ cid \in [Slots -> Ids \cup {NOID}]
    /\ gen \in [Slots -> Gens]
    /\ stack \in [Slots -> Slots]
    /\ top \in 0..N
    /\ numConnected \in Int
    /\ nextGen \in 1..(MaxGen + 1)

Stack == {stack[k] : k \in 0..(top - 1)}

\* the stack holds each free slot exactly once and nothing else; no push past its end
StackExact ==
    /\ ~overflow
    /\ \A j, k \in 0..(top - 1) : j # k => stack[j] # stack[k]
    /\ Stack = {i \in Slots : ~connected[i]}
    /\ top + numConnected = N

\* one client per slot: no assign takes a slot a client holds
OneClientPerSlot == ~doubleTake

\* num_connected counts the connected slots
ConnectedCount == numConnected = Cardinality({i \in Slots : connected[i]})

\* a handle releases only its own tenure
HandleReleasesOwnTenure == ~wrongRelease

\* a slot never holds the same owner generation twice
TenureGenerationsDistinct == ~staleAssign

\* a connected slot's generation is nonzero
ConnectedGenerationPositive == \A i \in Slots : connected[i] => gen[i] # 0

=============================================================================
