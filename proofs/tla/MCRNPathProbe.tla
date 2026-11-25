---------------------------- MODULE MCRNPathProbe ----------------------------
(***************************************************************************)
(* The small instances of RNPathProbe: sizes 1 .. 3 (3 the ceiling), the   *)
(* three-step client list << 3, 2, 1 >>, every path capacity 0 .. 3 each  *)
(* way chosen at Init (so one run covers every path), two sends a window,  *)
(* one loss and one attacker send.  MCRNPathProbe.cfg is the passing       *)
(* control; MCRNPathProbeServerList.cfg gives the server a list without   *)
(* the client's middle size, MCRNPathProbeOneEntry.cfg is the default     *)
(* one-entry list (no probing) and MCRNPathProbeLate.cfg lets a window     *)
(* expire with its packets in flight (safety only).  Each                  *)
(* MCRNPathProbeBroken*.cfg must violate the property it names.            *)
(***************************************************************************)
EXTENDS RNPathProbe

\* the client lists (a configuration file cannot hold a sequence)
ThreeStep == << 3, 2, 1 >>
OneEntry == << 3 >>

=============================================================================
