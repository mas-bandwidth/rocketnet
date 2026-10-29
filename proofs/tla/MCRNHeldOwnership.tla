------------------------- MODULE MCRNHeldOwnership --------------------------
(***************************************************************************)
(* The small instances of RNHeldOwnership: W = 2 descriptor slots, a held  *)
(* index of 4 cells, four held keys with every home function, blocks of    *)
(* two fragments, numbers 0 to 8.  MCRNHeldOwnership.cfg is the fixed code *)
(* with every home function (pass).  With one cluster (ZeroHome), to stay  *)
(* inside the run budget: MCRNHeldOwnershipGuards.cfg keeps the number-only *)
(* release with the bound and the identity check (pass on HeldBound,      *)
(* EraseEnds, NoStaleDelivery); MCRNHeldOwnershipGuardErase.cfg keeps only *)
(* the bounded erase (pass on EraseEnds with the index full).             *)
(* Reversed witnesses: ...BrokenNumberOnlyRelease.cfg (ReleaseOwned),      *)
(* ...BrokenNoGuardsErase.cfg (EraseEnds) and ...BrokenNoGuardsStale.cfg   *)
(* (NoStaleDelivery).                                                      *)
(***************************************************************************)
EXTENDS RNHeldOwnership

\* every hash function, clusters wrapping the array end included
AllHomes == [Keys -> 0..(2 * W - 1)]

\* one cluster: every key's home is cell 0, so the index fills as one run
ZeroHome == { [ k \in Keys |-> 0 ] }
=============================================================================
