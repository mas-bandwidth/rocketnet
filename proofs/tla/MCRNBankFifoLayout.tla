------------------------ MODULE MCRNBankFifoLayout ------------------------
(***************************************************************************)
(* The small instances of RNBankFifoLayout.  Controls: MCRNBankFifoLayout  *)
(* (portable, gso), LoopFour (xdp exchange, buffer handoff), Handoff and   *)
(* HandoffMaxReadyAll (buffer handoff max_ready), LifoSend (iocp send),    *)
(* LifoExposed (iocp and rio receive), Posted (iocp client).  Reversed     *)
(* witnesses, each one rule switch:                                        *)
(*   BrokenFullOffByOne    (FreeSlack)          ExactlyOnePlace            *)
(*   BrokenNoTailMod       (TailMod 0)          InBounds                   *)
(*   BrokenWrapTail        (TailMod N - 1)      ReadyMatches               *)
(*   BrokenLiteralFour     (InitKind, N = 4)    ExactlyOnePlace            *)
(*   BrokenMixedFreeOrder  (PushKind, PopKind)  ExactlyOnePlace            *)
(*   BrokenPostedArrayMod  (TailMod ArrLen)     ReadyMatches               *)
(* README-RNBankFifoLayout.md gives each trace and its check against the  *)
(* code.                                                                   *)
(***************************************************************************)
EXTENDS RNBankFifoLayout
=============================================================================
