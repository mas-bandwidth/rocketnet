--------------------------- MODULE RNFilterEpochs ---------------------------
(***************************************************************************)
(* Historical design from retired STANDARD.md at commit 96da6fb36107,    *)
(* "The keyed filter value"; no v1.0 keyed implementation. It models the  *)
(* live magics held by a keyed receiver, installation and retirement of  *)
(* generations, the oldest retired                                       *)
(* when the table is full, and the verdict on a packet's epoch byte.       *)
(*                                                                         *)
(* The rule:                                                               *)
(*   - generations advance 1, 2, 3 and wrap from the last back to 1; zero  *)
(*     is never a generation;                                              *)
(*   - a receiver accepts at least two live generations, so a rotation     *)
(*     does not refuse a packet sent under the generation before;          *)
(*   - epoch 0 is never keyed, and a keyed receiver refuses it rather than *)
(*     falling back to a constant;                                         *)
(*   - an epoch naming no live generation is refused on a comparison,      *)
(*     before any MAC; a live one costs exactly one MAC, under the magic   *)
(*     that epoch names.                                                   *)
(*                                                                         *)
(* N stands for 255: generations are 1..N with the same wrap rule. Live is *)
(* the table's capacity; Retire runs only when the table is full and       *)
(* Install only when it has room, so a capacity of three is what keeps two *)
(* live across a rotation. A key is named by its install index k (the      *)
(* magic itself is not modelled); genOf[k] is the generation the backend   *)
(* handed with it. Check(e, k) is a packet carrying epoch byte e whose     *)
(* filter value was computed under key k (k = 0: under no key, a forger);  *)
(* the MAC matches exactly when k is the key the receiver holds for e (a   *)
(* forged 32-bit match is outside the model). last is the verdict of the   *)
(* most recent check, with macs the ghost count of MACs it computed; an    *)
(* Install or Retire clears it, so every invariant over last reads the     *)
(* table the check saw.                                                    *)
(*                                                                         *)
(* The code card names its functions after the actions: Install, Retire    *)
(* and Check. Rocketnet 1.0 is unkeyed: rocketnet_prefilter_check          *)
(* (sdk/rocketnet_protocol.cpp) refuses every epoch but 0, so this module  *)
(* is written before the code it covers.                                   *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS N, Live, MaxInstalls

ASSUME /\ N \in Nat /\ N >= 2
       /\ Live \in Nat /\ Live >= 3 /\ Live <= N
       /\ MaxInstalls \in Nat /\ MaxInstalls >= 1

Gens   == 1..N
Epochs == 0..N
Keys   == 1..MaxInstalls

(* Rule switches, overridden by the reversed witnesses' configurations.   *)
(* AcceptZero: epoch 0 falls back to the unkeyed constant (the rule:      *)
(* refused). WrapTo: the generation after N (the rule: 1).                 *)
AcceptZero == FALSE
WrapTo     == 1

FirstGen   == 1
NextGen(g) == IF g = N THEN WrapTo ELSE g + 1
RuleNext(g) == IF g = N THEN 1 ELSE g + 1

VARIABLES live, installs, genOf, last

vars == <<live, installs, genOf, last>>

NoCheck == [e |-> 0, k |-> 0, verdict |-> "none", macs |-> 0]

LiveGens == {live[i].g : i \in DOMAIN live}
LiveKeys == {live[i].k : i \in DOMAIN live}
HeldKey(e) == (CHOOSE i \in DOMAIN live : live[i].g = e)
HeldKeyOf(e) == live[HeldKey(e)].k

(* The keys a legitimate sender can hold: the one it was most recently    *)
(* given, or the one before it while a packet sent under it is in flight. *)
Legit == {k \in 1..installs : k + 1 >= installs}

Init ==
    /\ live = <<>>
    /\ installs = 0
    /\ genOf = [k \in Keys |-> 0]
    /\ last = NoCheck

(* The backend hands generation g, the next of the newest.                *)
Install(g) ==
    /\ installs < MaxInstalls
    /\ Len(live) < Live
    /\ g = IF installs = 0 THEN FirstGen ELSE NextGen(genOf[installs])
    /\ installs' = installs + 1
    /\ live' = Append(live, [g |-> g, k |-> installs + 1])
    /\ genOf' = [genOf EXCEPT ![installs + 1] = g]
    /\ last' = NoCheck

(* The oldest generation leaves the table when the table is full.         *)
Retire ==
    /\ Len(live) = Live
    /\ live' = Tail(live)
    /\ last' = NoCheck
    /\ UNCHANGED <<installs, genOf>>

(* The receiver's verdict on epoch byte e of a packet signed under key k. *)
Check(e, k) ==
    /\ last' =
         IF e = 0
         THEN [e |-> e, k |-> k, macs |-> 0,
               verdict |-> IF AcceptZero THEN "accept" ELSE "refused"]
         ELSE IF e \notin LiveGens
         THEN [e |-> e, k |-> k, macs |-> 0, verdict |-> "refused"]
         ELSE [e |-> e, k |-> k, macs |-> 1,
               verdict |-> IF k = HeldKeyOf(e) THEN "accept" ELSE "refused"]
    /\ UNCHANGED <<live, installs, genOf>>

Next ==
    \/ \E g \in Epochs : Install(g)
    \/ Retire
    \/ \E e \in Epochs, k \in 0..installs : Check(e, k)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ installs \in 0..MaxInstalls
    /\ Len(live) <= Live
    /\ \A i \in DOMAIN live : live[i] \in [g : Epochs, k : Keys]
    /\ genOf \in [Keys -> Epochs]
    /\ last \in [e : Epochs, k : 0..MaxInstalls,
                 verdict : {"none", "accept", "refused"}, macs : 0..1]

(* Epoch 0 is never keyed: a keyed receiver refuses it, with no MAC and   *)
(* no fallback to the constant.                                           *)
NeverKeyedAtZero ==
    last.e = 0 => (last.verdict # "accept" /\ last.macs = 0)

(* Every live generation is in 1..N, and each install is the next of the  *)
(* one before under the rule: N wraps to 1, never to 0.                   *)
WrapSkipsZero ==
    /\ \A i \in DOMAIN live : live[i].g \in Gens
    /\ \A k \in 1..installs : genOf[k] \in Gens
    /\ \A k \in 2..installs : genOf[k] = RuleNext(genOf[k - 1])

(* Once a second generation is installed, at least two are always live.   *)
AtLeastTwoLiveAfterRotation ==
    installs >= 2 => Len(live) >= 2

(* An epoch naming no live generation is refused on the comparison, and   *)
(* no MAC is computed.                                                    *)
DeadEpochRefusedWithoutMac ==
    (last.e \in Gens /\ last.e \notin LiveGens)
        => (last.verdict = "refused" /\ last.macs = 0)

(* A live epoch costs exactly one MAC.                                    *)
OneMacPerLiveEpoch ==
    (last.e \in LiveGens /\ last.e # 0) => last.macs = 1

(* A sender holding the newest key, or the one before it, is never        *)
(* refused: no window of a rotation drops it.                             *)
LegitimateNeverRefused ==
    (last.k \in Legit /\ last.e = genOf[last.k] /\ last.verdict # "none")
        => last.verdict = "accept"

(* A key is accepted only while it is in the table: never once retired,   *)
(* and never because a later key wrapped onto the same epoch.             *)
OldKeyRefused ==
    (last.verdict = "accept" /\ last.e # 0)
        => (last.k \in LiveKeys /\ last.k + Live > installs)

(* Every live generation names one key: the epoch byte is unambiguous.    *)
LiveGenerationsDistinct ==
    Cardinality(LiveGens) = Len(live)

=============================================================================
