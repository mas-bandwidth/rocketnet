----------------------- MODULE MCRNGsoQueueBankLayout -----------------------
(* Model-checking harness for RNGsoQueueBankLayout: 2 queues x 3 banks x 3
   slots, 2 game threads, a destination on each queue (Dests = {0, 1}: d is
   on queue d % Q), a caller's pointer a byte off either way (SkewSpan = 1).
   Every switch is the code's in MCRNGsoQueueBankLayout.cfg (the control, two
   flushes) and in MCRNGsoQueueBankLayoutFlush3.cfg (the same at three
   flushes, a bench case over the per-case budget); each Broken cfg changes
   one. MCRNGsoQueueBankLayoutReachKeptProducer.cfg is the control's instance
   and must fail NoKeptProducer: a flush keeps a staged producer while a free
   bank waits (a contended try-acquire or a stopping transport), and the
   next frame's starts claim on from its count. *)
EXTENDS RNGsoQueueBankLayout

\* the reach predicate, negated: no queue has kept a staged producer at a
\* flush. A queue still on turn 0 after a flush, with claims in its producer
\* and no start yet in the new frame, kept that producer at the flush,
\* although two banks were free (no queue on turn 0 has handed one over)
NoKeptProducer ==
    ~ \E q \in Qs : /\ flushes >= 1
                    /\ turn[q] = 0
                    /\ cnt[q][prod[q]] > 0
                    /\ \A t \in Threads : demand[t] = 0
==============================================================================
