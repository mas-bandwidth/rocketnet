------------------------- MODULE MCRNServerSendFair -------------------------
(***************************************************************************)
(* The small instance of RNServerSendFair: three clients, one frame per   *)
(* update. MCRNServerSendFair.cfg is the committed run; the Walk config    *)
(* widens the same fair rule to five clients and two frames. The two       *)
(* Broken configs restore the reversed witnesses and must fail.            *)
(***************************************************************************)
EXTENDS RNServerSendFair
=============================================================================
