---------------------- MODULE MCRNSourceSnapshotLayout ----------------------
(***************************************************************************)
(* The small instances of RNSourceSnapshotLayout at the code's slot size:  *)
(* MaxEntries 2, so Cap 4 (rocketnet_source_snapshot_capacity); three      *)
(* addresses, two of them homed on the last cell so the second's probe     *)
(* wraps to cell 0, and the third homed on cell 0 so it probes past the    *)
(* wrapped one; a third distinct address added to a full slot is refused.  *)
(* MCRNSourceSnapshotLayout.cfg: two readers, three updates.               *)
(* MCRNSourceSnapshotLayoutOneReader.cfg: one reader, four updates.        *)
(* Reversed witnesses: MCRNSourceSnapshotLayoutBrokenHalfPublished.cfg     *)
(* (publish before the last add, NoTornRead),                              *)
(* MCRNSourceSnapshotLayoutBrokenNoRecheck.cfg (announce with no re-check, *)
(* WriterNeverWritesAnnounced) and                                         *)
(* MCRNSourceSnapshotLayoutBrokenLookupNoWrap.cfg (the lookup stops at the *)
(* slot's end, LookupMatchesPublished).                                    *)
(***************************************************************************)
EXTENDS RNSourceSnapshotLayout

MCHome(a) == CASE a = 1 -> 3 [] a = 2 -> 3 [] OTHER -> 0

=============================================================================
