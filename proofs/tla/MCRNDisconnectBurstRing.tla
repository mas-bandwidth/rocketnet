---------------------- MODULE MCRNDisconnectBurstRing ----------------------
(***************************************************************************)
(* The small instance of RNDisconnectBurstRing: a steady cap of two, so a  *)
(* ring of four slots, bursts of two packets (three in the second control, *)
(* so a pass leaves sealed counts two apart from a waiting one), and eight *)
(* disconnects (seven in the second), so a running server reaches its     *)
(* cap and drops a burst, the ring wraps and refills, the stop fills the   *)
(* ring to four, and a stop step with no send room drops every waiting     *)
(* burst at once (DropForRoom).                                            *)
(* MCRNDisconnectBurstRing.cfg and MCRNDisconnectBurstRingK3.cfg are the   *)
(* passing controls; each MCRNDisconnectBurstRingBroken*.cfg turns one     *)
(* rule off and must violate the invariant it names                        *)
(* (README-RNDisconnectBurstRing.md).                                      *)
(***************************************************************************)
EXTENDS RNDisconnectBurstRing

MCSpec == Spec

=============================================================================
