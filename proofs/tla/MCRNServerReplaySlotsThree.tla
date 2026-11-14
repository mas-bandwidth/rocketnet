-------------------- MODULE MCRNServerReplaySlotsThree ---------------------
(***************************************************************************)
(* RNServerReplaySlots at three addresses: a slot reused twice while both  *)
(* its record's forget and its skip mark's forget are refused, the case     *)
(* KeepSkipOwner and ZeroMarkWaitsForForgets are about.  Two addresses     *)
(* cannot reach it (MCRNServerReplaySlots.tla).                            *)
(*   MCRNServerReplaySlotsThree.cfg           any sink: every invariant but *)
(*                                             GreetedSlotHasEntry          *)
(*   MCRNServerReplaySlotsThreeXdpSink.cfg     the XDP sink's rule: every   *)
(*                                             invariant                    *)
(*   MCRNServerReplaySlotsBrokenSkipOverwritten.cfg  KernelEntryOwned       *)
(*   MCRNServerReplaySlotsBrokenZeroMarkTaken.cfg    GreetedSlotHasEntry    *)
(*   MCRNServerReplaySlotsBrokenKeptKeyZeroMark.cfg  GreetedSlotHasEntry    *)
(* (README-RNServerReplaySlots.md)                                          *)
(***************************************************************************)
EXTENDS RNServerReplaySlots

CONSTANTS s1, s2, a1, a2, a3

MCSpec == Spec

\* MCRNServerReplaySlotsBrokenSkipOverwritten.cfg: a Blocked slot is marked
\* and its skip mark overwritten (KeepSkipOwner FALSE)
MCKeepSkipOwnerBroken == FALSE

\* MCRNServerReplaySlotsThreeXdpSink.cfg: the XDP sink's zero-mark rule
MCZeroMarkWaits == TRUE

\* MCRNServerReplaySlotsBrokenKeptKeyZeroMark.cfg: the XDP sink's rule with
\* 7307f183e's kept-key exception (KeptKeyZeroMarkWaits FALSE)
MCKeptKeyZeroMarkBroken == FALSE

=============================================================================
