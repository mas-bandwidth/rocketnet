---------------------- MODULE MCRNDisconnectBurstRing ----------------------
(***************************************************************************)
(* The small instance of RNDisconnectBurstRing: a ring of three slots,     *)
(* bursts of two packets (three in the second control, so a pass leaves    *)
(* sealed counts two apart from a waiting one), and six disconnects, so    *)
(* the ring fills, drops a burst, wraps and refills.                       *)
(* MCRNDisconnectBurstRing.cfg and MCRNDisconnectBurstRingK3.cfg are the   *)
(* passing controls; each MCRNDisconnectBurstRingBroken*.cfg turns one     *)
(* rule off and must violate the invariant it names                        *)
(* (README-RNDisconnectBurstRing.md).                                      *)
(***************************************************************************)
EXTENDS RNDisconnectBurstRing

MCSpec == Spec

=============================================================================
