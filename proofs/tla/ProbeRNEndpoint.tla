------------------------- MODULE ProbeRNEndpoint -------------------------
\* Probe (not a case): the v1.1.1 model has no action that can falsify
\* NoEndBeforeT in its passing configuration.
EXTENDS MCRNEndpoint
ProbeTickNeverEnabled == \A e \in E : ~ENABLED Tick(e)
=============================================================================
