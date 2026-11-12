----------------------- MODULE MCRNGsoQueueBankLayout -----------------------
(* Model-checking harness for RNGsoQueueBankLayout: 2 queues x 3 banks x 3
   slots, 2 fill threads, a destination on each queue. Every switch is the
   code's in MCRNGsoQueueBankLayout.cfg (the control, two flushes) and in
   MCRNGsoQueueBankLayoutFlush3.cfg (the same at three flushes, a bench case
   over the per-case budget); each Broken cfg changes one. *)
EXTENDS RNGsoQueueBankLayout

MCQueueOf(d) == IF d = "a" THEN 0 ELSE 1

\* a pointer one byte before or after the slot start returned (negatives cannot be
\* written in a cfg)
MCSkew == {-1, 0, 1}
==============================================================================
