--------------------------- MODULE MCRNWriterFinish ---------------------------
(***************************************************************************)
(* The small instance of RNWriterFinish: two writers, two clients, an     *)
(* update capacity of two frames, five starts. MCRNWriterFinish.cfg is the *)
(* committed run, with the writer's finish as the code makes it. The      *)
(* reversed witness MCRNWriterFinishBrokenNoOwner.cfg lets a second       *)
(* writer onto a client and must fail (OneWriter).                        *)
(***************************************************************************)
EXTENDS RNWriterFinish
=============================================================================
