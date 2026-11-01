---------------------------- MODULE MCRNPacketLease ----------------------------
(***************************************************************************)
(* Small RNPacketLease instances: a four-entry table with used limit two. *)
(* Frames and scheduled operations vary by cfg; UsedOffByOne uses three   *)
(* frames. README-RNPacketLease.md lists each configuration and the       *)
(* check of its trace against the code.                                  *)
(***************************************************************************)
EXTENDS RNPacketLease
=============================================================================
