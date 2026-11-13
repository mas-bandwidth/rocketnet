-------------------- MODULE MCRNServerPayloadRingLayout --------------------
(***************************************************************************)
(* The small instances of RNServerPayloadRingLayout: a frame stride of 2   *)
(* (the broken recovery divides by 1) and a chunk of two slots.            *)
(* MCRNServerPayloadRingLayout.cfg runs three game threads on one line at  *)
(* a send capacity of four, the ring exactly the capacity (the code's      *)
(* sizing at ROCKETNET_MAX_CLIENTS clients), the guard for one line: the   *)
(* chunk branch, its clip at the capacity, a lost install and the          *)
(* one-slot branch are all reached.  MCRNServerPayloadRingLayoutSpare.cfg  *)
(* runs three threads on two lines (two on one, one on the other), the     *)
(* guard for two lines, at a capacity of four with a ring one larger      *)
(* (fewer clients).  Each MCRNServerPayloadRingLayoutBroken*.cfg turns one *)
(* rule off and must violate the invariant it names                        *)
(* (README-RNServerPayloadRingLayout.md).                                  *)
(***************************************************************************)
EXTENDS RNServerPayloadRingLayout

\* every thread on line 0, as before the game names a client (line 0 for all)
MCOneLine == [t \in Threads |-> 0]

\* one thread alone on line 1, the others sharing line 0
MCLoner == CHOOSE t \in Threads : TRUE
MCTwoLines == [t \in Threads |-> IF t = MCLoner THEN 1 ELSE 0]

MCSpec == Spec

=============================================================================
