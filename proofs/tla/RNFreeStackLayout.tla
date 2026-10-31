-------------------------- MODULE RNFreeStackLayout --------------------------
(***************************************************************************)
(* The dense free stacks of the transport protocol's tables at the code's  *)
(* layout: an array of Cap slot indices and a count, filled at create so   *)
(* the first pops return 0, 1, 2, ..., popped at free[count - 1] and       *)
(* pushed at free[count].  Three tables keep one each, with three take     *)
(* shapes over the same layout:                                            *)
(*                                                                         *)
(*   TakeMap      rocketnet_encryption_map_add (sdk/rocketnet_encryption_  *)
(*                map.cpp): refused when full, num_free = 0 or the next    *)
(*                generation exhausted (0); pops free[--num_free] and tags *)
(*                the slot with the next generation, which steps to 0      *)
(*                after its maximum (MaxGen here, UINT64_MAX in the code). *)
(*   TakeSlots(g) rocketnet_client_slots_assign (sdk/rocketnet_client_     *)
(*                slots.cpp): peeks free[count - 1], refuses a slot whose  *)
(*                generation is already the owner's g before any state     *)
(*                changes, then pops.                                      *)
(*   TakeClaim    rocketnet_used_token_history_claim_record                *)
(*                (sdk/rocketnet_token_tables.cpp): rn_token_claim_select_ *)
(*                slot peeks free[free_count - 1]; the record pops it only *)
(*                while it is still the top.  The bindings' rn_token_free_ *)
(*                pop is TakeMap's pop without the generation.             *)
(*   Release(i)   rocketnet_encryption_map_remove_generation, rocketnet_   *)
(*                client_slots_release and the token tables' release with  *)
(*                rn_token_free_push: only a live slot, matched by its     *)
(*                generation, is pushed.                                   *)
(*                                                                         *)
(* The open-addressed indexes beside these stacks are RNOpenAddress.tla's; *)
(* here the stack and the live flags are the subject.                      *)
(*                                                                         *)
(* Broken selects a reversed witness:                                      *)
(*   "noguard"   a release of a slot that is not live is pushed anyway     *)
(*               (StackIsFree);                                            *)
(*   "decfirst"  the client-slots take pops before the generation test,   *)
(*               so a refused take loses the slot (StackIsFree);           *)
(*   "wrapone"   the generation steps from its maximum to 1, not to the    *)
(*               exhausted 0 (GenerationNeverReissued).                    *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Cap,        \* the table's capacity
    MaxGen,     \* the generation counter's last value (UINT64_MAX in the code)
    Owners,     \* the owner generations a client-slots take may carry
    Broken      \* "none", or the reversed witness

ASSUME /\ Cap \in Nat \ {0} /\ MaxGen \in Nat \ {0} /\ Owners \subseteq Nat \ {0}
       /\ Broken \in {"none", "noguard", "decfirst", "wrapone"}

Idx == 0..Cap-1

VARIABLES
    free,       \* the stack array: free_indices, free_stack, free_slots
    count,      \* num_free, free_stack_count, free_count
    live,       \* slot -> live (address type, connected, ticket)
    gen,        \* slot -> its generation (the map's tag, the slot's owner)
    nextGen,    \* map->next_generation; 0 is exhausted
    issued      \* ghost: every map generation handed out

vars == <<free, count, live, gen, nextGen, issued>>

\* create: free[capacity - 1 - i] = i
Init ==
    /\ free = [k \in Idx |-> Cap - 1 - k]
    /\ count = Cap
    /\ live = [i \in Idx |-> FALSE]
    /\ gen = [i \in Idx |-> 0]
    /\ nextGen = 1
    /\ issued = {}

TakeMap ==
    /\ count > 0 /\ nextGen # 0
    /\ LET i == free[count - 1] IN
       /\ count' = count - 1
       /\ live' = [live EXCEPT ![i] = TRUE]
       /\ gen' = [gen EXCEPT ![i] = nextGen]
       /\ nextGen' = IF nextGen = MaxGen THEN (IF Broken = "wrapone" THEN 1 ELSE 0) ELSE nextGen + 1
       /\ issued' = issued \cup {nextGen}
       /\ UNCHANGED free

TakeSlots(g) ==
    /\ count > 0
    /\ LET i == free[count - 1] IN
       IF gen[i] = g
         THEN \* refused: nothing changes, unless the pop came first
              /\ count' = IF Broken = "decfirst" THEN count - 1 ELSE count
              /\ UNCHANGED <<free, live, gen, nextGen, issued>>
         ELSE /\ count' = count - 1
              /\ live' = [live EXCEPT ![i] = TRUE]
              /\ gen' = [gen EXCEPT ![i] = g]
              /\ UNCHANGED <<free, nextGen, issued>>

TakeClaim ==
    /\ count > 0
    /\ LET slot == free[count - 1] IN
       /\ count' = IF count > 0 /\ free[count - 1] = slot THEN count - 1 ELSE count
       /\ live' = [live EXCEPT ![slot] = TRUE]
       /\ UNCHANGED <<free, gen, nextGen, issued>>

Release(i) ==
    /\ live[i] \/ (Broken = "noguard" /\ count < Cap)
    /\ live' = [live EXCEPT ![i] = FALSE]
    /\ free' = [free EXCEPT ![count] = i]
    /\ count' = count + 1
    /\ UNCHANGED <<gen, nextGen, issued>>

Next ==
    \/ TakeMap
    \/ \E g \in Owners : TakeSlots(g)
    \/ TakeClaim
    \/ \E i \in Idx : Release(i)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ free \in [Idx -> Idx]
    /\ count \in 0..Cap
    /\ live \in [Idx -> BOOLEAN]
    /\ nextGen \in 0..MaxGen

\* the stack below count holds every slot that is not live, each once
StackIsFree ==
    /\ {free[k] : k \in 0..count-1} = {i \in Idx : ~live[i]}
    /\ count = Cardinality({i \in Idx : ~live[i]})

\* a map generation is never handed out twice, and never 0
GenerationNeverReissued ==
    /\ 0 \notin issued
    /\ nextGen # 0 => nextGen \notin issued

=============================================================================
