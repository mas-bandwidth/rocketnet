------------------------ MODULE MCRNIocpReceiveAdmit ------------------------
(***************************************************************************)
(* The small instances of RNIocpReceiveAdmit.  MCRNIocpReceiveAdmit.cfg:   *)
(* a bank of three slots, seven datagrams of any kind, the filter         *)
(* published; MCRNIocpReceiveAdmitCap4.cfg: four slots, eight datagrams;  *)
(* MCRNIocpReceiveAdmitFilterOff.cfg: before the filter is published,    *)
(* where only the bounds and the counts hold.  Reversed witnesses:        *)
(* MCRNIocpReceiveAdmitBrokenBankFirst.cfg (the capacity check before the *)
(* prefilter, NoEvictionByGarbage), MCRNIocpReceiveAdmitBrokenUncountedFull.cfg *)
(* (a bank-full drop not counted, EveryDropCounted) and                    *)
(* MCRNIocpReceiveAdmitBrokenCheckPastEnd.cfg (num_packets <= capacity,   *)
(* InBounds).                                                             *)
(***************************************************************************)
EXTENDS RNIocpReceiveAdmit
=============================================================================
