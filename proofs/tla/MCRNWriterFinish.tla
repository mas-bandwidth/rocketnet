--------------------------- MODULE MCRNWriterFinish ---------------------------
(***************************************************************************)
(* The small instance of RNWriterFinish: two writers, two clients, an     *)
(* update capacity of two frames, five starts. MCRNWriterFinish.cfg is the *)
(* committed run, with the writer's finish as the code makes it. The      *)
(* reversed witnesses each restore one defect and must fail:              *)
(* MCRNWriterFinishBrokenNoSeal.cfg leaves the packet open for the walk   *)
(* (NothingLeftForWalk), MCRNWriterFinishBrokenQueueNoCapacity.cfg queues *)
(* a start with no capacity (NothingLeftForWalk), and                     *)
(* MCRNWriterFinishBrokenNoOwner.cfg lets a second writer onto a client   *)
(* (OneWriter).                                                           *)
(***************************************************************************)
EXTENDS RNWriterFinish
=============================================================================
