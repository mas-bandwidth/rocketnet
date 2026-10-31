--------------------- MODULE MCRNSimulatorStoreLayout ---------------------
(***************************************************************************)
(* The small instance of RNSimulatorStoreLayout: three slots, two shards,  *)
(* three destinations (0 and 2 share shard 0), times 0..1 and generations  *)
(* modulo 2.  Two passing controls split the word arithmetic:              *)
(* MCRNSimulatorStoreLayout.cfg has two-bit occupancy words and one-bit    *)
(* summary words (two occupancy words, two summary words), and             *)
(* MCRNSimulatorStoreLayoutSummaryBits.cfg one-bit occupancy words and     *)
(* two-bit summary words (three occupancy words, two summary words).       *)
(* Each MCRNSimulatorStoreLayoutBroken*.cfg turns one rule off and must    *)
(* violate the invariant it names (README-RNSimulatorStoreLayout.md).      *)
(***************************************************************************)
EXTENDS RNSimulatorStoreLayout

MCSpec == Spec

=============================================================================
