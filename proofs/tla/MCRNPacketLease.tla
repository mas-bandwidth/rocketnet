---------------------------- MODULE MCRNPacketLease ----------------------------
(***************************************************************************)
(* The small instance of RNPacketLease: a table of four entries (the     *)
(* limit of used is two), two threads, two frames that hash to the *)
(* same entry, two values and two operations of thread 1 and one of thread 2.  README-         *)
(* RNPacketLease.md lists the configs and the check of each trace against *)
(* the code.                                                               *)
(***************************************************************************)
EXTENDS RNPacketLease
=============================================================================
