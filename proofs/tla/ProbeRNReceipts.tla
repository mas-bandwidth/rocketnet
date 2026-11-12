------------------------- MODULE ProbeRNReceipts -------------------------
\* Probe (temporary, not a case): mutations that must make each revision 1.1
\* property of RNReceipts fail, applied by cfg overrides (Op <- ProbeOp).
EXTENDS MCRNReceipts

\* the time bound of NoEndBeforeSilenceT replaced by FALSE
ProbeBoundFalse == ~TimeoutConfigured => (~live => FALSE)

\* the silence end one tick late, and never
ProbeSilenceLate == SilenceT + 1
ProbeSilenceNever == SilenceT + 100
=============================================================================
