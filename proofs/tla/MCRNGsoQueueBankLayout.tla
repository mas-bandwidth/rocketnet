----------------------- MODULE MCRNGsoQueueBankLayout -----------------------
(* Model-checking harness for RNGsoQueueBankLayout: 2 queues x 3 banks x 3
   slots, 2 fill threads, a destination on each queue (Dests = {0, 1}: d is
   on queue d % Q), a caller's pointer a byte off either way (SkewSpan = 1).
   Every switch is the code's in MCRNGsoQueueBankLayout.cfg (the control, two
   flushes) and in MCRNGsoQueueBankLayoutFlush3.cfg (the same at three
   flushes, a bench case over the per-case budget); each Broken cfg changes
   one. *)
EXTENDS RNGsoQueueBankLayout
==============================================================================
