-------------------------- MODULE MCReceiveOrder --------------------------
(***************************************************************************)
(* The committed run of ReceiveOrder in the ruled order.  Every invariant  *)
(* passes.  The witnesses are the same module under the Broken cfgs: three *)
(* move one cheap rung behind a crypto rung and TLC must reject them on    *)
(* CheapRefusalsFirst; MCReceiveOrderBrokenTokenManyAddresses removes the  *)
(* token claim (ClaimCheck <- MCClaimCheckOff) and TLC must reject it on   *)
(* OneHalfOpenPerToken.                                                    *)
(***************************************************************************)
EXTENDS ReceiveOrder

MCClaimCheckOff == FALSE

Done == outcome # "pending"

Terminating == Done /\ UNCHANGED vars

MCNext == Next \/ Terminating

MCSpec == Init /\ [][MCNext]_vars

=============================================================================
