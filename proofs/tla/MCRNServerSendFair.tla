------------------------- MODULE MCRNServerSendFair -------------------------
(***************************************************************************)
(* The small instance of RNServerSendFair: three clients, one frame per   *)
(* update. MCRNServerSendFair.cfg is the committed run; the Walk config    *)
(* widens the same fair rule to five clients and two frames. The Dark      *)
(* config has a second client that never acknowledges (m1.8). The three    *)
(* Broken configs restore the reversed witnesses and must fail.  The      *)
(* Rounds configs check revision 1.2, one flush's rounds at the code's     *)
(* index arithmetic; their three Broken configs must fail.               *)
(***************************************************************************)
EXTENDS RNServerSendFair
=============================================================================
