----------------------------- MODULE RNLazyPool -----------------------------
(***************************************************************************)
(* A pool of N fixed-size entries with a LIFO free stack, initialized      *)
(* lazily (sdk/rocketnet_allocator.cpp: the send descriptor pool,          *)
(* rn_region_descriptor_take and rn_region_carve, whose free stack lives   *)
(* in the descriptors' free_slot array). The send store's chunk classes    *)
(* share only this model's lazy watermark discipline (fresh, the in-use    *)
(* guard and untouched-above-fresh below); their take scans the meta       *)
(* array's live bits from a cursor for the lowest free chunk, and both     *)
(* pools are modelled at the code's layout in RNSendStoreLayout.tla.       *)
(*                                                                         *)
(* An eager pool threads its free list through every entry at reset, so    *)
(* the reset writes every entry and touches every page of the pool.  The   *)
(* lazy pool keeps a watermark, fresh: entries 1..fresh have been taken    *)
(* since the reset and are free-listed or held; entries above it were      *)
(* never taken, and nothing writes or reads them.  Take pops the free list *)
(* or, when it is empty, takes entry fresh + 1.  Reset empties the list    *)
(* and zeroes the watermark, leaving modeled entry fields (mem) unchanged.*)
(* SDK carve also clears enqueue_ticket and schedule_flags below the old  *)
(* descriptor watermark before lowering it; those fields are not modeled.*)
(*                                                                         *)
(* The model runs both pools on the same actions.  eager is the reference: *)
(* the list an eager reset threads, <<1, ..., N>>, popped and pushed the   *)
(* same way.  The lazy pool refines it when its list, followed by the      *)
(* untaken entries in order, is the eager list (Refines): the same entry   *)
(* is taken at every step.  An entry is in use exactly when it is held;    *)
(* the lazy query reads an entry's in-use mark only below the watermark    *)
(* (InUseExact), because above it the mark is a previous use's stale byte. *)
(* No entry above the watermark is written (UntouchedAboveFresh).          *)
(*                                                                         *)
(* GuardFresh = FALSE makes the in-use query read the mark of any entry    *)
(* (a full scan to N, or a check against N instead of fresh);              *)
(* ResetFresh = FALSE makes reset keep the watermark.  Each must fail:     *)
(* those are the reversed witnesses.                                       *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS N,            \* the pool's entries
          Owners,       \* who may hold one entry each
          GuardFresh,   \* the in-use query consults the watermark
          ResetFresh    \* reset zeroes the watermark

ASSUME N \in Nat \ {0} /\ Owners # {}
ASSUME GuardFresh \in BOOLEAN /\ ResetFresh \in BOOLEAN

Ids == 1..N
NONE == 0

VARIABLES free,     \* the lazy free stack, head first (entry i of the stack lives in slot i)
          fresh,    \* the lazy watermark
          eager,    \* the reference: the eager free list, head first
          held,     \* held[o]: the entry o holds, NONE when none
          mem,      \* mem[i]: entry i's in-use mark as its bytes hold it ("garbage" before any write)
          gen,      \* gen[i]: entry i's generation tag (1 once a take has tagged it)
          touched   \* the entries written since the last reset

vars == <<free, fresh, eager, held, mem, gen, touched>>

Range(seq) == { seq[k] : k \in DOMAIN seq }
Held == { held[o] : o \in Owners } \ {NONE}
Untaken == [ k \in 1..(N - fresh) |-> fresh + k ]     \* <<fresh+1, ..., N>>

Init ==
    /\ free = <<>>
    /\ fresh = 0
    /\ eager = [ k \in 1..N |-> k ]
    /\ held = [ o \in Owners |-> NONE ]
    /\ mem = [ i \in Ids |-> "garbage" ]
    /\ gen = [ i \in Ids |-> 0 ]
    /\ touched = {}

\* rn_region_descriptor_take: the free list's head, or
\* the first entry never taken; the taker writes the entry whole
Take(o) ==
    /\ held[o] = NONE
    /\ Len(free) > 0 \/ fresh < N
    /\ LET i == IF Len(free) > 0 THEN Head(free) ELSE fresh + 1
       IN /\ free' = IF Len(free) > 0 THEN Tail(free) ELSE free
          /\ fresh' = IF Len(free) > 0 THEN fresh ELSE fresh + 1
          /\ held' = [ held EXCEPT ![o] = i ]
          /\ mem' = [ mem EXCEPT ![i] = "held" ]
          /\ gen' = [ gen EXCEPT ![i] = 1 ]  \* the take tags the slot (the code's counter, bounded for the model)
          /\ touched' = touched \cup {i}
          \* the eager pool takes its head on the same step
          /\ eager' = Tail(eager)

\* rn_region_send_free (rn_region_descriptor_release): the entry is marked free
\* and pushed on both lists
Release(o) ==
    /\ held[o] # NONE
    /\ LET i == held[o]
       IN /\ free' = <<i>> \o free
          /\ eager' = <<i>> \o eager
          /\ held' = [ held EXCEPT ![o] = NONE ]
          /\ mem' = [ mem EXCEPT ![i] = "free" ]
          /\ touched' = touched \cup {i}
          /\ UNCHANGED <<fresh, gen>>

\* rn_region_carve: every holder lets go (the
\* region's connection ended), the lazy list empties and the watermark goes
\* to zero, writing no entry; the eager pool threads every entry again
Reset ==
    /\ free' = <<>>
    /\ fresh' = IF ResetFresh THEN 0 ELSE fresh
    /\ eager' = [ k \in 1..N |-> k ]
    /\ held' = [ o \in Owners |-> NONE ]
    /\ touched' = {}
    /\ UNCHANGED <<mem, gen>>

Next == \/ \E o \in Owners : Take(o) \/ Release(o)
        \/ Reset

Spec == Init /\ [][Next]_vars

\* the lazy in-use query (rn_region_send_data, rn_region_send_free and
\* rn_region_send_free_fragment refuse a descriptor at or past fresh before
\* reading it)
InUse(i) == IF GuardFresh THEN i <= fresh /\ mem[i] = "held" ELSE mem[i] = "held"

TypeOK ==
    /\ fresh \in 0..N
    /\ Range(free) \subseteq Ids /\ Range(eager) \subseteq Ids
    /\ held \in [ Owners -> Ids \cup {NONE} ]
    /\ mem \in [ Ids -> {"garbage", "free", "held"} ]
    /\ gen \in [ Ids -> 0..1 ]
    /\ touched \subseteq Ids

\* the lazy pool takes the same entry as the eager one at every step
Refines == eager = free \o Untaken

\* every entry taken since the reset is free-listed or held, once
Conservation ==
    /\ Range(free) \cap Held = {}
    /\ Range(free) \cup Held = 1..fresh
    /\ Len(free) + Cardinality(Held) = fresh
    /\ \A o1, o2 \in Owners : o1 # o2 /\ held[o1] # NONE => held[o1] # held[o2]

InUseExact == \A i \in Ids : InUse(i) <=> i \in Held

UntouchedAboveFresh == touched \subseteq 1..fresh

\* a take is refused only when every entry is held
TakeOnlyRefusedWhenFull == ( Len(free) = 0 /\ fresh = N ) => Cardinality(Held) = N

\* a held entry carries a generation its last take bumped: never zero
HeldGenerationPositive == \A i \in Held : gen[i] > 0

=============================================================================
