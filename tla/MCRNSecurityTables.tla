------------------------- MODULE MCRNSecurityTables -------------------------
(***************************************************************************)
(* The small instance of RNSecurityTables: two addresses, two tokens,      *)
(* one of them expiring at tick 3, tables at the production ratios         *)
(* reduced (history 2, admitted 2, two client slots), a verify budget      *)
(* of two per update.  MCRNSecurityTables.cfg is the fixed-code run;       *)
(* README-RNSecurityTables.md lists each reversed witness as edits to      *)
(* that file.                                                              *)
(***************************************************************************)
EXTENDS RNSecurityTables

MCExpire == [tk \in Tokens |-> IF tk = "te" THEN 3 ELSE 0]
MCCid == [tk \in Tokens |-> CASE tk = "te" -> 2 [] tk = "tf" -> 3 [] OTHER -> 1]
\* "th" and "te" are held at both addresses (one client that moved, or a
\* shared token); only "A" holds "tf", so "B" replaying it is the attacker.
MCHolds == [tk \in Tokens |-> IF tk = "tf" THEN {"A"} ELSE {"A", "B"}]
\* MCRNSecurityTablesBrokenGlobalLockout.cfg overrides RefuseUnremembered
\* with this: the full failure table refuses a source it cannot remember.
MCRefuseUnremembered == TRUE
\* MCRNSecurityTablesBrokenReleaseKeepsSlot.cfg overrides ReleaseToFree with
\* this: a release zeroes the claim's deadline and leaves its slot in place.
MCReleaseToFree == FALSE
\* MCRNSecurityTablesBrokenReleaseTwice.cfg overrides ReleaseChecksLive with
\* this: a release pushes the slot without unlinking it from its chain.
MCReleaseChecksLive == FALSE
\* MCRNSecurityTablesBrokenBackwardsWindow.cfg overrides BackwardsRollover
\* with this: the window rolls over only when the clock passes it.
MCNoBackwardsRollover == FALSE
=============================================================================
