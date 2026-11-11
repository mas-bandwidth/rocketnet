------------------------- MODULE ProbeRNReceipts -------------------------
\* Probe (not a case): the v1.1.1 model has no action that can falsify
\* NoTimeoutEndWhenOff in its passing configuration.
EXTENDS MCRNReceipts
ProbeDisconnectNeverEnabled == ~ENABLED Disconnect
=============================================================================
