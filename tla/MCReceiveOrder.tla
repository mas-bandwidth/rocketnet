-------------------------- MODULE MCReceiveOrder --------------------------
(***************************************************************************)
(* The committed run of ReceiveOrder in the ruled order.  Every invariant  *)
(* passes.  The witnesses are the same module under the Broken cfgs, each  *)
(* moving one cheap rung behind a crypto rung; TLC must reject all three.  *)
(***************************************************************************)
EXTENDS ReceiveOrder

Done == outcome # "pending"

Terminating == Done /\ UNCHANGED vars

MCNext == Next \/ Terminating

MCSpec == Init /\ [][MCNext]_vars

=============================================================================
