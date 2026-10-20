-------------------------- MODULE MCRNResendTiming --------------------------
(***************************************************************************)
(* The small instance of RNResendTiming: one tick is 100 ms, the update    *)
(* rounded up to the unit, and reliable_timeout is 10 s (100 ticks).       *)
(* MCRNResendTiming.cfg is the rule at the defaults: resend_interval 100   *)
(* ms (1), rto_initial 100 ms (1), rto_min 10 ms rounded up to one tick    *)
(* (1), rto_max 250 ms rounded up (3), no back-off.                        *)
(* MCRNResendTimingBrokenBackoff.cfg is the reversed witness, the first    *)
(* version's schedule: rto_initial 1000 ms (10), rto_min 50 ms rounded up  *)
(* (1), rto_max 2000 ms (20), no cap (Interval = RtoMax) and back-off to   *)
(* 8. It must fail AttemptsBeforeTimeout: a sample sets rto to 1.5 s at    *)
(* the first tick, and the sends at 0, 1.5 and 4.5 s are all that fit      *)
(* before the timeout at 10 s (the next is due at 10.5 s), three where     *)
(* four are owed; any rto above 10/7 s does the same. Both instances pass  *)
(* ConfigAccepted, so the validation rule alone does not keep the          *)
(* property; it needs the delay without back-off.                          *)
(***************************************************************************)
EXTENDS RNResendTiming
=============================================================================
