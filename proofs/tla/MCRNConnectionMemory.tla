----------------------- MODULE MCRNConnectionMemory -----------------------
(***************************************************************************)
(* The small instance of RNConnectionMemory: three slots carved from one   *)
(* block, four clients, a region of three records.  The cfg of the same    *)
(* name is the committed run with both rules on; the two Broken cfgs are   *)
(* the reversed witnesses (README-RNConnectionMemory.md).                  *)
(***************************************************************************)
EXTENDS RNConnectionMemory
=============================================================================
