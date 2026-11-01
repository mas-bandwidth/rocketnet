----------------------- MODULE MCRNServerReplaySlots -----------------------
(***************************************************************************)
(* The small instance of RNServerReplaySlots: two client slots, two        *)
(* addresses and highest_opened 0..1, so a slot is reused at the other     *)
(* address, an address moves to the other slot, and any forget or mark the *)
(* sink is asked for may be refused.  MCRNServerReplaySlots.cfg is the     *)
(* passing control; each MCRNServerReplaySlotsBroken*.cfg turns one rule   *)
(* off and must violate the invariant it names                             *)
(* (README-RNServerReplaySlots.md).                                        *)
(***************************************************************************)
EXTENDS RNServerReplaySlots

CONSTANTS s1, s2, a1, a2

MCSpec == Spec

=============================================================================
