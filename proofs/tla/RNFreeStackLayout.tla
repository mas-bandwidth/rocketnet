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
(*   TakeSlots(g) the guard order of rocketnet_client_slots_assign (sdk/   *)
(*                rocketnet_client_slots.cpp): peeks free[count - 1],      *)
(*                refuses a slot whose generation is already the owner's g *)
(*                before any state changes, then pops.  The client-slots   *)
(*                stack is this layout only while nothing is held: its     *)
(*                release parks the index above the top until publish, and *)
(*                its pop moves the last held entry down.  That stack is   *)
(*                RNClientSlotsLayout.tla's (Assign, ReleaseHandle,        *)
(*                Release, Publish); here only the take's guard order is   *)
(*                kept for it.                                             *)
(*   PeekClaim    rocketnet_used_token_history_claim_record                *)
(*                (sdk/rocketnet_token_tables.cpp): rn_token_claim_select_ *)
(*                slot peeks free[free_count - 1] and remembers it; with   *)
(*                no free slot it selects the oldest claim whose deadline  *)
(*                has passed, a live slot (any live slot here: the model   *)
(*                keeps no deadlines or insertion order).                  *)
(*   PopClaim     the record pops the slot only when free_count > 0 and   *)
(*                it is the top; otherwise (the expired live slot) it     *)
(*                retires it and inserts it again, so it stays live and    *)
(*                the stack is untouched.  Both steps are in the one call, *)
(*                so no other table operation runs between them.  The     *)
(*                bindings' rn_token_free_pop is TakeMap's pop without     *)
(*                the generation.                                          *)
(*   Release(i)   rocketnet_encryption_map_remove_generation and the token *)
(*                tables' release with rn_token_free_push: only a live     *)
(*                slot, matched by its generation, is pushed at            *)
(*                free[count].  Not rocketnet_client_slots_release, which  *)
(*                holds the index at free[count + held] until publish      *)
(*                (RNClientSlotsLayout.tla); StackIsFree is not a          *)
(*                property of that stack between a release and a publish.  *)
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
(*   "popexpired" the claim pops the stack for the expired live slot too, *)
(*               not only for a slot taken from it (StackIsFree).          *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Cap,        \* the table's capacity
    MaxGen,     \* the generation counter's last value (UINT64_MAX in the code)
    Owners,     \* the owner generations a client-slots take may carry
    Broken      \* "none", or the reversed witness

ASSUME /\ Cap \in Nat \ {0} /\ MaxGen \in Nat \ {0} /\ Owners \subseteq Nat \ {0}
       /\ Broken \in {"none", "noguard", "decfirst", "wrapone", "popexpired"}

Idx == 0..Cap-1

VARIABLES
    free,       \* the stack array: free_indices, free_stack, free_slots
    count,      \* num_free, free_stack_count, free_count
    live,       \* slot -> live (address type, connected, ticket)
    gen,        \* slot -> its generation (the map's tag, the slot's owner)
    nextGen,    \* map->next_generation; 0 is exhausted
    issued,     \* ghost: every map generation handed out
    pending     \* the slot PeekClaim selected, or NoPeek between claims

vars == <<free, count, live, gen, nextGen, issued, pending>>

NoPeek == -1

\* create: free[capacity - 1 - i] = i
Init ==
    /\ free = [k \in Idx |-> Cap - 1 - k]
    /\ count = Cap
    /\ live = [i \in Idx |-> FALSE]
    /\ gen = [i \in Idx |-> 0]
    /\ nextGen = 1
    /\ issued = {}
    /\ pending = NoPeek

TakeMap ==
    /\ pending = NoPeek
    /\ count > 0 /\ nextGen # 0
    /\ LET i == free[count - 1] IN
       /\ count' = count - 1
       /\ live' = [live EXCEPT ![i] = TRUE]
       /\ gen' = [gen EXCEPT ![i] = nextGen]
       /\ nextGen' = IF nextGen = MaxGen THEN (IF Broken = "wrapone" THEN 1 ELSE 0) ELSE nextGen + 1
       /\ issued' = issued \cup {nextGen}
       /\ UNCHANGED <<free, pending>>

TakeSlots(g) ==
    /\ pending = NoPeek
    /\ count > 0
    /\ LET i == free[count - 1] IN
       IF gen[i] = g
         THEN \* refused: nothing changes, unless the pop came first
              /\ count' = IF Broken = "decfirst" THEN count - 1 ELSE count
              /\ UNCHANGED <<free, live, gen, nextGen, issued, pending>>
         ELSE /\ count' = count - 1
              /\ live' = [live EXCEPT ![i] = TRUE]
              /\ gen' = [gen EXCEPT ![i] = g]
              /\ UNCHANGED <<free, nextGen, issued, pending>>

\* rn_token_claim_select_slot: the top while free_count > 0, else the oldest
\* expired claim, a live slot. PopClaim is the rest of the same call.
PeekClaim ==
    /\ pending = NoPeek
    /\ IF count > 0
       THEN pending' = free[count - 1]
       ELSE \E i \in Idx : live[i] /\ pending' = i
    /\ UNCHANGED <<free, count, live, gen, nextGen, issued>>

\* The code's test: free_count > 0 and the slot is the top, then --free_count;
\* else rn_token_slot_retire, and the insert below leaves the slot live. The
\* "popexpired" witness pops in the else arm too.
PopClaim ==
    /\ pending \in Idx
    /\ IF (count > 0 /\ free[count - 1] = pending) \/ Broken = "popexpired"
       THEN /\ count' = count - 1
            /\ live' = [live EXCEPT ![pending] = TRUE]
       ELSE /\ count' = count
            /\ live' = [live EXCEPT ![pending] = TRUE]
    /\ pending' = NoPeek
    /\ UNCHANGED <<free, gen, nextGen, issued>>

Release(i) ==
    /\ pending = NoPeek
    /\ live[i] \/ (Broken = "noguard" /\ count < Cap)
    /\ live' = [live EXCEPT ![i] = FALSE]
    /\ free' = [free EXCEPT ![count] = i]
    /\ count' = count + 1
    /\ UNCHANGED <<gen, nextGen, issued, pending>>

Next ==
    \/ TakeMap
    \/ \E g \in Owners : TakeSlots(g)
    \/ PeekClaim
    \/ PopClaim
    \/ \E i \in Idx : Release(i)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ free \in [Idx -> Idx]
    /\ count \in 0..Cap
    /\ live \in [Idx -> BOOLEAN]
    /\ nextGen \in 0..MaxGen
    /\ pending \in Idx \cup {NoPeek}

\* the stack below count holds every slot that is not live, each once
StackIsFree ==
    /\ {free[k] : k \in 0..count-1} = {i \in Idx : ~live[i]}
    /\ count = Cardinality({i \in Idx : ~live[i]})

\* a map generation is never handed out twice, and never 0
GenerationNeverReissued ==
    /\ 0 \notin issued
    /\ nextGen # 0 => nextGen \notin issued

=============================================================================
